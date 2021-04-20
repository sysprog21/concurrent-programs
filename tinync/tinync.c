#include <stddef.h>

/* coroutine status values */
enum {
    CR_BLOCKED = 0,
    CR_FINISHED = 1,
};

/* Helper macros to generate unique labels */
#define __cr_line3(name, line) _cr_##name##line
#define __cr_line2(name, line) __cr_line3(name, line)
#define __cr_line(name) __cr_line2(name, __LINE__)

struct cr {
    void *label;
    int status;
};

#define cr_context_name(name) __cr_context_##name
#define cr_context(name) struct cr cr_context_name(name)
#define cr_context_init()                   \
    {                                       \
        .label = NULL, .status = CR_BLOCKED \
    }

#define cr_func_name(name) __cr_func_##name
#define cr_proto(name, ...) cr_func_name(name)(struct cr * ctx, ##__VA_ARGS__)

#define cr_run(name, ...) \
    cr_func_name(name)(&cr_context_name(name), ##__VA_ARGS__)

#define cr_local static

#define cr_begin()                         \
    do {                                   \
        if ((ctx)->status == CR_FINISHED)  \
            return;                        \
        if ((ctx)->label)                  \
            goto *(ctx)->label;            \
    } while (0)
#define cr_label(o, stat)                                   \
    do {                                                    \
        (o)->status = (stat);                               \
        __cr_line(label) : (o)->label = &&__cr_line(label); \
    } while (0)
#define cr_end() cr_label(ctx, CR_FINISHED)

#define cr_status(name) cr_context_name(name).status

#define cr_wait(cond)               \
    do {                            \
        cr_label(ctx, CR_BLOCKED);  \
        if (!(cond))                \
            return;                 \
    } while (0)

#define cr_exit(stat)         \
    do {                      \
        cr_label(ctx, stat);  \
        return;               \
    } while (0)

#define cr_queue(T, size) \
    struct {              \
        T buf[size];      \
        size_t r, w;      \
    }
#define cr_queue_init() \
    {                   \
        .r = 0, .w = 0  \
    }
#define cr_queue_len(q) (sizeof((q)->buf) / sizeof((q)->buf[0]))
#define cr_queue_cap(q) ((q)->w - (q)->r)
#define cr_queue_empty(q) ((q)->w == (q)->r)
#define cr_queue_full(q) (cr_queue_cap(q) == cr_queue_len(q))

#define cr_queue_push(q, el) \
    (!cr_queue_full(q) && ((q)->buf[(q)->w++ % cr_queue_len(q)] = (el), 1))
#define cr_queue_pop(q) \
    (cr_queue_empty(q) ? NULL : &(q)->buf[(q)->r++ % cr_queue_len(q)])

/* Wrap system calls and other functions that return -1 and set errno */
#define cr_sys(call)                                                        \
    cr_wait((errno = 0) ||                                                  \
            !(((call) == -1) && (errno == EAGAIN || errno == EWOULDBLOCK || \
                                 errno == EINPROGRESS || errno == EINTR)))

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

typedef cr_queue(uint8_t, 4096) byte_queue_t;

static void cr_proto(stdin_loop, byte_queue_t *out)
{
    cr_local uint8_t b;
    cr_local int r;
    cr_begin();
    for (;;) {
        cr_sys(r = read(STDIN_FILENO, &b, 1));
        if (r == 0) {
            cr_wait(cr_queue_empty(out));
            cr_exit(1);
        }
        cr_wait(!cr_queue_full(out));
        cr_queue_push(out, b);
    }
    cr_end();
}

static void cr_proto(socket_write_loop, byte_queue_t *in, int fd)
{
    cr_local uint8_t *b;
    cr_begin();
    for (;;) {
        cr_wait(!cr_queue_empty(in));
        b = cr_queue_pop(in);
        cr_sys(send(fd, b, 1, 0));
    }
    cr_end();
}

static void cr_proto(socket_read_loop, int fd)
{
    cr_local uint8_t b;
    cr_local int r;
    cr_begin();
    for (;;) {
        cr_sys(r = recv(fd, &b, 1, 0));
        if (r == 0)
            cr_exit(1);
        cr_sys(write(STDOUT_FILENO, &b, 1));
    }
    cr_end();
}

static int nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "USAGE: %s <ip> <port>\n", argv[0]);
        return 1;
    }

    char *host = argv[1];
    int port = atoi(argv[2]);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket()");
        return 1;
    }
    if (nonblock(fd) < 0) {
        perror("nonblock() socket");
        return 1;
    }
    if (nonblock(STDIN_FILENO) < 0) {
        perror("nonblock() stdin");
        return 1;
    }
    if (nonblock(STDOUT_FILENO) < 0) {
        perror("nonblock() stdout");
        return 1;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr =
            {
                .s_addr = inet_addr(host),
            },
        .sin_port = htons(port),
    };
    connect(fd, (struct sockaddr *) &addr, sizeof(struct sockaddr_in));

    byte_queue_t queue = cr_queue_init();

    cr_context(stdin_loop) = cr_context_init();
    cr_context(socket_read_loop) = cr_context_init();
    cr_context(socket_write_loop) = cr_context_init();

    while (cr_status(stdin_loop) == CR_BLOCKED &&
           cr_status(socket_read_loop) == CR_BLOCKED) {
        if (cr_queue_empty(&queue)) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            FD_SET(fd, &fds);
            select(fd + 1, &fds, NULL, NULL, NULL);
        }
        cr_run(socket_read_loop, fd);
        cr_run(socket_write_loop, &queue, fd);
        cr_run(stdin_loop, &queue);
    }

    close(fd);
    return 0;
}
