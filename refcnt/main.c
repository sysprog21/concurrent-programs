/*
 * Generic reference counting in C.
 *
 * Use refcnt_malloc/refcnt_unref in instead of malloc/free. Use refcnt_ref to
 * duplicate a reference as necessary. Use refcnt unref to stop using a pointer.
 *
 * The resulting string must be released using refcnt_unref since refcnt_strdup
 * function utilizes refcnt_malloc.
 *
 * This implementation is thread-safe with the exception of refcnt_realloc. If
 * you need to use refcnt_realloc in a multi-threaded environment, you must
 * synchronize access to the reference.
 *
 * If you define REFCNT_CHECK, references passed into refcnt_ref and and
 * refcnt_unref will be checked that they were created by refcnt_malloc. This is
 * useful for debugging, but it will slow down your program somewhat.
 *
 * If you define REFCNT_TRACE, refcnt_malloc and refcnt_unref will print
 * the line number and file name where they were called. This is useful for
 * debugging memory leaks or use after free errors.
 */

#include <assert.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define REFCNT_CHECK

#define maybe_unused __attribute__((unused))

typedef struct {
#ifdef REFCNT_CHECK
    int magic;
#endif
    atomic_uint refcount;
    char data[];
} refcnt_t;

#ifdef REFCNT_TRACE
#define _REFCNT_TRACE(call)                                                \
    ({                                                                     \
        fprintf(stderr, "%s:%d:(%s) %s", __FILE__, __LINE__, __FUNCTION__, \
                #call);                                                    \
        call;                                                              \
    })
#define refcnt_malloc refcnt_t_malloc
#define refcnt_realloc refcnt_t_realloc
#define refcnt_ref refcnt_t_ref
#define refcnt_unref refcnt_t_unref
#define refcnt_strdup refcnt_t_strdup
#endif

#define REFCNT_MAGIC 0xDEADBEEF

static maybe_unused void *refcnt_malloc(size_t len)
{
    refcnt_t *ref = malloc(sizeof(refcnt_t) + len);
    if (!ref)
        return NULL;
#ifdef REFCNT_CHECK
    ref->magic = REFCNT_MAGIC;
#endif
    atomic_init(&ref->refcount, 1);
    return ref->data;
}

static maybe_unused void *refcnt_realloc(void *ptr, size_t len)
{
    refcnt_t *ref = (void *) (ptr - offsetof(refcnt_t, data));
#ifdef REFCNT_CHECK
    assert(ref->magic == REFCNT_MAGIC);
#endif
    ref = realloc(ref, sizeof(refcnt_t) + len);
    if (!ref)
        return NULL;
    return ref->data;
}

static maybe_unused void *refcnt_ref(void *ptr)
{
    refcnt_t *ref = (void *) (ptr - offsetof(refcnt_t, data));
#ifdef REFCNT_CHECK
    assert(ref->magic == REFCNT_MAGIC && "Invalid refcnt pointer");
#endif
    atomic_fetch_add(&ref->refcount, 1);
    return ref->data;
}

static maybe_unused void refcnt_unref(void *ptr)
{
    refcnt_t *ref = (void *) (ptr - offsetof(refcnt_t, data));
#ifdef REFCNT_CHECK
    assert(ref->magic == REFCNT_MAGIC && "Invalid refcnt pointer");
#endif
    if (atomic_fetch_sub(&ref->refcount, 1) == 1)
        free(ref);
}

static maybe_unused char *refcnt_strdup(char *str)
{
    refcnt_t *ref = malloc(sizeof(refcnt_t) + strlen(str) + 1);
    if (!ref)
        return NULL;
#ifdef REFCNT_CHECK
    ref->magic = REFCNT_MAGIC;
#endif
    atomic_init(&ref->refcount, 1);
    strcpy(ref->data, str);
    return ref->data;
}

#ifdef REFCNT_TRACE
#undef refcnt_malloc
#undef refcnt_realloc
#undef refcnt_ref
#undef refcnt_unref
#undef refcnt_strdup
#define refcnt_malloc(len) _REFCNT_TRACE(refcnt_t_malloc(len))
#define refcnt_realloc(ptr, len) _REFCNT_TRACE(refcnt_t_realloc(ptr, len))
#define refcnt_ref(ptr) _REFCNT_TRACE(refcnt_t_ref(ptr))
#define refcnt_unref(ptr) _REFCNT_TRACE(refcnt_t_unref(ptr))
#define refcnt_strdup(ptr) _REFCNT_TRACE(refcnt_t_strdup(ptr))
#endif

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define N_ITERATIONS 100

static void *test_thread(void *arg)
{
    char *str = arg;
    for (int i = 0; i < N_ITERATIONS; i++) {
        char *str2 = refcnt_ref(str);
        fprintf(stderr, "Thread %u, %i: %s\n", (unsigned int) pthread_self(), i,
                str2);
        refcnt_unref(str2);
    }
    refcnt_unref(str);
    return NULL;
}

#define N_THREADS 64

int main(int argc, char **argv)
{
    /* Create threads */
    pthread_t threads[N_THREADS];

    /* Create a new string that is count referenced */
    char *str = refcnt_strdup("Hello, world!");

    /* Start the threads, passing a new counted copy of the referece */
    for (int i = 0; i < N_THREADS; i++)
        pthread_create(&threads[i], NULL, test_thread, refcnt_ref(str));

    /* We no longer own the reference */
    refcnt_unref(str);

    /* Whichever thread finishes last will free the string */
    for (int i = 0; i < N_THREADS; i++)
        pthread_join(threads[i], NULL);

    void *ptr = malloc(100);
    /* This should cause a heap overflow while checking the magic num which the
     * sanitizer checks.
     * Leaving commented out for now
     */
    // refcnt_ref(ptr);

    free(ptr);
    return 0;
}
