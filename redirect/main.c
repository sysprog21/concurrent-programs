#include <stdbool.h>
#include <stdio.h>
#include <sys/epoll.h>

#define EV_FOREACH(node, list) \
    for (typeof(node) nxt, node = list; node && (nxt = node->next); node = nxt)

#define EV_INSERT(node, list)                 \
    do {                                      \
        typeof(node) next = list;             \
        list = node;                          \
        if (next)                             \
            next->prev = node;                \
        node->next = next, node->prev = NULL; \
    } while (0)

#define EV_REMOVE(node, list)                              \
    do {                                                   \
        typeof(node) prev = node->prev, next = node->next; \
        if (prev)                                          \
            prev->next = next;                             \
        if (next)                                          \
            next->prev = prev;                             \
        node->prev = node->next = NULL;                    \
        if (list == node)                                  \
            list = next;                                   \
    } while (0)

/* I/O or timer watcher */
typedef enum { EV_IO_TYPE = 1, EV_TIMER_TYPE } ev_type_t;

/* Event mask, used internally only. */
#define EV_EVENT_MASK                                                       \
    (EV_ERROR | EV_READ | EV_WRITE | EV_PRI | EV_RDHUP | EV_HUP | EV_EDGE | \
     EV_ONESHOT)

/* Main EV context */
typedef struct {
    bool running;
    int fd; /* For epoll() */
    struct ev *watchers;
    bool workaround; /* For workarounds, e.g. redirected stdin */
} ev_ctx_t;

/* hide all private data members in ev_t */
#define ev_private_t                                        \
    struct ev *next, *prev;                                 \
                                                            \
    int active;                                             \
    int events;                                             \
                                                            \
    /* Watcher callback with optional argument */           \
    void (*cb)(struct ev *, void *, int);                   \
    void *arg;                                              \
                                                            \
    /* Arguments for different watchers */                  \
    union {                                                 \
        struct { /* Timer watchers, time in milliseconds */ \
            int timeout, period;                            \
        } t;                                                \
    } u;                                                    \
                                                            \
    /* Watcher type */                                      \
    ev_type_t

static int _ev_watcher_init(ev_ctx_t *ctx,
                            struct ev *w,
                            ev_type_t type,
                            void (*cb)(struct ev *, void *, int),
                            void *arg,
                            int fd,
                            int events);
static int _ev_watcher_start(struct ev *w);
static int _ev_watcher_stop(struct ev *w);
static bool _ev_watcher_active(struct ev *w);
static int _ev_watcher_rearm(struct ev *w);

/* Max number of simulateneous events */
#define EV_MAX_EVENTS 10

/* I/O events and timer revents are always EV_READ */
#define EV_ERROR EPOLLERR
#define EV_READ EPOLLIN
#define EV_WRITE EPOLLOUT
#define EV_PRI EPOLLPRI
#define EV_HUP EPOLLHUP
#define EV_RDHUP EPOLLRDHUP
#define EV_EDGE EPOLLET
#define EV_ONESHOT EPOLLONESHOT

/* Run flags */
enum { EV_ONCE = 1, EV_NONBLOCK = 2 };

/* Event watcher */
typedef struct ev {
    ev_private_t type; /* private data */

    int fd;
    ev_ctx_t *ctx;
} ev_t;

/* Generic callback for watchers, @events holds %EV_READ and/or %EV_WRITE with
 * optional %EV_PRI (priority data available to read) and any of the %EV_HUP
 * and/or %EV_RDHUP, which may be used to signal hang-up events.
 *
 * %EV_ERROR conditions must be handled by all callbacks.
 * I/O watchers may also need to check EV_HUP. Appropriate action, e.g.
 * restart the watcher, is up to the application and is thus delegated to the
 * callback.
 */
typedef void(ev_cb_t)(ev_t *w, void *arg, int events);

/* Public interface */
int ev_init(ev_ctx_t *ctx);
int ev_exit(ev_ctx_t *ctx);
int ev_run(ev_ctx_t *ctx, int flags);

int ev_io_start(ev_t *w);
int ev_io_stop(ev_t *w);

#include <errno.h>

/* Create an I/O watcher
 * @param ctx     A valid EV context
 * @param w       Pointer to an ev_t watcher
 * @param cb      I/O callback
 * @param arg     Optional callback argument
 * @param fd      File descriptor to watch, or -1 to register an empty watcher
 * @param events  Events to watch for: %EV_READ, %EV_WRITE, %EV_EDGE,
 * %EV_ONESHOW
 *
 * @return POSIX OK(0) or non-zero with @param errno set on error.
 */
