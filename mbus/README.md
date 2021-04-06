# mbus: concurrent message bus

## Introduction

`mbus` implements a concurrent data structure. Independent clients can register
their callback functions against this bus to receive messages directed to them.
Any user with a reference to the bus can send messages to a registered client
by just knowing the ID of the destination client. Users can also communicate
with all of these clients at once through broadcast messaging.

Even though separate threads can register their own callbacks independently,
when Thread 1 sends Thread 2 a message, Thread 2's callback function is
executed in Thread 1's context. The only way to execute Thread 2's callback
function in that same thread would be to use some sort of low-level
interrupt, which might break the callee's execution flow.

The following diagram illustrates this concept.
```
               +----------+    +-----------------+
               | Thread 1 |    | callbackThread2 |
               +----+-----+    +------------+----+
                    |                ^      |
bus_send(bus,       |                |      +--- queue_push(ctx->queue, msg)
         idThread2, |                |
         msg)       |                |
                    |      +---------+  callbackThread2(ctxThread2, msg)
                    v      |
         +-----------------+-------------------+
         |                mbus                 |
         +-------------------------------------+
               ^    :      :
               | bus_register(bus, idThread2,
               |    :      :  callbackThread2, ctxThread2)
         +-----+------+    :
         |  Thread 2  +--------------> queue_pop(ctx->queue)
         +------------+  loops over
               :    :      :
               :    :      :                                          Time
        -------*----*------*----------------------------------------------->
```

First, Thread 2 registers its callback function with `bus_register`. This
callback function is definitely user-defined, meaning that the user controls
what gets done when a message is sent to Thread 2. After some time, Thread 1
needs to send a message to Thread 2, which is done through `bus_send`. This
redirects Thread 1's execution flow to Thread 2's callback function; as
mentioned above, this gets executed in Thread 1's context, meaning that the
callback function must handle the actual message passing to Thread 2.

The simplest case is to push this message to Thread 2's task queue, but the
user could do something else, like changing a shared variable to signal
a condition.

`mbus` essentially works as a synchronized callback table. This allows for
a very simple implementation while having an extendable usability.
By providing a generic callback API, clients can define specific behavior
to be invoked when a message is sent to them.

## APIs

`bus_send` has two modes of operation, individual and broadcast. Individual
calls a single client's registered callback. Broadcast does the same, but
for every registered client. 

If its callback is set (meaning that it is a registered client), it will be
called with the client's context and the message. The client's context is
a pointer to some opaque data that will be passed to the callback; it is
usually owned by whomever registered that callback. Reading the client is
atomic, meaning that there are no race conditions in which that client is
copied from memory at the same time as that client's information being
freed; in other words, `mbus` are guaranteed that right after calling
`__atomic_load`, client contains valid data.

`bus_unregister` deletes a client's entry in the `bus->clients` array.
This way, no more messages can be sent to it, since the callback will be
set to `NULL`.

The store is atomic, meaning that it cannot happen at the same time as the
load seen in `bus_send`.
