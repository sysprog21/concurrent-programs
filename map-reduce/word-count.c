/* Word cache configs */
#define MAX_WORD_SIZE 32
#define MAX_N_WORDS 8192

#include <stddef.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

struct hlist_head {
    struct hlist_node *first;
};

struct hlist_node {
    struct hlist_node *next, **pprev;
};

#define HLIST_HEAD_INIT \
    {                   \
        .first = NULL   \
    }
#define HLIST_HEAD(name) struct hlist_head name = {.first = NULL}
#define INIT_HLIST_HEAD(ptr) ((ptr)->first = NULL)

static inline void INIT_HLIST_NODE(struct hlist_node *h)
{
    h->next = NULL;
    h->pprev = NULL;
}

static inline int hlist_empty(const struct hlist_head *h)
{
    return !h->first;
}

static inline void hlist_add_head(struct hlist_node *n, struct hlist_head *h)
{
    struct hlist_node *first = h->first;
    n->next = first;
    if (first)
        first->pprev = &n->next;
    h->first = n;
    n->pprev = &h->first;
}

#include <stdbool.h>

static inline bool hlist_is_singular_node(struct hlist_node *n,
                                          struct hlist_head *h)
{
    return !n->next && n->pprev == &h->first;
}

#define container_of(list_ptr, container_type, member_name)               \
    ({                                                                    \
        const typeof(((container_type *) 0)->member_name) *__member_ptr = \
            (list_ptr);                                                   \
        (container_type *) ((char *) __member_ptr -                       \
                            offsetof(container_type, member_name));       \
    })

#define hlist_entry(ptr, type, member) container_of(ptr, type, member)

#define hlist_first_entry(head, type, member) \
    hlist_entry((head)->first, type, member)

#define hlist_for_each(pos, head) \
    for (pos = (head)->first; pos; pos = pos->next)

#define hlist_entry_safe(ptr, type, member)                  \
    ({                                                       \
        typeof(ptr) ____ptr = (ptr);                         \
        ____ptr ? hlist_entry(____ptr, type, member) : NULL; \
    })

#define hlist_for_each_entry(pos, head, member)                              \
    for (pos = hlist_entry_safe((head)->first, typeof(*(pos)), member); pos; \
         pos = hlist_entry_safe((pos)->member.next, typeof(*(pos)), member))

static inline void __hash_init(struct hlist_head *ht, unsigned int sz)
{
    for (unsigned int i = 0; i < sz; i++)
        INIT_HLIST_HEAD(&ht[i]);
}

typedef uint32_t hash_t;

/* A node of the table */
struct ht_node {
    hash_t hash;
    struct hlist_node list;
};

/* user-defined functions */
typedef int hashfunc_t(void *key, hash_t *hash, uint32_t *bkt);
typedef int cmp_t(struct ht_node *n, void *key);

/* hash table */
struct htable {
    hashfunc_t *hashfunc;
    cmp_t *cmp;
    uint32_t n_buckets;
    struct hlist_head *buckets;
};

/* Initializes a hash table */
static inline int ht_init(struct htable *h,
                          hashfunc_t *hashfunc,
                          cmp_t *cmp,
                          uint32_t n_buckets)
{
    h->hashfunc = hashfunc, h->cmp = cmp;
    h->n_buckets = n_buckets;
    h->buckets = malloc(sizeof(struct hlist_head) * n_buckets);
    __hash_init(h->buckets, h->n_buckets);
    return 0;
}

/* destroys ressource called by ht_init */
static inline int ht_destroy(struct htable *h)
{
    free(h->buckets);
    return 0;
}

/* Find an element with the key in the hash table.
 * Return the element if success.
 */
static inline struct ht_node *ht_find(struct htable *h, void *key)
{
    hash_t hval;
    uint32_t bkt;
    h->hashfunc(key, &hval, &bkt);

    struct hlist_head *head = &h->buckets[bkt];
    struct ht_node *n;
    hlist_for_each_entry (n, head, list) {
        if (n->hash == hval) {
            int res = h->cmp(n, key);
            if (!res)
                return n;
            if (res > 0)
                return NULL;
        }
    }
    return NULL;
}

