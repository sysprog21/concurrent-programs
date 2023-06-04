/* concurrency linked list */

#pragma once

#include <stddef.h>

/* Reuse the RCU from thread-rcu */
#include "../thread-rcu/rcu.h"

#define __allow_unused __attribute__((unused))

#define container_of(ptr, type, member)                         \
    __extension__({                                             \
        const __typeof__(((type *) 0)->member) *__mptr = (ptr); \
        (type *) ((char *) __mptr - offsetof(type, member));    \
    })
#define list_entry_rcu(ptr, type, member) \
    container_of(READ_ONCE(ptr), type, member)

#define list_next_rcu(n) (*((struct list_head __rcu **) (&(n)->next)))

struct list_head {
    struct list_head *next, *prev;
};

static inline void list_init_rcu(struct list_head *node)
{
    node->next = node;
    barrier();
    node->prev = node;
}

static inline void __list_add_rcu(struct list_head *new,
                                  struct list_head *prev,
                                  struct list_head *next)
{
    next->prev = new;
    new->next = next;
    new->prev = prev;
    barrier();
    rcu_assign_pointer(list_next_rcu(prev), new);
}

static inline void list_add_rcu(struct list_head *new, struct list_head *head)
{
    __list_add_rcu(new, head, head->next);
}

static inline void list_add_tail_rcu(struct list_head *new,
                                     struct list_head *head)
{
    __list_add_rcu(new, head->prev, head);
}

static inline void __list_del_rcu(struct list_head *prev,
                                  struct list_head *next)
{
    next->prev = prev;
    barrier();
    rcu_assign_pointer(list_next_rcu(prev), next);
}

static inline void list_del_rcu(struct list_head *node)
{
    __list_del_rcu(node->prev, node->next);
    list_init_rcu(node);
}

/*
 * For the write side only (i.e., it should hold the lock when we use the
 * non-rcu postfix for loop API).
 */

#define list_for_each(n, head) for (n = (head)->next; n != (head); n = n->next)

#define list_for_each_from(pos, head) for (; pos != (head); pos = pos->next)

#define list_for_each_safe(pos, n, head)                   \
    for (pos = (head)->next, n = pos->next; pos != (head); \
         pos = n, n = pos->next)

/* The read side should only use the following API. */

#define list_for_each_entry_rcu(pos, head, member)                     \
    for (pos = list_entry_rcu((head)->next, __typeof__(*pos), member); \
         &pos->member != (head);                                       \
         pos = list_entry_rcu(pos->member.next, __typeof__(*pos), member))
