#pragma once

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(CONDITION) __builtin_expect(!!(CONDITION), 1)
#else
#define LIKELY(CONDITION) (!!(CONDITION))
#endif

/* Expands to a string that looks like "<file>:<line>", e.g. "tmp.c:10".
 *
 * See http://c-faq.com/ansi/stringize.html for an explanation of STRINGIZE
 * and STRINGIZE2.
 */
#define SOURCE_LOCATOR __FILE__ ":" STRINGIZE(__LINE__)
#define STRINGIZE(ARG) STRINGIZE2(ARG)
#define STRINGIZE2(ARG) #ARG

/* Bit masks */
#define BITMASK_8 0xff
#define BITMASK_16 0xffff
#define BITMASK_32 0xffffffff

/* Cacheline marking is typically done using zero-sized array. */
typedef uint8_t CACHE_LINE_MARKER[1];

/* This is a void expression that issues a compiler error if POINTER cannot be
 * compared for equality with the given pointer TYPE.  This generally means
 * that POINTER is a qualified or unqualified TYPE.  However,
 * BUILD_ASSERT_TYPE(POINTER, void *) will accept any pointer to object type,
 * because any pointer to object can be compared for equality with "void *".
 *
 * POINTER can be any expression.  The use of "sizeof" ensures that the
 * expression is not actually evaluated, so that any side effects of the
 * expression do not occur.
 *
 * The cast to int is present only to suppress an "expression using sizeof
 * bool" warning from "sparse" (see
 * http://permalink.gmane.org/gmane.comp.parsers.sparse/2967).
 */
#define BUILD_ASSERT_TYPE(POINTER, TYPE) \
    ((void) sizeof((int) ((POINTER) == (TYPE) (POINTER))))

/* Casts 'pointer' to 'type' and issues a compiler warning if the cast changes
 * anything other than an outermost "const" or "volatile" qualifier. */
#define CONST_CAST(TYPE, POINTER) \
    (BUILD_ASSERT_TYPE(POINTER, TYPE), (TYPE) (POINTER))

/* Given OBJECT of type pointer-to-structure, expands to the offset of MEMBER
 * within an instance of the structure.
 *
 * The GCC-specific version avoids the technicality of undefined behavior if
 * OBJECT is null, invalid, or not yet initialized.  This makes some static
 * checkers (like Coverity) happier.  But the non-GCC version does not actually
 * dereference any pointer, so it would be surprising for it to cause any
 * problems in practice.
 */
#if defined(__GNUC__) || defined(__clang__)
#define OBJECT_OFFSETOF(OBJECT, MEMBER) offsetof(typeof(*(OBJECT)), MEMBER)
#else
#define OBJECT_OFFSETOF(OBJECT, MEMBER) \
    ((char *) &(OBJECT)->MEMBER - (char *) (OBJECT))
#endif

/* Given a pointer-typed lvalue OBJECT, expands to a pointer type that may be
 * assigned to OBJECT. */
#ifdef __GNUC__
#define TYPEOF(OBJECT) typeof(OBJECT)
#else
#define TYPEOF(OBJECT) void *
#endif

/* Given POINTER, the address of the given MEMBER within an object of the type
 * that that OBJECT points to, returns OBJECT as an assignment-compatible
 * pointer type (either the correct pointer type or "void *").  OBJECT must be
 * an lvalue.
 *
 * This is the same as CONTAINER_OF except that it infers the structure type
 * from the type of '*OBJECT'.
 */
#define OBJECT_CONTAINING(POINTER, OBJECT, MEMBER)                          \
    ((TYPEOF(OBJECT)) (void *) ((char *) (POINTER) -OBJECT_OFFSETOF(OBJECT, \
                                                                    MEMBER)))

/* Given POINTER, the address of the given MEMBER within an object of the type
 * that that OBJECT points to, assigns the address of the outer object to
 * OBJECT, which must be an lvalue.
 *
 * Evaluates to (void) 0 as the result is not to be used.
 */
#define ASSIGN_CONTAINER(OBJECT, POINTER, MEMBER) \
    ((OBJECT) = OBJECT_CONTAINING(POINTER, OBJECT, MEMBER), (void) 0)

/* As explained in the comment above OBJECT_OFFSETOF(), non-GNUC compilers
 * like MSVC will complain about un-initialized variables if OBJECT
 * hasn't already been initialized. To prevent such warnings, INIT_CONTAINER()
 * can be used as a wrapper around ASSIGN_CONTAINER.
 */
#define INIT_CONTAINER(OBJECT, POINTER, MEMBER) \
    ((OBJECT) = NULL, ASSIGN_CONTAINER(OBJECT, POINTER, MEMBER))

/* Yields the size of MEMBER within STRUCT. */
#define MEMBER_SIZEOF(STRUCT, MEMBER) (sizeof(((STRUCT *) NULL)->MEMBER))

/* Yields the offset of the end of MEMBER within STRUCT. */
#define OFFSETOFEND(STRUCT, MEMBER) \
    (offsetof(STRUCT, MEMBER) + MEMBER_SIZEOF(STRUCT, MEMBER))

/* Given POINTER, the address of the given MEMBER in a STRUCT object, returns
 *  the STRUCT object.
 */
#define CONTAINER_OF(POINTER, STRUCT, MEMBER) \
    ((STRUCT *) (void *) ((char *) (POINTER) -offsetof(STRUCT, MEMBER)))

/* Returns the address after the object pointed by POINTER casted to TYPE */
#define OBJECT_END(TYPE, POINTER) (TYPE)((char *) POINTER + sizeof(*POINTER))

/* Returns the size in bytes of array with NUM elements of type TYPE */
#define ARRAY_SIZE(TYPE, NUM) (sizeof(TYPE) * NUM)

/* Like the standard assert macro, except always evaluates the condition,
 * even with NDEBUG.
 */
#ifndef NDEBUG
#define ASSERT(CONDITION) (LIKELY(CONDITION) ? (void) 0 : assert(0))
#else
#define ASSERT(CONDITION) ((void) (CONDITION))
#endif

#include <stdio.h>
#include <stdlib.h>

static inline void abort_msg(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    abort();
}

static inline void *xmalloc(size_t size)
{
    void *p = malloc(size ? size : 1);
    if (!p)
        abort_msg("Out of memory");
    return p;
}