/* Insert a new element with the key 'key' in the htable.
 * Return 0 if success.
 */
#include <stdio.h>

static inline int ht_insert(struct htable *h, struct ht_node *n, void *key)
{
    hash_t hval;
    uint32_t bkt;
    h->hashfunc(key, &hval, &bkt);
    n->hash = hval;

    struct hlist_head *head = &h->buckets[bkt];
    struct hlist_node *iter;
    hlist_for_each(iter, head)
    {
        struct ht_node *tmp = hlist_entry(iter, struct ht_node, list);
        if (tmp->hash >= hval) {
            int cmp = h->cmp(tmp, key);
            if (!cmp) /* already exist */
                return -1;
            if (cmp > 0) {
                hlist_add_head(&n->list, head);
                return 0;
            }
        }
    }

    hlist_add_head(&n->list, head);
    return 0;
}

static inline struct ht_node *ht_get_first(struct htable *h, uint32_t bucket)
{
    struct hlist_head *head = &h->buckets[bucket];
    if (hlist_empty(head))
        return NULL;
    return hlist_first_entry(head, struct ht_node, list);
}

static inline struct hlist_node *hlist_next(struct hlist_head *root,
                                            struct hlist_node *current)
{
    if ((hlist_empty(root)) || hlist_is_singular_node(current, root) ||
        !current)
        return NULL;
    return current->next;
}

static inline struct ht_node *ht_get_next(struct htable *h,
                                          uint32_t bucket,
                                          struct ht_node *n)
{
    struct hlist_node *ln = hlist_next(&h->buckets[bucket], &n->list);
    if (!ln)
        return NULL;
    return hlist_entry(ln, struct ht_node, list);
}

/* cache of words. Count the number of word using a modified hash table */
struct wc_cache {
    struct htable htable;
};

struct wc_word {
    char word[MAX_WORD_SIZE], *full_word;
    uint32_t counter;
    struct ht_node node, node_main;
};

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* TODO: handle '-' character (hyphen) */
/* TODO: add number support */
/* FIXME: remove the assumptions on ASCII encoding */

static uint32_t n_buckets;
static struct wc_cache main_cache, *thread_caches;

#define FIRST_LOWER_LETTER 'a'
#define N_LETTERS (('z' - 'a') + 1)
#define MIN_N_BUCKETS N_LETTERS

#define GET_WORD(w) (w->full_word ? w->full_word : w->word)

#define MIN_MAX(a, b, op)       \
    ({                          \
        __typeof__(a) _a = (a); \
        __typeof__(b) _b = (b); \
        _a op _b ? _a : _b;     \
    })

#define MAX(a, b) MIN_MAX(a, b, >)
#define MIN(a, b) MIN_MAX(a, b, <)

/* Called to compare word if their hash value is similar */
static inline int __wc_cmp(struct ht_node *n, void *key, char m)
{
    struct wc_word *w = m ? container_of(n, struct wc_word, node_main)
                          : container_of(n, struct wc_word, node);
    return strcasecmp(GET_WORD(w), (char *) key);
}

static int wc_cmp(struct ht_node *n, void *key)
{
    return __wc_cmp(n, key, 0);
}

static int wc_cmp_main(struct ht_node *n, void *key)
{
    return __wc_cmp(n, key, 1);
}

static uint32_t code_min, code_max, code_range;

static uint32_t get_code(char *word)
{
    uint32_t code = 0;
    /* The hash value is a concatenation of the letters */
    char shift =
        (char) (sizeof(unsigned int) * CHAR_BIT - __builtin_clz(N_LETTERS));

    for (int i = ((sizeof(code) * 8) / shift) - 1; i >= 0 && *word; i--) {
        char c = tolower(*(word)) - FIRST_LOWER_LETTER;
        code |= ((uint32_t) c) << (i * shift);
        word++;
    }
    return code;
}

