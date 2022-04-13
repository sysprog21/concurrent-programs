#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef int32_t lf_timer_t;
#define LF_TIMER_NULL -1

typedef uint64_t lf_tick_t;
#define LF_TIMER_TICK_INVALID ~UINT64_C(0)

typedef void (*lf_timer_cb)(lf_timer_t tim, lf_tick_t tmo, void *arg);

/** Allocate a timer and associate with the callback and user argument
 * @return LF_TIMER_NULL if no timer available
 */
lf_timer_t lf_timer_alloc(lf_timer_cb cb, void *arg);

/** Free a timer */
void lf_timer_free(lf_timer_t tim);

/** Set (activate) an inactive (expired or cancelled) timer
 * @return false if timer already active
 */
bool lf_timer_set(lf_timer_t tim, lf_tick_t tmo);

/** Reset an active (not yet expired) timer
 * @return false if timer inactive (already expired or cancelled)
 */
bool lf_timer_reset(lf_timer_t tim, lf_tick_t tmo);

/** Cancel (deactivate) an active (not yet expired) timer
 * @return false if timer inactive (already expired or cancelled)
 */
bool lf_timer_cancel(lf_timer_t tim);

/** Return current timer tick */
lf_tick_t lf_timer_tick_get(void);

/** Set current timer tick */
void lf_timer_tick_set(lf_tick_t now);

/** Expire timers <= current tick and invoke callbacks */
void lf_timer_expire(void);
