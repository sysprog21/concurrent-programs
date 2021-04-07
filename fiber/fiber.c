#define FIBER_NOERROR 0
#define FIBER_MAXFIBERS 1
#define FIBER_MALLOC_ERROR 2
#define FIBER_CLONE_ERROR 3
#define FIBER_INFIBER 4

/* The maximum number of fibers that can be active at once. */
#define MAX_FIBERS 10

/* The size of the stack for each fiber. */
#define FIBER_STACK (1024 * 1024)

#define _GNU_SOURCE

#include <sched.h> /* For clone */
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h> /* For pid_t */
#include <sys/wait.h>  /* For wait */
#include <unistd.h>    /* For getpid */

typedef struct {
    pid_t pid;   /* The pid of the child thread as returned by clone */
    void *stack; /* The stack pointer */
} fiber_t;

/* The fiber "queue" */
static fiber_t fiber_list[MAX_FIBERS];

/* The pid of the parent process */
static pid_t parent;

/* The number of active fibers */
static int num_fibers = 0;

void fiber_init()
{
    for (int i = 0; i < MAX_FIBERS; ++i)
        fiber_list[i].pid = 0, fiber_list[i].stack = 0;
    parent = getpid();
}

/* Yield control to another execution context */
void fiber_yield()
{
    /* move the current process to the end of the process queue. */
    sched_yield();
}

struct fiber_args {
    void (*func)(void);
};

static int fiber_start(void *arg)
{
    struct fiber_args *args = (struct fiber_args *) arg;
    void (*func)() = args->func;
    free(args);

    func();
    return 0;
}

/* Creates a new fiber, running the func that is passed as an argument. */
int fiber_spawn(void (*func)(void))
{
    if (num_fibers == MAX_FIBERS)
        return FIBER_MAXFIBERS;

    if ((fiber_list[num_fibers].stack = malloc(FIBER_STACK)) == 0)
        return FIBER_MALLOC_ERROR;

    struct fiber_args *args;
    if ((args = malloc(sizeof(*args))) == 0) {
        free(fiber_list[num_fibers].stack);
        return FIBER_MALLOC_ERROR;
    }
    args->func = func;

    fiber_list[num_fibers].pid = clone(
        fiber_start, (char *) fiber_list[num_fibers].stack + FIBER_STACK,
        SIGCHLD | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_VM, args);
    if (fiber_list[num_fibers].pid == -1) {
        free(fiber_list[num_fibers].stack);
        free(args);
        return FIBER_CLONE_ERROR;
    }

    num_fibers++;
    return FIBER_NOERROR;
}

/* Execute the fibers until they all quit. */
int fiber_wait_all()
{
    /* Check to see if we are in a fiber, since we do not get signals in the
     * child threads
     */
    pid_t pid = getpid();
    if (pid != parent)
        return FIBER_INFIBER;

    /* Wait for the fibers to quit, then free the stacks */
    while (num_fibers > 0) {
        if ((pid = wait(0)) == -1)
            exit(1);

        /* Find the fiber, free the stack, and swap it with the last one */
        for (int i = 0; i < num_fibers; ++i) {
            if (fiber_list[i].pid == pid) {
                free(fiber_list[i].stack);
                if (i != --num_fibers)
                    fiber_list[i] = fiber_list[num_fibers];
                break;
            }
        }
    }

    return FIBER_NOERROR;
}

static void fibonacci()
{
    int fib[2] = {0, 1};
    printf("Fib(0) = 0\nFib(1) = 1\n");
    for (int i = 2; i < 15; ++i) {
        int next = fib[0] + fib[1];
        printf("Fib(%d) = %d\n", i, next);
        fib[0] = fib[1];
        fib[1] = next;
        fiber_yield();
    }
}

static void squares()
{
    for (int i = 1; i < 10; ++i) {
        printf("%d * %d = %d\n", i, i, i * i);
        fiber_yield();
    }
}

int main()
{
    fiber_init();

    fiber_spawn(&fibonacci);
    fiber_spawn(&squares);

    /* Since these are non-preemptive, we must allow them to run */
    fiber_wait_all();

    return 0;
}