static inline int scale_range_init()
{
    code_min = get_code("a"), code_max = get_code("zzzzzzzzzz");
    code_range = (code_max - code_min);
    return 0;
}

static inline uint32_t scale_range(uint32_t code)
{
    return (uint32_t)((((double) code - code_min) * n_buckets) / code_range);
}

/* Must keep an an alphabetic order when assiging buckets. */
static int hash_bucket(void *key, hash_t *hash, uint32_t *bkt)
{
    uint32_t code = get_code((char *) key);
    *hash = (hash_t) code, *bkt = scale_range(code);
    return 0;
}

/* Initialize each (worker+main) cache */
int wc_init(uint32_t n_threads, uint32_t n_words)
{
    /* FIXME: a resizable hash table would be better */
    n_buckets = MAX(MIN(n_words, MAX_N_WORDS), MIN_N_BUCKETS);
    thread_caches = malloc(sizeof(struct wc_cache) * n_threads);

    scale_range_init();

    for (size_t i = 0; i < n_threads; i++) {
        if (ht_init(&thread_caches[i].htable, hash_bucket, wc_cmp, n_buckets))
            return -1;
    }

    if (ht_init(&main_cache.htable, hash_bucket, wc_cmp_main, n_buckets))
        return -1;

    return 0;
}

/* Copy while setting to lower case */
static char *wc_strncpy(char *dest, char *src, size_t n)
{
    /* case insensitive */
    for (size_t i = 0; i < n && src[i] != '\0'; i++)
        dest[i] = (char) tolower((int) (src[i]));
    return dest;
}

/* Add a word to the cache of the thread tid. If the word already exists,
 * increment its counter. Otherwise, add a new word.
 */
int wc_add_word(uint32_t tid, char *word, uint32_t count)
{
    struct wc_word *w;
    struct wc_cache *cache = &thread_caches[tid];
    struct ht_node *n;
    if (!(n = ht_find(&cache->htable, word))) {
        /* word was absent. Allocate a new wc_word */
        if (!(w = calloc(1, sizeof(struct wc_word))))
            return -1;

        if (count > (MAX_WORD_SIZE - 1))
            w->full_word = calloc(1, count + 1);

        wc_strncpy(GET_WORD(w), word, count);

        /* Add the new world to the table */
        ht_insert(&cache->htable, &w->node, word);
    } else
        w = container_of(n, struct wc_word, node);

    w->counter++;
    return 0;
}

static int __merge_results(uint32_t tid, uint32_t j, struct wc_cache *wcc)
{
    struct wc_cache *cache = &main_cache;

    struct ht_node *iter = ht_get_first(&wcc->htable, j);
    for (; iter; iter = ht_get_next(&wcc->htable, j, iter)) {
        struct wc_word *iw = container_of(iter, struct wc_word, node);

        /* check if word already exists in main_cache */
        char *wd = GET_WORD(iw);
        struct ht_node *n;
        if ((n = ht_find(&cache->htable, wd))) {
            /* word already exists. Get the word and increment it */
            struct wc_word *w = container_of(n, struct wc_word, node_main);
            w->counter += iw->counter;
        } else /* if word does not exist, then insert the new word */
            ht_insert(&cache->htable, &iw->node_main, wd);
    }
    return 0;
}

/* Merge the results of all threads to the main cache.
 * This Merge is done in parralel by all threads.
 * Each thread handles at least n_buckets/n_threads buckets.
 */
