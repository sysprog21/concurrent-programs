/* System parameters */
const int PORT = 9000;
#define BACKLOG 1024
#define N_THREADS 24 * sysconf(_SC_NPROCESSORS_ONLN)
#define MAXMSG 1024

/* File parameters */
const char *DOCUMENT_ROOT;
#define MAXPATH 1024

#include <pthread.h>

typedef struct __node {
    int fd;
    struct __node *next;
} node_t;

typedef struct {
    node_t *head, *tail;
    pthread_mutex_t *head_lock, *tail_lock; /* guards head and tail */
    pthread_cond_t *non_empty;
    int size; /* only used for connection timeout heuristic */
} queue_t;

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

/* Simple two-lock concurrent queue described in the paper:
 * http://www.cs.rochester.edu/~scott/papers/1996_PODC_queues.pdf
 * The queue uses a head lock to protect concurrent dequeues and a tail lock
 * to protect concurrent enqueues. Enqueue always succeeds, and their
 * dequeue method may do nothing and return false if it sees that the queue
 * may be empty.
 */
void queue_init(queue_t *q)
{
    node_t *dummy;
    pthread_mutex_t *head_lock, *tail_lock;
    pthread_cond_t *non_empty;

    if (!(dummy = malloc(sizeof(node_t)))) /* Out of memory */
        goto exit;
    if (!(head_lock = malloc(sizeof(pthread_mutex_t)))) /* Out of memory */
        goto cleanup_dummy;
    if (!(tail_lock = malloc(sizeof(pthread_mutex_t)))) /* Out of memory */
        goto cleanup_head_lock;
    if (!(non_empty = malloc(sizeof(pthread_cond_t)))) /* Out of memory */
        goto cleanup_tail_lock;
    if (pthread_mutex_init(head_lock, NULL) ||
        pthread_mutex_init(tail_lock, NULL)) /* Fail to initialize mutex */
        goto cleanup_non_empty;

    dummy->next = NULL;
    q->head = (q->tail = dummy);
    q->head_lock = head_lock, q->tail_lock = tail_lock;
    q->non_empty = non_empty;
    pthread_cond_init(q->non_empty, NULL);

    q->size = 0;
    return;

cleanup_non_empty:
    free(non_empty);
cleanup_tail_lock:
    free(tail_lock);
cleanup_head_lock:
    free(head_lock);
cleanup_dummy:
    free(dummy);
exit:
    exit(1);
}

static void enqueue(queue_t *q, int fd)
{
    /* Construct new node */
    node_t *node = malloc(sizeof(node_t));
    node->fd = fd, node->next = NULL;

    pthread_mutex_lock(q->tail_lock);
    /* Add node to end of queue */
    q->tail->next = node;
    q->tail = node;
    q->size++;

    /* Wake any sleeping worker threads */
    pthread_cond_signal(q->non_empty);
    pthread_mutex_unlock(q->tail_lock);
}

static void dequeue(queue_t *q, int *fd)
{
    node_t *old_head;
    pthread_mutex_lock(q->head_lock);
    /* Wait until signaled that queue is non_empty.
     * Need while loop in case a new thread manages to steal the queue
     * element after the waiting thread is signaled, but before it can
     * re-acquire head_lock.
     */
    while (!q->head->next) /* i.e. q is empty */
        pthread_cond_wait(q->non_empty, q->head_lock);

    /* Store dequeued value and update dummy head */
    old_head = q->head;
    *fd = old_head->next->fd;
    q->head = q->head->next;
    q->size--;
    pthread_mutex_unlock(q->head_lock);
    free(old_head);
}

typedef int status_t;
enum {
    STATUS_OK = 200,
    STATUS_BAD_REQUEST = 400,
    STATUS_FORBIDDEN = 403,
    STATUS_NOT_FOUND = 404,
    STATUS_REQUEST_TIMEOUT = 408,
    STATUS_REQUEST_TOO_LARGE = 413,
    STATUS_SERVER_ERROR = 500,
};

typedef enum { GET, HEAD } http_method_t;

typedef enum {
    APPLICATION,
    AUDIO,
    IMAGE,
    MESSAGE,
    MULTIPART,
    TEXT,
    VIDEO
} content_type_t;

typedef struct {
    http_method_t method;
    char path[MAXPATH];
    content_type_t type;
    int protocol_version;
} http_request_t;

#include <string.h>

/* A collection of useful functions for parsing HTTP messages.
 * See function parse_request for high-level control flow and work upward.
 */

/* TRY_CATCH and TRY_CATCH_S are private macros that "throw" appropriate
 * status codes whenever a parsing method encounters an error. By wrapping
 * every parsing method call in a TRY_CATCH, errors may be piped up to the
 * original parse_request call. The second TRY_CATCH_S macro is for specially
 * translating the error outputs of the string.h function strsep into the
 * BAD_REQUEST (400) status code.
 */
#define TRY_CATCH(STATEMENT)      \
    do {                          \
        status_t s = (STATEMENT); \
        if (s != STATUS_OK)       \
            return s;             \
    } while (0)