int ev_io_init(ev_ctx_t *ctx,
               ev_t *w,
               ev_cb_t *cb,
               void *arg,
               int fd,
               int events)
{
    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }

    if (_ev_watcher_init(ctx, w, EV_IO_TYPE, cb, arg, fd, events))
        return -1;

    return _ev_watcher_start(w);
}

/* Reset an I/O watcher
 * @param w       Pointer to an ev_t watcher
 * @param fd      New file descriptor to monitor
 * @param events  Requested events to watch for, a mask of %EV_READ and
 * %EV_WRITE
 *
 * @return POSIX OK(0) or non-zero with @param errno set on error.
 */
static int ev_io_set(ev_t *w, int fd, int events)
{
    if ((events & EV_ONESHOT) && _ev_watcher_active(w))
        return _ev_watcher_rearm(w);

    /* Ignore any errors, only to clean up anything lingering */
    ev_io_stop(w);

    return ev_io_init(w->ctx, w, (ev_cb_t *) w->cb, w->arg, fd, events);
}

/* Start an I/O watcher
 * @param w  Watcher to start (again)
 *
 * @return POSIX OK(0) or non-zero with @param errno set on error.
 */
int ev_io_start(ev_t *w)
{
    return ev_io_set(w, w->fd, w->events);
}

/* Stop an I/O watcher
 * @param w  Watcher to stop
 *
 * @return POSIX OK(0) or non-zero with @param errno set on error.
 */
int ev_io_stop(ev_t *w)
{
    return _ev_watcher_stop(w);
}

#include <sys/timerfd.h>
#include <unistd.h>

static inline void ms2ts(int ms, struct timespec *ts)
{
    if (ms) /* millisecond */
        ts->tv_sec = ms / 1000, ts->tv_nsec = (ms % 1000) * 1000000;
    else
        ts->tv_sec = 0, ts->tv_nsec = 0;
}

static int ev_timer_set(ev_t *w, int timeout, int period);

/* Create and start a timer watcher
 * @param ctx      A valid EV context
 * @param w        Pointer to an ev_t watcher
 * @param cb       Callback function
 * @param arg      Optional callback argument
 * @param timeout  Timeout in milliseconds before @param cb is called
 * @param period   For periodic timers this is the period time that @param
 * timeout is reset to
 *
 * This function creates, and optionally starts, a timer watcher. There are
 * two types of timers: one-shot and periodic.
 *
 * One-shot timers only use @param timeout, @param period is zero.
 *
 * Periodic timers can either start their life disabled, with @param timeout
 * set to zero, or with the same value as @param period.
 *
 * When the timeout expires, for either of the two types, @param cb is called,
 * with the optional @param arg argument. A one-shot timer ends its life there,
 * while a periodic task's @param timeout is reset to the @param period and
 * restarted.
 *
 * A timer is automatically started if the event loop is already running,
 * otherwise it is kept on hold until triggered by calling ev_run().
 *
 * @return POSIX OK(0) or non-zero with @param errno set on error.
 */
static int ev_timer_init(ev_ctx_t *ctx,
                         ev_t *w,
                         ev_cb_t *cb,
                         void *arg,
                         int timeout,
                         int period)
{
    if (timeout < 0 || period < 0) {
        errno = ERANGE;
        return -1;
    }

    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0)
        return -1;

    if (_ev_watcher_init(ctx, w, EV_TIMER_TYPE, cb, arg, fd, EV_READ))
        goto out;

    if (ev_timer_set(w, timeout, period)) {
        _ev_watcher_stop(w);
    out:
        close(fd);
        w->fd = -1;
        return -1;
    }

    return 0;
}

/* Reset a timer
 * @param w        Watcher to reset
 * @param timeout  Timeout in milliseconds before @param cb is called, zero
 *                 disarms timer
 * @param period   For periodic timers this is the period time that @param
 *                 timeout is reset to
 *
 * Note, the @param timeout value must be non-zero.  Setting it to zero will
 * disarm the timer.  This is the underlying Linux function @func
 * timerfd_settimer() which has this behavior.
 *
 * @return POSIX OK(0) or non-zero with @param errno set on error.
 */
static int ev_timer_set(ev_t *w, int timeout, int period)
{
    /* Every watcher must be registered to a context */
    if (!w || !w->ctx) {
        errno = EINVAL;
        return -1;
    }

    if (timeout < 0 || period < 0) {
        errno = ERANGE;
        return -1;
    }

    if (w->fd < 0) { /* Handle stopped timers */
        if (!timeout && !period)
            return 0; /* Timer already stopped */

        if (ev_timer_init(w->ctx, w, w->cb, w->arg, timeout, period))
            return -1;
    }

    w->u.t.timeout = timeout, w->u.t.period = period;

    if (w->ctx->running) {
        struct itimerspec time;

        ms2ts(timeout, &time.it_value), ms2ts(period, &time.it_interval);
        if (timerfd_settime(w->fd, 0, &time, NULL) < 0)
            return 1;
    }

    return _ev_watcher_start(w);
}

