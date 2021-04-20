# Tinync
Tinync is a simplified implementation of [nc](https://en.wikipedia.org/wiki/Netcat), which aims to demonstrate the usage of [coroutine](https://en.wikipedia.org/wiki/Coroutine) macros and their effects.

## Internals
### Declaring a Coroutine Function
At the very beginning, a coroutine function should be declared first, coroutine function is part of program that will be executed simultaneously with other coroutines. All coroutines should be specified with the macro `cr_proto`, following shows the example:
```cpp
static void cr_proto(coroutine_name, parameter_declarations)
{
    /* coroutine body */
}
```

### Controlling Macros
With in the coroutine, there are several macros for controlling the behavior of coroutine, following are list of them:
* `cr_begin` aims to initiate the context of coroutine when the coroutine is first invoked, or resume previous execution for the rest of invocations. This macro should be placed before other cotrolling macros except `cr_local`.
* `cr_end` ends up a coroutine that will mark the status of coroutine as finished, which could be detected outside to ensure whether a coroutine finisihed its job or not.
* `cr_wait` is used for waiting a condition happened. Once the condition it is waiting for has not happened yet, it will pause the current coroutine and switch to another.
* `cr_exit` not only yields a coroutine but also updates its state with given state.
* `cr_sys` is a wrapper of `cr_wait` which performs waiting on system calls and other functions that return -1 and set `errno`.
* `cr_local` is a marker for programmers to recognize a variable related to coroutine easily.

In `tinync.c`, we can see the combinations of these macros:
```cpp
static void cr_proto(stdin_loop, byte_queue_t *out)
{
    /* b and r are variables used in coroutine whose
     * value will be preserved across pauses.
     */
    cr_local uint8_t b;
    cr_local int r;

    /* Initiates the context of this coroutine. */
    cr_begin();
    for (;;) {
        /* Wait for read system call to become successful. */
        cr_sys(r = read(STDIN_FILENO, &b, 1));
        if (r == 0) {
            /* Wait until queue out is flushed. */
            cr_wait(cr_queue_empty(out));

            /* Exit the coroutine with status update as finished. */
            cr_exit(1);
        }
        /* Wait until there is place in queue out. */
        cr_wait(!cr_queue_full(out));
        cr_queue_push(out, b);
    }
    /* End up this coroutine, status will be updated as finished. */
    cr_end();
}
```

### Coroutine Context
Context is an important part for coroutine, which preserves the execution point of a coroutine that could be resumed later. To define a context for a coroutine, use `cr_context` macro and initiates it with macro `cr_context_init`. It is important to **assign an identical name to context and its corresponding function**. With example presented at the beginning, its corresponding context should be specified as follows:
```cpp
cr_context(coroutine_name) = cr_context_init();
```

### Launching and Monitoring
Now, all required preparations are done, programmers may launch a coroutine via `cr_run` macro and monitor it with `cr_status` macro. To execute several coroutines simultaneously, place `cr_run`s that launch coroutines in a loop and keep tracking their status until all of them are finished. Following shows a simple example:
```cpp
while (cr_status(coroutine_1) != CR_FINISHED &&
       cr_status(coroutine_2) != CR_FINISHED &&
       cr_status(coroutine_3) != CR_FINISHED) {
           cr_run(coroutine_1);
           cr_run(coroutine_2);
           cr_run(coroutine_3);
}
```

## Run the Sample Program
Tinync is a sample program that handles several coroutines to maintain communication with remote while accept user input simultaneously. To compile it, use `make`:
```shell
$ make
```
This sample requires two terminals, let's say `T1` and `T2`. Before launching `tinync`, start `nc` in `T1` first:
```shell
$ nc -l 127.0.0.1 9000
```
Then launch `tinync` in `T2`:
```shell
$ tinync 127.0.0.1 9000
```
Now, any words typed in `T1` will be recived and presented in `T2`, and vice versa.