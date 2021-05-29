#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define RINGBUF_PAD(SIZE) (((size_t)(SIZE) + 7U) & (~7U))

typedef struct {
    uint32_t size, gap;
} ringbuf_element_t;

typedef struct {
    size_t size, mask, rsvd /* reserved */, gapd;
    memory_order acquire, release;
    atomic_size_t head, tail;
    uint8_t buf[] __attribute__((aligned(sizeof(ringbuf_element_t))));
} ringbuf_t;

static inline size_t ringbuf_body_size(size_t minimum)
{
    size_t size = 1;
    while (size < minimum)
        size <<= 1; /* assure size to be a power of 2 */
    return size;
}

static inline void ringbuf_init(ringbuf_t *ringbuf,
                                size_t body_size,
                                bool release_and_acquire)
{
    ringbuf->acquire =
        release_and_acquire ? memory_order_acquire : memory_order_relaxed;
    ringbuf->release =
        release_and_acquire ? memory_order_release : memory_order_relaxed;

    atomic_init(&ringbuf->head, 0);
    atomic_init(&ringbuf->tail, 0);

    ringbuf->size = body_size;
    ringbuf->mask = ringbuf->size - 1;
}

static inline ringbuf_t *ringbuf_new(size_t minimum, bool release_and_acquire)
{
    ringbuf_t *ringbuf = NULL;

    const size_t body_size = ringbuf_body_size(minimum);
    const size_t total_size = sizeof(ringbuf_t) + body_size;

    posix_memalign((void **) &ringbuf, sizeof(ringbuf_element_t), total_size);
    mlock(ringbuf, total_size); /* prevent memory from being flushed */

    if (ringbuf)
        ringbuf_init(ringbuf, body_size, release_and_acquire);

    return ringbuf;
}

static inline void ringbuf_free(ringbuf_t *ringbuf)
{
    if (!ringbuf)
        return;
    munlock(ringbuf->buf, ringbuf->size);
    free(ringbuf);
}

static inline void _ringbuf_write_advance_raw(ringbuf_t *ringbuf,
                                              size_t head,
                                              size_t written)
{
    /* only producer is allowed to advance write head */
    const size_t new_head = (head + written) & ringbuf->mask;
    atomic_store_explicit(&ringbuf->head, new_head, ringbuf->release);
}

static inline void *ringbuf_write_request_max(ringbuf_t *ringbuf,
                                              size_t minimum,
                                              size_t *maximum)
{
    assert(ringbuf);

    size_t space; /* size of writable buffer */
    size_t end;   /* virtual end of writable buffer */
    const size_t head = atomic_load_explicit(
        &ringbuf->head, memory_order_relaxed); /* read head */
    const size_t tail = atomic_load_explicit(
        &ringbuf->tail,
        ringbuf->acquire); /* read tail (consumer modifies it any time) */
    const size_t padded = 2 * sizeof(ringbuf_element_t) + RINGBUF_PAD(minimum);

    /* calculate writable space */
    if (head > tail)
        space = ((tail - head + ringbuf->size) & ringbuf->mask) - 1;
    else if (head < tail)
        space = (tail - head) - 1;
    else
        space = ringbuf->size - 1;
    end = head + space;

    /* available region wraps over at the end of buffer */
    if (end > ringbuf->size) {
        /* get first part of available buffer */
        uint8_t *buf1 = ringbuf->buf + head;
        const size_t len1 = ringbuf->size - head;

        /* not enough space left on first part of buffer */
        if (len1 < padded) {
            /* get second part of available buffer */
            uint8_t *buf2 = ringbuf->buf;
            const size_t len2 = end & ringbuf->mask;

            if (len2 < padded) { /* not enough space left on second buffer */
                ringbuf->rsvd = 0;
                ringbuf->gapd = 0;
                if (maximum)
                    *maximum = ringbuf->rsvd;
                return NULL;
            }
            /* enough space left on second buffer, use it! */
            ringbuf->rsvd = len2;
            ringbuf->gapd = len1;
            if (maximum)
                *maximum = ringbuf->rsvd;
            return buf2 + sizeof(ringbuf_element_t);
        }

        /* enough space left on first part of buffer, use it! */
        ringbuf->rsvd = len1;
        ringbuf->gapd = 0;
        if (maximum)
            *maximum = ringbuf->rsvd;
        return buf1 + sizeof(ringbuf_element_t);
    }

    /* available region is contiguous */
    uint8_t *buf = ringbuf->buf + head;
    if (space < padded) { /* no space left on contiguous buffer */
        ringbuf->rsvd = 0;
        ringbuf->gapd = 0;
        if (maximum)
            *maximum = ringbuf->rsvd;
        return NULL;
    }

    /* enough space left on contiguous buffer, use it! */
    ringbuf->rsvd = space;
    ringbuf->gapd = 0;
    if (maximum)
        *maximum = ringbuf->rsvd;
    return buf + sizeof(ringbuf_element_t);
}