/* Stop and unregister a timer watcher
 * @param w  Watcher to stop
 *
 * @return POSIX OK(0) or non-zero with @param errno set on error.
 */
static int ev_timer_stop(ev_t *w)
{
    if (!_ev_watcher_active(w))
        return 0;

    if (_ev_watcher_stop(w))
        return -1;

    close(w->fd);
    w->fd = -1;

    return 0;
}

#include <string.h> /* memset() */

#include <fcntl.h> /* O_CLOEXEC */
#include <sys/ioctl.h>
#include <sys/select.h> /* for select() workaround */

static int _init(ev_ctx_t *ctx, bool close_old)
{
    int fd = epoll_create1(EPOLL_CLOEXEC);
    if (fd < 0)
        return -1;

    if (close_old)
        close(ctx->fd);

    ctx->fd = fd;

    return 0;
}

/* Used by file I/O workaround when epoll => EPERM */
static bool has_data(int fd)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);

    struct timeval timeout = {0, 0};
    if (select(1, &fds, NULL, NULL, &timeout) > 0) {
        int n = 0;
        return (ioctl(0, FIONREAD, &n) == 0) && (n > 0);
    }

    return false;
}

static int _ev_watcher_init(ev_ctx_t *ctx,
                            ev_t *w,
                            ev_type_t type,
                            ev_cb_t *cb,
                            void *arg,
                            int fd,
                            int events)
{
    if (!ctx || !w) {
        errno = EINVAL;
        return -1;
    }

    w->ctx = ctx, w->type = type, w->active = 0, w->fd = fd;
    w->cb = cb, w->arg = arg;
    w->events = events;

    return 0;
}

static int _ev_watcher_start(ev_t *w)
{
    if (!w || w->fd < 0 || !w->ctx) {
        errno = EINVAL;
        return -1;
    }

    if (_ev_watcher_active(w))
        return 0;

    struct epoll_event ev = {.events = w->events | EPOLLRDHUP, .data.ptr = w};
    if (epoll_ctl(w->ctx->fd, EPOLL_CTL_ADD, w->fd, &ev) < 0) {
        if (errno != EPERM)
            return -1;

        /* Handle special case: "prog < file.txt" */
        if (w->type != EV_IO_TYPE || w->events != EV_READ)
            return -1;

        if (w->fd != STDIN_FILENO)
            return -1; /* special handling for stdin */

        w->ctx->workaround = true;
        w->active = -1;
    } else
        w->active = 1;

    /* Add to internal list for bookkeeping */
    EV_INSERT(w, w->ctx->watchers);

    return 0;
}

static int _ev_watcher_stop(ev_t *w)
{
    if (!w) {
        errno = EINVAL;
        return -1;
    }

    if (!_ev_watcher_active(w))
        return 0;

    w->active = 0;

    /* Remove from internal list */
    EV_REMOVE(w, w->ctx->watchers);

    /* Remove from kernel */
    if (epoll_ctl(w->ctx->fd, EPOLL_CTL_DEL, w->fd, NULL) < 0)
        return -1;

    return 0;
}

static bool _ev_watcher_active(ev_t *w)
{
    return w ? (w->active > 0) : false;
}

static int _ev_watcher_rearm(ev_t *w)
{
    if (!w || w->fd < 0) {
        errno = EINVAL;
        return -1;
    }

    struct epoll_event ev = {.events = w->events | EPOLLRDHUP, .data.ptr = w};
    if (epoll_ctl(w->ctx->fd, EPOLL_CTL_MOD, w->fd, &ev) < 0)
        return -1;

    return 0;
}

/* Create an event loop context
 * @param ctx  Pointer to an ev_ctx_t context to be initialized
 *
 * In cases where you have multiple events pending in the cache and some event
 * may cause later ones, already sent by the kernel to userspace, to be deleted
 * the pointer returned to the event loop for this later event may be deleted.
 *
 * @return POSIX OK(0) on success, or non-zero on error.
 */
int ev_init(ev_ctx_t *ctx)
{
    if (!ctx) {
        errno = EINVAL;
        return -1;
    }

    memset(ctx, 0, sizeof(*ctx));
    return _init(ctx, false);
}

/* Terminate the event loop
 * @param ctx  A valid EV context
 *
 * @return POSIX OK(0) or non-zero with @param errno set on error.
 */
