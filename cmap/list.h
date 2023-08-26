#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "util.h"

/* Doubly-linked list head or element. */
struct list {
    struct list *prev, *next; /* previous/next list element */
};

#define LIST_INITIALIZER(LIST) \
    {                          \
        LIST, LIST             \
    }

/* Static initilizer */
static inline void list_init(struct list *);

/* list insertion */
static inline void list_insert(struct list *, struct list *);
static inline void list_push_back(struct list *, struct list *);

/* list removal */
static inline struct list *list_remove(struct list *);
static inline struct list *list_pop_front(struct list *);

static inline bool list_is_empty(const struct list *);

/* Iterate through the list */
#define LIST_FOREACH(ITER, MEMBER, LIST)             \
    for (INIT_CONTAINER(ITER, (LIST)->next, MEMBER); \
         &(ITER)->MEMBER != (LIST);                  \
         ASSIGN_CONTAINER(ITER, (ITER)->MEMBER.next, MEMBER))

/* Iterate and pop */
#define LIST_FOREACH_POP(ITER, MEMBER, LIST) \
    while (!list_is_empty(LIST) &&           \
           (INIT_CONTAINER(ITER, list_pop_front(LIST), MEMBER), 1))

/* Initializes 'list' as an empty list. */
static inline void list_init(struct list *list)
{
    list->next = list->prev = list;
}

/* Inserts 'elem' just before 'before'. */
static inline void list_insert(struct list *before, struct list *elem)
{
    elem->prev = before->prev;
    elem->next = before;
    before->prev->next = elem;
    before->prev = elem;
}

/* Inserts 'elem' at the end of 'list', so that it becomes the back in
 * 'list'.
 */
static inline void list_push_back(struct list *list, struct list *elem)
{
    list_insert(list, elem);
}

/* Removes 'elem' from its list and returns the element that followed it.
 * Undefined behavior if 'elem' is not in a list.
 */
static inline struct list *list_remove(struct list *elem)
{
    elem->prev->next = elem->next;
    elem->next->prev = elem->prev;
    return elem->next;
}

/* Removes the front element from 'list' and returns it.  Undefined behavior if
 * 'list' is empty before removal.
 */
static inline struct list *list_pop_front(struct list *list)
{
    struct list *front = list->next;
    list_remove(front);
    return front;
}

/* Returns true if 'list' is empty, false otherwise. */
static inline bool list_is_empty(const struct list *list)
{
    return list->next == list;
}