static inline void ringbuf_write_advance(ringbuf_t *ringbuf, size_t written)
{
    assert(ringbuf);
    /* fail miserably if someone tries to write more than rsvd */
    assert(written <= ringbuf->rsvd);

    /* write element header at head */
    const size_t head =
        atomic_load_explicit(&ringbuf->head, memory_order_relaxed);
    if (ringbuf->gapd > 0) {
        /* fill end of first buffer with gap */
        ringbuf_element_t *element =
            (ringbuf_element_t *) (ringbuf->buf + head);
        element->size = ringbuf->gapd - sizeof(ringbuf_element_t);
        element->gap = 1;

        /* fill written element header */
        element = (void *) ringbuf->buf;
        element->size = written;
        element->gap = 0;
    } else {
        /* fill written element header */
        ringbuf_element_t *element =
            (ringbuf_element_t *) (ringbuf->buf + head);
        element->size = written;
        element->gap = 0;
    }

    /* advance write head */
    _ringbuf_write_advance_raw(
        ringbuf, head,
        ringbuf->gapd + sizeof(ringbuf_element_t) + RINGBUF_PAD(written));
}

static inline void _ringbuf_read_advance_raw(ringbuf_t *ringbuf,
                                             size_t tail,
                                             size_t read)
{
    /* only consumer is allowed to advance read tail */
    const size_t new_tail = (tail + read) & ringbuf->mask;
    atomic_store_explicit(&ringbuf->tail, new_tail, ringbuf->release);
}

static inline const void *ringbuf_read_request(ringbuf_t *ringbuf,
                                               size_t *toread)
{
    assert(ringbuf);
    size_t space; /* size of available buffer */
    const size_t tail = atomic_load_explicit(
        &ringbuf->tail, memory_order_relaxed);  // read tail
    const size_t head = atomic_load_explicit(
        &ringbuf->head,
        ringbuf->acquire); /* read head (producer modifies it any time) */

    // calculate readable space
    if (head > tail)
        space = head - tail;
    else
        space = (head - tail + ringbuf->size) & ringbuf->mask;

    if (space > 0) { /* there may be chunks available for reading */
        const size_t end = tail + space; /* virtual end of available buffer */

        if (end > ringbuf->size) { /* available buffer wraps around at end */
            /* first part of available buffer */
            const uint8_t *buf1 = ringbuf->buf + tail;
            const size_t len1 = ringbuf->size - tail;
            const ringbuf_element_t *element = (const ringbuf_element_t *) buf1;

            if (element->gap) { /* gap element? */
                /* skip gap */
                _ringbuf_read_advance_raw(ringbuf, tail, len1);

                /* second part of available buffer */
                const uint8_t *buf2 = ringbuf->buf;
                /* there will always be at least on element after a gap */
                element = (const ringbuf_element_t *) buf2;

                *toread = element->size;
                return buf2 + sizeof(ringbuf_element_t);
            }

            /* valid chunk, use it! */
            *toread = element->size;
            return buf1 + sizeof(ringbuf_element_t);
        }

        /* available buffer is contiguous */
        const uint8_t *buf = ringbuf->buf + tail;
        const ringbuf_element_t *element = (const ringbuf_element_t *) buf;
        *toread = element->size;
        return buf + sizeof(ringbuf_element_t);
    }

    /* no chunks available. i.e., empty buffer */
    *toread = 0;
    return NULL;
}

static inline void ringbuf_read_advance(ringbuf_t *ringbuf)
{
    assert(ringbuf);
    /* get element header from tail (for size) */
    const size_t tail =
        atomic_load_explicit(&ringbuf->tail, memory_order_relaxed);
    const ringbuf_element_t *element =
        (const ringbuf_element_t *) (ringbuf->buf + tail);

    /* advance read tail */
    _ringbuf_read_advance_raw(
        ringbuf, tail, sizeof(ringbuf_element_t) + RINGBUF_PAD(element->size));
}

/* Test program */

static const struct timespec req = {.tv_sec = 0, .tv_nsec = 1};

static uint64_t iterations = 10000;
#define THRESHOLD (RAND_MAX / 256)
#define PAD(SIZE) (((size_t)(SIZE) + 7U) & (~7U))