#define TRY_CATCH_S(STATEMENT)         \
    do {                               \
        if (!(STATEMENT))              \
            return STATUS_BAD_REQUEST; \
    } while (0)

static const char *type_to_str(const content_type_t type)
{
    switch (type) {
    case APPLICATION:
        return "application";
    case AUDIO:
        return "audio";
    case IMAGE:
        return "image";
    case MESSAGE:
        return "message";
    case MULTIPART:
        return "multipart";
    case TEXT:
        return "text";
    case VIDEO:
        return "video";
    default:
        return NULL;
    }
}

static const char *status_to_str(status_t status)
{
    switch (status) {
    case STATUS_OK:
        return "OK";
    case STATUS_BAD_REQUEST:
        return "Bad Request";
    case STATUS_FORBIDDEN:
        return "Forbidden";
    case STATUS_NOT_FOUND:
        return "Not Found";
    case STATUS_REQUEST_TIMEOUT:
        return "Request Timeout";
    case STATUS_REQUEST_TOO_LARGE:
        return "Request Entity Too Large";
    case STATUS_SERVER_ERROR:
    default:
        return "Internal Server Error";
    }
}

/* Private utility method that acts like strsep(s," \t"), but also advances
 * s so that it skips any additional whitespace.
 */
static char *strsep_whitespace(char **s)
{
    char *ret = strsep(s, " \t");
    while (*s && (**s == ' ' || **s == '\t'))
        (*s)++; /* extra whitespace */
    return ret;
}

/* Same as strsep_whitespace, but for newlines. */
static char *strsep_newline(char **s)
{
    char *ret;
    char *r = strchr(*s, '\r');
    char *n = strchr(*s, '\n');

    if (!r || n < r)
        ret = strsep(s, "\n");
    else {
        ret = strsep(s, "\r");
        (*s)++; /* advance past the trailing \n */
    }
    return ret;
}

static status_t parse_method(char *token, http_request_t *request)
{
    if (strcmp(token, "GET") == 0)
        request->method = GET;
    else if (strcmp(token, "HEAD") == 0)
        request->method = HEAD;
    else
        return STATUS_BAD_REQUEST;
    return STATUS_OK;
}

static status_t parse_path(char *token, http_request_t *request)
{
    if (strcmp(token, "/") == 0 || strcmp(token, "/index.html") == 0) {
        snprintf(request->path, MAXPATH, "%s/index.html", DOCUMENT_ROOT);
        request->type = TEXT;
    } else /* FIXME: handle images files and other resources */
        return STATUS_NOT_FOUND;
    return STATUS_OK;
}

static status_t parse_protocol_version(char *token, http_request_t *request)
{
    if (!strcmp(token, "HTTP/1.0"))
        request->protocol_version = 0;
    else if (!strcmp(token, "HTTP/1.1"))
        request->protocol_version = 1;
    else
        return STATUS_BAD_REQUEST;
    return STATUS_OK;
}

static status_t parse_initial_line(char *line, http_request_t *request)
{
    char *token;
    TRY_CATCH_S(token = strsep_whitespace(&line));
    TRY_CATCH(parse_method(token, request));
    TRY_CATCH_S(token = strsep_whitespace(&line));
    TRY_CATCH(parse_path(token, request));
    TRY_CATCH_S(token = strsep_whitespace(&line));
    TRY_CATCH(parse_protocol_version(token, request));

    return STATUS_OK;
}

/* FIXME: Currently ignores any request headers */
static status_t parse_header(char *line, http_request_t *request)
{
    (void) line, (void) request;
    return STATUS_OK;
}

static status_t parse_request(char *msg, http_request_t *request)
{
    char *line;
    TRY_CATCH_S(line = strsep_newline(&msg));
    TRY_CATCH(parse_initial_line(line, request));
    while ((line = strsep_newline(&msg)) != NULL && *line != '\0')
        TRY_CATCH(parse_header(line, request));

    return STATUS_OK;
}

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* Some wrapper functions for listening socket initalization,
 * where we may simply exit if we cannot set up the socket.
 */

static int socket_(int domain, int type, int protocol)
{
    int sockfd;
    if ((sockfd = socket(domain, type, protocol)) < 0) {
        fprintf(stderr, "Socket error!\n");
        exit(1);
    }
    return sockfd;
}

static int bind_(int socket,
                 const struct sockaddr *address,
                 socklen_t address_len)
{
    int ret;
    if ((ret = bind(socket, address, address_len)) < 0) {
        perror("bind_");
        exit(1);
    }
    return ret;
}

static int listen_(int socket, int backlog)
{
    int ret;
    if ((ret = listen(socket, backlog)) < 0) {
        fprintf(stderr, "Listen error!\n");
        exit(1);
    }
    return ret;
}

/* Initialize listening socket */
static int listening_socket()
{
    struct sockaddr_in serveraddr;
    memset(&serveraddr, 0, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(PORT);
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);

    int listenfd = socket_(AF_INET, SOCK_STREAM, 0);
    bind_(listenfd, (struct sockaddr *) &serveraddr, sizeof(serveraddr));
    listen_(listenfd, BACKLOG);
    return listenfd;
}

