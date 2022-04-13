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
};

static LIST_HEAD(tasklist);
static void (**tasks)(void *);
static int ntasks;
static jmp_buf sched;

static void task_add(struct list_head *tasklist, jmp_buf env)
{
    struct task *t = malloc(sizeof(*t));
    memcpy(t->env, env, sizeof(jmp_buf));
    INIT_LIST_HEAD(&t->list);
    list_add_tail(&t->list, tasklist);
}

static void task_switch(struct list_head *tasklist)
{
    jmp_buf env;

    if (!list_empty(tasklist)) {
        struct task *t = list_first_entry(tasklist, struct task, list);
        list_del(&t->list);
        memcpy(env, t->env, sizeof(jmp_buf));
        free(t);
        longjmp(env, 1);
    }
}

static void task_join(struct list_head *tasklist)
{
    jmp_buf env;

    while (!list_empty(tasklist)) {
        struct task *t = list_first_entry(tasklist, struct task, list);
        list_del(&t->list);
        memcpy(env, t->env, sizeof(jmp_buf));
        free(t);
        longjmp(env, 1);
    }
}

void schedule(void)
{
    static int i;

    srand(0xCAFEBABE ^ (uintptr_t) &schedule); /* Thanks to ASLR */

    setjmp(sched);

    while (ntasks-- > 0) {
        int n = rand() % 5;
        tasks[i++](&n);
        printf("Never reached\n");
    }

    task_join(&tasklist);
}

/* A task yields control n times */

void task0(void *arg)
{
    jmp_buf env;
    static int n;
    n = *(int *) arg;

    printf("Task 0: n = %d\n", n);

    if (setjmp(env) == 0) {
        task_add(&tasklist, env);
        // Jump back to scheduler
        longjmp(sched, 1);
    }

    for (int i = 0; i < n; i++) {
        if (setjmp(env) == 0) {
            task_add(&tasklist, env);
            task_switch(&tasklist);
        }
        printf("Task 0: resume\n");
    }

    printf("Task 0: complete\n");
    longjmp(sched, 1);
}

void task1(void *arg)
{
    jmp_buf env;
    static int n;
    n = *(int *) arg;

    printf("Task 1: n = %d\n", n);

    if (setjmp(env) == 0) {
        task_add(&tasklist, env);
        // Jump back to scheduler
        longjmp(sched, 1);
    }

    for (int i = 0; i < n; i++) {
        if (setjmp(env) == 0) {
            task_add(&tasklist, env);
            task_switch(&tasklist);
        }
        printf("Task 1: resume\n");
    }

    printf("Task 1: complete\n");
    longjmp(sched, 1);
}

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
int main(void)
{
    void (*registered_task[])(void *) = {task0, task1};
    tasks = registered_task;
    ntasks = ARRAY_SIZE(registered_task);

    schedule();

    return 0;
}
