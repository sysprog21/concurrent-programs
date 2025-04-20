/* Implementing coroutines with setjmp/longjmp */

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "list.h"

struct task {
    jmp_buf env;
    struct list_head list;
    char task_name[10];
    int n;
    int i;
};

struct arg {
    int n;
    int i;
    char *task_name;
};

static LIST_HEAD(tasklist);
static void (**tasks)(void *);
static struct arg *args;
static int ntasks;
static jmp_buf sched;
static struct task *cur_task;

static void task_add(struct task *task)
{
    list_add_tail(&task->list, &tasklist);
}

static void task_switch()
{
    if (!list_empty(&tasklist)) {
        struct task *t = list_first_entry(&tasklist, struct task, list);
        list_del(&t->list);
        cur_task = t;
        longjmp(t->env, 1);
    }
}

void schedule(void)
{
    static int i;

    setjmp(sched);

    while (ntasks-- > 0) {
        struct arg arg = args[i];
        tasks[i++](&arg);
        printf("Never reached\n");
    }

    task_switch();
}

static long long fib_sequence(long long k)
{
    /* FIXME: use clz/ctz and fast algorithms to speed up */
    long long f[k + 2];

    f[0] = 0;
    f[1] = 1;

    for (int i = 2; i <= k; i++) {
        f[i] = f[i - 1] + f[i - 2];
    }

    return f[k];
}

/* A task yields control n times */

void task0(void *arg)
{
    struct task *task = malloc(sizeof(struct task));
    strcpy(task->task_name, ((struct arg *) arg)->task_name);
    task->n = ((struct arg *) arg)->n;
    task->i = ((struct arg *) arg)->i;
    INIT_LIST_HEAD(&task->list);

    printf("%s: n = %d\n", task->task_name, task->n);

    if (setjmp(task->env) == 0) {
        task_add(task);
        longjmp(sched, 1);
    }

    task = cur_task;

    for (; task->i < task->n; task->i += 2) {
        if (setjmp(task->env) == 0) {
            long long res = fib_sequence(task->i);
            printf("%s fib(%d) = %lld\n", task->task_name, task->i, res);
            task_add(task);
            task_switch();
        }
        task = cur_task;
        printf("%s: resume\n", task->task_name);
    }

    printf("%s: complete\n", task->task_name);
    free(task);
    longjmp(sched, 1);
}

void task1(void *arg)
{
    struct task *task = malloc(sizeof(struct task));
    strcpy(task->task_name, ((struct arg *) arg)->task_name);
    task->n = ((struct arg *) arg)->n;
    task->i = ((struct arg *) arg)->i;
    INIT_LIST_HEAD(&task->list);

    printf("%s: n = %d\n", task->task_name, task->n);

    if (setjmp(task->env) == 0) {
        task_add(task);
        longjmp(sched, 1);
    }

    task = cur_task;

    for (; task->i < task->n; task->i++) {
        if (setjmp(task->env) == 0) {
            printf("%s %d\n", task->task_name, task->i);
            task_add(task);
            task_switch();
        }
        task = cur_task;
        printf("%s: resume\n", task->task_name);
    }

    printf("%s: complete\n", task->task_name);
    free(task);
    longjmp(sched, 1);
}

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
int main(void)
{
    void (*registered_task[])(void *) = {task0, task0, task1};
    struct arg arg0 = {.n = 70, .i = 0, .task_name = "Task 0"};
    struct arg arg1 = {.n = 70, .i = 1, .task_name = "Task 1"};
    struct arg arg2 = {.n = 70, .i = 0, .task_name = "Task 2"};
    struct arg registered_arg[] = {arg0, arg1, arg2};
    tasks = registered_task;
    args = registered_arg;
    ntasks = ARRAY_SIZE(registered_task);

    schedule();

    return 0;
}