static void *worker_routine(void *arg)
{
    pthread_detach(pthread_self());

    int connfd, file, len, recv_bytes;
    char msg[MAXMSG], buf[1024];
    status_t status;
    http_request_t *request = malloc(sizeof(http_request_t));
    queue_t *q = (queue_t *) arg;
    struct stat st;

    while (1) {
    loopstart:
        dequeue(q, &connfd);
        memset(msg, 0, MAXMSG);
        recv_bytes = 0;

        /* Loop until full HTTP msg is received */
        while (strstr(strndup(msg, recv_bytes), "\r\n\r\n") == NULL &&
               strstr(strndup(msg, recv_bytes), "\n\n") == NULL &&
               recv_bytes < MAXMSG) {
            if ((len = recv(connfd, msg + recv_bytes, MAXMSG - recv_bytes,
                            0)) <= 0) {
                /* If client has closed, then close and move on */
                if (len == 0) {
                    close(connfd);
                    goto loopstart;
                }
                /* If timeout or error, skip parsing and send appropriate
                 * error message
                 */
                if (errno == EWOULDBLOCK) {
                    status = STATUS_REQUEST_TIMEOUT;
                } else {
                    status = STATUS_SERVER_ERROR;
                    perror("recv");
                }
                goto send;
            }
            recv_bytes += len;
        }

        /* Parse (complete) message */
        status = parse_request(msg, request);

    send:
        /* Send initial line */
        len = sprintf(msg, "HTTP/1.%d %d %s\r\n", request->protocol_version,
                      status, status_to_str(status));
        send(connfd, msg, len, 0);

        /* Send header lines */
        time_t now;
        time(&now);
        len = strftime(buf, 1024, "Date: %a, %d %b %Y %H:%M:%S GMT\r\n",
                       gmtime(&now));
        send(connfd, buf, len, 0);
        if (status == STATUS_OK && request->method == GET) {
            stat(request->path, &st);
            len = sprintf(msg, "Content-Length: %d\r\n", (int) st.st_size);
            send(connfd, msg, len, 0);
            len = sprintf(msg, "Content-Type: %s\r\n",
                          type_to_str(request->type));
            send(connfd, msg, len, 0);
        }
        send(connfd, "\r\n", 2, 0);

        /* If request was well-formed GET, then send file */
        if (status == STATUS_OK && request->method == GET) {
            if ((file = open(request->path, O_RDONLY)) < 0)
                perror("open");
            while ((len = read(file, msg, MAXMSG)) > 0)
                if (send(connfd, msg, len, 0) < 0)
                    perror("sending file");
            close(file);
        }

        /* If HTTP/1.0 or recv error, close connection. */
        if (request->protocol_version == 0 || status != STATUS_OK)
            close(connfd);
        else /* Otherwise, keep connection alive and re-enqueue */
            enqueue(q, connfd);
    }
    return NULL;
}

struct greeter_args {
    int listfd;
    queue_t *q;
};

void *greeter_routine(void *arg)
{
    struct greeter_args *ga = (struct greeter_args *) arg;
    int listfd = ga->listfd;
    queue_t *q = ga->q;

    struct sockaddr_in clientaddr;
    struct timeval timeout;

    /* Accept connections, set their timeouts, and enqueue them */
    while (1) {
        socklen_t clientlen = sizeof(clientaddr);
        int connfd =
            accept(listfd, (struct sockaddr *) &clientaddr, &clientlen);
        if (connfd < 0) {
            perror("accept");
            continue;
        }

        /* Basic heuristic for timeout based on queue length.
         * Minimum timeout 10s + another second for every 50 connections on the
         * queue.
         */
        int n = q->size;
        timeout.tv_sec = 10;
        if (n > 0)
            timeout.tv_sec += n / 50;
        setsockopt(connfd, SOL_SOCKET, SO_RCVTIMEO, (void *) &timeout,
                   sizeof(timeout));
        enqueue(q, connfd);
    }
}

int main()
{
    queue_t *connections;
    pthread_t workers[N_THREADS / 2], greeters[N_THREADS / 2];

    /* Get current working directory */
    char cwd[1024];
    const char *RESOURCES = "/resources";
    if (getcwd(cwd, sizeof(cwd) - sizeof(RESOURCES)) == NULL)
        perror("getcwd");

    /* Assign document root */
    DOCUMENT_ROOT = strcat(cwd, RESOURCES);

    /* Initalize connections queue */
    connections = malloc(sizeof(queue_t));
    queue_init(connections);

    /* Initialize listening socket */
    int listfd = listening_socket();

    /* Package arguments for greeter threads */
    struct greeter_args ga = {.listfd = listfd, .q = connections};

    /* Spawn greeter threads. */
    for (int i = 0; i < N_THREADS / 2; i++)
        pthread_create(&greeters[i], NULL, greeter_routine, (void *) (&ga));

    /* Spawn worker threads. These will immediately block until signaled by
     * main server thread pushes connections onto the queue and signals.
     */
    for (int i = 0; i < N_THREADS / 2; i++)
        pthread_create(&workers[i], NULL, worker_routine, (void *) connections);

    pthread_exit(NULL);
    return 0;
}