int wc_merge_results(uint32_t tid, uint32_t n_threads)
{
    uint32_t n_workers;
    /* Keep the number of workers <= nbthread */
    if (n_threads > n_buckets) {
        if (tid > n_buckets - 1)
            return 0;
        n_workers = n_buckets;
    } else
        n_workers = n_threads;

    /* Each thread will treat at least wk_buckets */
    uint32_t wk_buckets = n_buckets / n_workers;

    /* The range that this thread will treat */
    uint32_t wk_bstart = wk_buckets * tid, wk_bend = wk_bstart + wk_buckets;

    /* last thread must also do last buckets */
    if ((tid == (n_workers - 1)))
        wk_bend += n_buckets % n_workers;

    for (size_t i = 0; i < n_threads; i++) {
        struct wc_cache *cache = &thread_caches[i];
        for (size_t j = wk_bstart; j < wk_bend; j++) {
            /* Traverse the buckets of all threads from wk_bstart to wk_bend.
             * Aggregate the nodes of theses buckets in the main_cache.
             */
            __merge_results(tid, j, cache);
        }
    }
    return 0;
}

/* Print the merged results */
int wc_print(int id)
{
    uint32_t total = 0, count_total = 0, bkt_total = 0, empty_bkt = 0;
    int valid = (id == -1);
    struct wc_cache *cache = valid ? &main_cache : &thread_caches[id];

    for (size_t j = 0; j < n_buckets; j++) {
        struct ht_node *iter = ht_get_first(&cache->htable, j);
        for (; iter; iter = ht_get_next(&cache->htable, j, iter)) {
            struct wc_word *w =
                valid ? container_of(iter, struct wc_word, node_main)
                      : container_of(iter, struct wc_word, node);
            printf("%s : %d\n", GET_WORD(w), w->counter);
            bkt_total++, total++;
            count_total += w->counter;
        }
        if (!bkt_total)
            empty_bkt++;
        bkt_total = 0;
    }
    printf("Words: %d, word counts: %d, full buckets: %d (ideal %d)\n", total,
           count_total, n_buckets - empty_bkt, MIN(total, n_buckets));
    return 0;
}

static int __wc_destroy(struct wc_cache *wcc, int id)
{
    int valid = (id == -1);
    for (uint32_t j = 0; j < n_buckets; j++) {
        struct ht_node *iter = ht_get_first(&wcc->htable, j);
        struct ht_node *tmp = ht_get_next(&wcc->htable, j, iter);
        for (; tmp; iter = tmp, tmp = ht_get_next(&wcc->htable, j, tmp)) {
            struct wc_word *w =
                valid ? container_of(iter, struct wc_word, node_main)
                      : container_of(iter, struct wc_word, node);

            free(w->full_word);
            free(w);
        }
    }
    return 0;
}

/* Destroy ressource allocated by wc_init */
/* Free nodes and htable */
int wc_destroy(uint32_t n_threads)
{
    for (size_t i = 0; i < n_threads; i++) {
        if (__wc_destroy(&thread_caches[i], i))
            return -1;
        if (ht_destroy(&thread_caches[i].htable))
            return -1;
    }
    free(thread_caches);

    if (ht_destroy(&main_cache.htable))
        return -1;
    return 0;
}

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* I/O operation configs */
#ifndef BUFFER_SIZE
#define BUFFER_SIZE 4096
#endif

static int fd;
static off_t file_size;
static void *file_content;

static __thread char *worker_buffer;

#if defined(__linux__)
#define MMAP_FLAGS (MAP_POPULATE | MAP_PRIVATE)
#else
#define MMAP_FLAGS (MAP_PRIVATE)
#endif

/* Initialize file access for worker threads.
 * Should be called by main thread. Return 0 on success.
 */
static inline int fa_init(char *file, uint32_t n_threads, off_t *fsz)
{
    /* Opening file */
    if ((fd = open(file, O_RDONLY)) < 0) {
        perror("open");
        return -1;
    }

    /* Get file size */
    if ((file_size = lseek(fd, 0, SEEK_END)) < 0) {
        perror("lseek");
        return -1;
    }

    file_content = mmap(NULL, file_size, PROT_READ, MMAP_FLAGS, fd, 0);
    if (file_content == MAP_FAILED)
        file_content = NULL;

    *fsz = file_size;
    return 0;
}

/* Initialize file read access.
 * Should be called by worker threads. Return 0 on success.
 */
