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
    void *local; /* private local storage */
};

#define cr_init()                           \
    {                                       \
        .label = NULL, .status = CR_BLOCKED \
    }
#define cr_begin(o)                     \
    do {                                \
        if ((o)->status == CR_FINISHED) \
            return;                     \
        if ((o)->label)                 \
            goto *(o)->label;           \
    } while (0)
#define cr_label(o, stat)                                   \
    do {                                                    \
        (o)->status = (stat);                               \
        __cr_line(label) : (o)->label = &&__cr_line(label); \
    } while (0)
#define cr_end(o) cr_label(o, CR_FINISHED)

#define cr_status(o) (o)->status

#define cr_wait(o, cond)         \
    do {                         \
        cr_label(o, CR_BLOCKED); \
        if (!(cond))             \
            return;              \
    } while (0)

#define cr_exit(o, stat)   \
    do {                   \
        cr_label(o, stat); \
        return;            \
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
#define cr_sys(o, call)                                                     \
    cr_wait(o, (errno = 0) || !(((call) == -1) &&                           \
                                (errno == EAGAIN || errno == EWOULDBLOCK || \
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

static void stdin_loop(struct cr *o, byte_queue_t *out)
{
    static uint8_t b;
    static int r;
    cr_begin(o);
    for (;;) {
        cr_sys(o, r = read(STDIN_FILENO, &b, 1));
        if (r == 0) {
            cr_wait(o, cr_queue_empty(out));
            cr_exit(o, 1);
        }
        cr_wait(o, !cr_queue_full(out));
        cr_queue_push(out, b);
    }
    cr_end(o);
}

static void socket_write_loop(struct cr *o, int fd, byte_queue_t *in)
{
    static uint8_t *b;
    cr_begin(o);
    for (;;) {
        cr_wait(o, !cr_queue_empty(in));
        b = cr_queue_pop(in);
        cr_sys(o, send(fd, b, 1, 0));
    }
    cr_end(o);
}

static void socket_read_loop(struct cr *o, int fd)
{
    static uint8_t b;
    static int r;
    cr_begin(o);
    for (;;) {
        cr_sys(o, r = recv(fd, &b, 1, 0));
        if (r == 0)
            cr_exit(o, 1);
        cr_sys(o, write(STDOUT_FILENO, &b, 1));
    }
    cr_end(o);
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

    struct cr cr_stdin = cr_init();
    struct cr cr_socket_read = cr_init();
    struct cr cr_socket_write = cr_init();
    byte_queue_t queue = cr_queue_init();

    while (cr_status(&cr_stdin) == CR_BLOCKED &&
           cr_status(&cr_socket_read) == CR_BLOCKED) {
        if (cr_queue_empty(&queue)) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            FD_SET(fd, &fds);
            select(fd + 1, &fds, NULL, NULL, NULL);
        }
        socket_read_loop(&cr_socket_read, fd);
        socket_write_loop(&cr_socket_write, fd, &queue);
        stdin_loop(&cr_stdin, &queue);
    }

    close(fd);
    return 0;
}
