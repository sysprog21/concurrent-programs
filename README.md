# Complementary Programs for course "Linux Kernel Internals"

This distribution is a collection of programs that are generally unrelated,
except in that they all deal with the concurrent programming. The main
purpose of these programs is to be illustrative and educational.

## Project Listing
* [Coroutine](https://en.wikipedia.org/wiki/Coroutine)
    - [tinync](tinync/): A tiny `nc` implementation using coroutine.
    - [fiber](fiber/): A user-level thread (fiber) using `clone` system call.
    - [preempt\_sched](preempt_sched/): A preemptive userspace multitasking based on a SIGALRM signal.
* Multi-threading Paradigms
    - [tpool](tpool/): A lightweight thread pool.
* [Producer–consumer problem](https://en.wikipedia.org/wiki/Producer%E2%80%93consumer_problem)
    - [spmc](spmc/): A concurrent single-producer/multiple-consumer queue.
    - [mpsc](mpsc/): An unbounded lockless single-consumer/multiple-producer FIFO queue.
    - [mpmc](mpmc/): A multiple-producer/multiple-consumer (MPMC) queue.
    - [channel](channel/): A Linux futex based Go channel implementation.
* [Lock-Free](https://en.wikipedia.org/wiki/Non-blocking_algorithm) Data Structure
    - [ringbuffer](ringbuffer/): A lock-less ring buffer.
    - [ringbuf\_shm](ringbuf_shm/): An optimized lock-free ring buffer with shared memory.
    - [mbus](mbus/): A concurrent message bus.
* [Synchronization](https://en.wikipedia.org/wiki/Synchronization_(computer_science))
    - [rcu\_list](rcu_list/): A concurrent linked list utilizing the simplified RCU algorithm.
    - [qsbr](qsbr/): An implementation of Quiescent state based reclamation (QSBR).
* Applications
    - [httpd](httpd/): A multi-threaded web server.
    - [map-reduce](map-reduce/): word counting using MapReduce.
    - [redirect](redirect/): An I/O multiplexer to monitor stdin redirect using `timerfd` and `epoll`.
    - [picosh](picosh/): A minimalist UNIX shell.

## License

The above projects are released under the BSD 2 clause license.
Use of this source code is governed by a BSD-style license that can be found
in the LICENSE file.