static void *producer_main(void *arg)
{
    ringbuf_t *ringbuf = arg;

    uint64_t cnt = 0;
    while (cnt < iterations) {
        if (rand() < THRESHOLD)
            nanosleep(&req, NULL);

        size_t written = PAD(rand() * 1024.f / RAND_MAX);

        size_t maximum;
        uint8_t *ptr;
        if ((ptr = ringbuf_write_request_max(ringbuf, written, &maximum))) {
            assert(maximum >= written);
            const uint8_t *end = ptr + written;
            for (uint8_t *src = ptr; src < end; src += sizeof(uint64_t)) {
                *(uint64_t *) src = cnt;
                assert(*(uint64_t *) src == cnt);
            }
            ringbuf_write_advance(ringbuf, written);
            cnt++;
        } /* else: buffer full */
    }

    return NULL;
}

static void *consumer_main(void *arg)
{
    ringbuf_t *ringbuf = arg;

    uint64_t cnt = 0;
    while (cnt < iterations) {
        if (rand() < THRESHOLD)
            nanosleep(&req, NULL);

        const uint8_t *ptr;
        size_t toread;
        if ((ptr = ringbuf_read_request(ringbuf, &toread))) {
            const uint8_t *end = ptr + toread;
            for (const uint8_t *src = ptr; src < end; src += sizeof(uint64_t))
                assert(*(const uint64_t *) src == cnt);
            ringbuf_read_advance(ringbuf);
            cnt++;
        } /* else: buffer empty */
    }

    return NULL;
}

static void test_threaded()
{
    pthread_t producer, consumer;
    ringbuf_t *ringbuf = ringbuf_new(8192, true);
    assert(ringbuf);

    pthread_create(&consumer, NULL, consumer_main, ringbuf);
    pthread_create(&producer, NULL, producer_main, ringbuf);

    pthread_join(producer, NULL);
    pthread_join(consumer, NULL);

    ringbuf_free(ringbuf);
}

typedef struct _ringbuf_shm_t ringbuf_shm_t;

struct _ringbuf_shm_t {
    char *name;
    int fd;
    ringbuf_t *ringbuf;
};

static int ringbuf_shm_init(ringbuf_shm_t *ringbuf_shm,
                            const char *name,
                            size_t minimum,
                            bool release_and_acquire)
{
    const size_t body_size = ringbuf_body_size(minimum);
    const size_t total_size = sizeof(ringbuf_t) + body_size;

    ringbuf_shm->name = strdup(name);
    if (!ringbuf_shm->name)
        return -1;

    bool is_first = true;
    ringbuf_shm->fd = shm_open(ringbuf_shm->name, O_RDWR | O_CREAT | O_EXCL,
                               S_IRUSR | S_IWUSR);
    if (ringbuf_shm->fd == -1) {
        is_first = false;
        ringbuf_shm->fd =
            shm_open(ringbuf_shm->name, O_RDWR, S_IRUSR | S_IWUSR);
    }
    if (ringbuf_shm->fd == -1) {
        free(ringbuf_shm->name);
        return -1;
    }

    if ((ftruncate(ringbuf_shm->fd, total_size) == -1) ||
        ((ringbuf_shm->ringbuf = mmap(NULL, total_size, PROT_READ | PROT_WRITE,
                                      MAP_SHARED, ringbuf_shm->fd, 0)) ==
         MAP_FAILED)) {
        shm_unlink(ringbuf_shm->name);
        close(ringbuf_shm->fd);
        free(ringbuf_shm->name);
        return -1;
    }

    if (is_first)
        ringbuf_init(ringbuf_shm->ringbuf, body_size, release_and_acquire);

    return 0;
}

static void ringbuf_shm_deinit(ringbuf_shm_t *ringbuf_shm)
{
    const size_t total_size = sizeof(ringbuf_t) + ringbuf_shm->ringbuf->size;

    munmap(ringbuf_shm->ringbuf, total_size);
    shm_unlink(ringbuf_shm->name);
    close(ringbuf_shm->fd);
    free(ringbuf_shm->name);
}

static void test_shared()
{
    pid_t pid = fork();
    assert(pid != -1);

    const char *name = "/ringbuf_shm_test";
    if (pid == 0) { /* child process */
        ringbuf_shm_t ringbuf_shm;
        assert(ringbuf_shm_init(&ringbuf_shm, name, 8192, true) == 0);

        consumer_main(ringbuf_shm.ringbuf);

        ringbuf_shm_deinit(&ringbuf_shm);
    } else { /* parent process */
        ringbuf_shm_t ringbuf_shm;
        assert(ringbuf_shm_init(&ringbuf_shm, name, 8192, true) == 0);

        producer_main(ringbuf_shm.ringbuf);

        ringbuf_shm_deinit(&ringbuf_shm);
    }
}

int main(int argc, char **argv)
{
    srand(time(NULL));

    test_threaded();
    test_shared();

    return 0;
}