static inline int fa_read_init()
{
    if (!file_content && !(worker_buffer = malloc(BUFFER_SIZE)))
        return -1;
    return 0;
}

/* destroy fa_read_init allocated ressource */
static inline int fa_read_destroy()
{
    free(worker_buffer);
    return 0;
}

/* Read worker part of the file. Should be called by worker threads. */
static inline off_t fa_read(uint32_t id, char **buff, off_t size, off_t pos)
{
    off_t size_read;
    if (file_content) {
        if (pos >= file_size) /* EOF */
            return 0;

        *buff = (char *) file_content + pos;

        off_t end = pos + size;
        size_read = (end > file_size) ? (end - file_size) : size;
        return size_read;
    }

    off_t size_to_read = BUFFER_SIZE < size ? BUFFER_SIZE : size;
    if ((size_read = pread(fd, worker_buffer, size_to_read, pos)) == -1) {
        perror("pread");
        return -1;
    }

    *buff = worker_buffer;
    return size_read;
}

#include <pthread.h>

char *file_name;
uint32_t n_threads;

struct thread_info {
    pthread_t thread_id; /* ID returned by pthread_create() */
    int thread_num;      /* Application-defined thread # */
};

#define BETWEEN(_wd, _min, _max) ((_wd >= _min) && (_wd <= _max))
#define IS_LETTER(c) (BETWEEN(*buff, 'A', 'Z') || BETWEEN(*buff, 'a', 'z'))

static off_t file_size;
static pthread_barrier_t barrier;

int mr_init(void)
{
    if (pthread_barrier_init(&barrier, NULL, n_threads)) {
        perror("barrier init");
        return -1;
    }

    if (fa_init(file_name, n_threads, &file_size))
        return -1;

    if (wc_init(n_threads, file_size / MAX_WORD_SIZE))
        return -1;

    return 0;
}

int mr_destroy(void)
{
    if (pthread_barrier_destroy(&barrier)) {
        perror("barrier init");
        return -1;
    }

    if (fa_init(file_name, n_threads, &file_size))
        return -1;

    if (wc_destroy(n_threads))
        return -1;

    return 0;
}

int mr_reduce(void)
{
    /* The merge is done by worker threads */
    return 0;
}

int mr_print(void)
{
    return wc_print(-1);
}

static __thread off_t worker_slice, worker_current;

static __thread uint32_t count = 0, wsize = 0;
static __thread char *word = NULL;

/* The next three funcitons handle a buffer of the file.
 * Note that a buffer may end in the middle of word.
 * The word will be completed on the next call to the func.
 */

static int add_letter(char c)
{
    if ((count > wsize - 1) || !wsize) {
        wsize += MAX_WORD_SIZE;
        char *orig = word;
        if (!(word = realloc(word, wsize))) {
            free(orig);
            return -1;
        }
    }

    word[count++] = c;
    return 0;
}

static inline int add_sep(uint32_t tid)
{
    if (count) {
        word[count] = '\0'; /* Add current word */
        if (wc_add_word(tid, word, count))
            return -1;
        count = 0;
    }
    return 0;
}

static int buff_proceed(uint32_t tid, char *buff, size_t size, char last)
{
    for (; size; size--, buff++) {
        if (!IS_LETTER(*buff)) {
            if (add_sep(tid)) /* Not a letter */
                return -1;
            continue;
        }
        if (add_letter(*buff)) /* Is a letter */
            return -1;
    }

    if (last) /* If this is the last buffer, end the word (if any) */
        add_sep(tid);

    return 0;
}