int ev_exit(ev_ctx_t *ctx)
{
    if (!ctx) {
        errno = EINVAL;
        return -1;
    }

    ev_t *w;
    EV_FOREACH (w, ctx->watchers) {
        /* Remove from internal list */
        EV_REMOVE(w, ctx->watchers);

        if (!_ev_watcher_active(w))
            continue;

        switch (w->type) {
        case EV_IO_TYPE:
            ev_io_stop(w);
            break;
        case EV_TIMER_TYPE:
            ev_timer_stop(w);
            break;
        }
    }

    ctx->watchers = NULL, ctx->running = false;
    if (ctx->fd > -1)
        close(ctx->fd);
    ctx->fd = -1;

    return 0;
}

/* Start the event loop
 * @param ctx    A valid EV context
 * @param flags  A mask of %EV_ONCE and %EV_NONBLOCK, or zero
 *
 * With @flags set to %EV_ONCE the event loop returns after the first event has
 * been served, useful for instance to set a timeout on a file descriptor. If
 * @flags also has %EV_NONBLOCK flag set the event loop will return immediately
 * if no event is pending, useful when run inside another event loop.
 *
 * @return POSIX OK(0) upon successful termination of the event loop, or
 * non-zero on error.
 */
int ev_run(ev_ctx_t *ctx, int flags)
{
    if (!ctx || ctx->fd < 0) {
        errno = EINVAL;
        return -1;
    }

    int timeout = -1;
    if (flags & EV_NONBLOCK)
        timeout = 0;

    ctx->running = true; /* Start the event loop */

    /* Start all dormant timers */
    ev_t *w;
    EV_FOREACH (w, ctx->watchers) {
        if (w->type == EV_TIMER_TYPE)
            ev_timer_set(w, w->u.t.timeout, w->u.t.period);
    }

    while (ctx->running && ctx->watchers) {
        struct epoll_event ee[EV_MAX_EVENTS];
        int nfds, rerun = 0;

        /* Handle special case: "prog < file.txt" */
        if (ctx->workaround) {
            EV_FOREACH (w, ctx->watchers) {
                if (w->active != -1 || !w->cb)
                    continue;

                if (!has_data(w->fd)) {
                    w->active = 0;
                    EV_REMOVE(w, ctx->watchers);
                }

                rerun++;
                w->cb(w, w->arg, EV_READ);
            }
        }

        if (rerun)
            continue;
        ctx->workaround = false;

        while ((nfds = epoll_wait(ctx->fd, ee, EV_MAX_EVENTS, timeout)) < 0) {
            if (!ctx->running)
                break;

            if (EINTR == errno)
                continue; /* Signalled, try again */

            /* Unrecoverable error, cleanup and exit with error. */
            ev_exit(ctx);

            return -2;
        }

        for (int i = 0; ctx->running && i < nfds; i++) {
            uint64_t exp;

            w = (ev_t *) ee[i].data.ptr;
            uint32_t events = ee[i].events;

            switch (w->type) {
            case EV_IO_TYPE:
                if (events & (EPOLLHUP | EPOLLERR))
                    ev_io_stop(w);
                break;

            case EV_TIMER_TYPE:
                if (read(w->fd, &exp, sizeof(exp)) != sizeof(exp)) {
                    ev_timer_stop(w);
                    events = EV_ERROR;
                }

                if (!w->u.t.period)
                    w->u.t.timeout = 0;
                if (!w->u.t.timeout)
                    ev_timer_stop(w);
                break;
            }

            /* Must be last action for watcher, callback may delete itself */
            if (w->cb)
                w->cb(w, w->arg, events & EV_EVENT_MASK);
        }

        if (flags & EV_ONCE)
            break;
    }

    return 0;
}

#include <err.h>
#include <stdlib.h>

static void process_stdin(ev_t *w, void *arg /* unused */, int events)
{
    if (events == EV_ERROR) {
        warnx("Spurious problem with the stdin watcher, restarting.");
        ev_io_start(w);
    }

    char buf[256];
    int len = read(w->fd, buf, sizeof(buf));
    if (len == -1) {
        warn("Error reading from stdin");
        return;
    }

    if (len == 0 || EV_HUP == events) /* Ignore */
        return;

    printf("Read %d bytes\n", len);
    if (write(STDOUT_FILENO, buf, len) != len)
        warn("Failed writing to stdout");
}

int main(void)
{
    ev_ctx_t ctx;
    ev_init(&ctx);

    ev_t watcher;
    if (ev_io_init(&ctx, &watcher, process_stdin, NULL, STDIN_FILENO, EV_READ))
        err(errno, "Failed setting up STDIN watcher");

    return ev_run(&ctx, 0);
}