/* Configure the buffer slices of each worker */
static int buff_init(uint32_t tid)
{
    if (fa_read_init())
        return -1;

    worker_slice = file_size / n_threads;
    worker_current = worker_slice * tid;

    /* Last thread handle remaining bytes */
    if (tid == (n_threads - 1))
        worker_slice += file_size % n_threads;

    off_t worker_end = worker_current + worker_slice;

    /* Balance worker_slice to include words at the ends.
     * skip first letters if we are not the first thread.
     */
    char *buff;
    do {
        if (tid == 0)
            break;
        if (fa_read(tid, &buff, 1, worker_current) != 1)
            return -1;
        if (!IS_LETTER(*buff))
            break;
        worker_current++;
        worker_slice--;
    } while (*buff);

    /* add letters of the last word if we are not the last thread */
    do {
        if (tid == (n_threads - 1))
            break;
        if (fa_read(tid, &buff, 1, worker_end) != 1)
            return -1;
        if (!IS_LETTER(*buff))
            break;
        worker_end++, worker_slice++;
    } while (*buff);

    return 0;
}

static int buff_destroy()
{
    free(word);
    if (fa_read_destroy())
        return -1;
    return 0;
}

/* Read a buffer from the file */
static int buff_read(uint32_t tid, char **buff, off_t *size, char *last)
{
    if (!worker_slice)
        return 0;

    off_t size_read = fa_read(tid, buff, worker_slice, worker_current);
    if (size_read == -1)
        return -1;

    *size = size_read;
    worker_current += size_read, worker_slice -= size_read;
    if (!worker_slice)
        *last = 1;
    return 0;
}

void *mr_map(void *id)
{
    uint32_t tid = ((struct thread_info *) id)->thread_num;
    int ret = buff_init(tid);
    if (ret)
        goto bail;

    char *buff;
    off_t size = 0;
    char last = 0;
    while (!(ret = buff_read(tid, &buff, &size, &last))) {
        if (!size)
            break;
        if (buff_proceed(tid, buff, size, last))
            goto bail;
        if (last) /* If this was the last buffer */
            break;
    }

    if (buff_destroy())
        goto bail;

    /* wait for other worker before merging */
    if (pthread_barrier_wait(&barrier) > 0) {
        perror("barrier wait");
        goto bail;
    }

    /* merge results (done in parrallel) */
    ret = wc_merge_results(tid, n_threads);
bail:
    return ((void *) (long) ret);
}

#include <sys/time.h>

static struct thread_info *tinfo;

#define throw_err(msg)      \
    do {                    \
        perror(msg);        \
        exit(EXIT_FAILURE); \
    } while (0)

static int parse_args(int argc, char **argv)
{
    if (argc < 3)
        return -1;

    file_name = argv[1];
    if (!file_name)
        return -1;

    n_threads = atoi(argv[2]);
    if (!n_threads)
        return -1;
    return 0;
}

static void run_threads(void)
{
    if (!(tinfo = calloc(n_threads, sizeof(struct thread_info))))
        throw_err("calloc");

    for (size_t i = 0; i < n_threads; i++) {
        tinfo[i].thread_num = i;
        if (pthread_create(&tinfo[i].thread_id, NULL, mr_map, &tinfo[i]))
            throw_err("thread create");
    }
}

static void wait_threads()
{
    for (size_t i = 0; i < n_threads; i++)
        if (pthread_join(tinfo[i].thread_id, NULL))
            throw_err("thread join");
    free(tinfo);
}

static double now()
{
    struct timeval tp;
    if (gettimeofday(&tp, (struct timezone *) NULL) == -1)
        perror("gettimeofday");
    return ((double) (tp.tv_sec) * 1000.0) + (((double) tp.tv_usec / 1000.0));
}

int main(int argc, char **argv)
{
    if (-1 == parse_args(argc, argv)) {
        printf("ERROR: Wrong arguments\n");
        printf("usage: %s FILE_NAME THREAD_NUMBER\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    double start = now();
    if (mr_init())
        exit(EXIT_FAILURE);

    run_threads();
    wait_threads();

    if (mr_reduce())
        exit(EXIT_FAILURE);

    /* Done here, to avoid counting the printing */
    double end = now();

    if (mr_print())
        exit(EXIT_FAILURE);
    if (mr_destroy())
        exit(EXIT_FAILURE);

    printf("Done in %g msec\n", end - start);

    exit(EXIT_SUCCESS);
}
