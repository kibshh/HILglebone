/**
 * Hardware-timer pool for digital-output one-shot pulses.
 *
 * Owns five general-purpose timers split into two independent
 * sub-pools, and hands them out to digital_out sensors that requested
 * timer_kind = hardware at setup time:
 *
 *   - SHORT sub-pool (3 slots): TIM9, TIM10, TIM11
 *       16-bit ARR -> pulses cap at 65 535 µs (~65 ms).
 *   - LONG sub-pool (2 slots): TIM2, TIM5
 *       32-bit ARR -> pulses cap at ~4.29 × 10^9 µs (~71 minutes).
 *
 * The caller picks the sub-pool via the `kind` argument to acquire().
 * Allocation is strict: if the requested sub-pool is exhausted,
 * acquire() returns false -- it does NOT silently fall back to the
 * other sub-pool. This keeps scarce 32-bit slots reserved for sensors
 * that genuinely need them.
 *
 * The `internal_id` passed to acquire() is the only identifier the
 * caller needs. It is stored inside the module and used as the key for
 * all subsequent operations (release, start, get_max_pulse_us). The
 * concrete slot index is an implementation detail hidden here.
 *
 * Each slot is configured for one-shot mode at 1 µs / tick (PSC = 83
 * on the shared 84 MHz timer clock). When the counter reaches its
 * auto-reload value the timer fires UIF and the slot's registered
 * callback is invoked.
 *
 * The callbacks run in interrupt context. Keep them short -- the
 * digital_out backend just writes BSRR (atomic) to flip the pin back.
 */

#ifndef DIGITAL_OUT_PULSE_TIMER_H
#define DIGITAL_OUT_PULSE_TIMER_H

#include <stdbool.h>
#include <stdint.h>

/* ── Pool sizing & slot indices ───────────────────────────────────── */

/* Slot index = position into the internal slot table. Trailing
 * PULSE_TIMER_HW_MAX_COUNT auto-counts the entries above it -- adding
 * a TIM here automatically grows the pool size everywhere. */
typedef enum
{
    PULSE_TIMER_HW_SLOT_TIM2  = 0,
    PULSE_TIMER_HW_SLOT_TIM5,
    PULSE_TIMER_HW_SLOT_TIM9,
    PULSE_TIMER_HW_SLOT_TIM10,
    PULSE_TIMER_HW_SLOT_TIM11,

    PULSE_TIMER_HW_MAX_COUNT,
} pulse_timer_hw_slot_t;

/* Per-sub-pool pulse-length caps, exposed so callers can validate
 * pulse_us against the cap for their chosen `kind` before calling start(). */
#define PULSE_TIMER_HW_MAX_PULSE_US_16 0xFFFFUL          /* 65 535 µs */
#define PULSE_TIMER_HW_MAX_PULSE_US_32 0xFFFFFFFFUL      /* ~71 min   */

/* Which sub-pool to draw from. Caller-selected, never auto-fallback. */
typedef enum
{
    PULSE_TIMER_HW_KIND_SHORT = 0,   /* 16-bit timer (TIM9/10/11) */
    PULSE_TIMER_HW_KIND_LONG  = 1,   /* 32-bit timer (TIM2/5)     */
} pulse_timer_hw_kind_t;

/* Callback signature: invoked from interrupt context when the pulse
 * expires. `internal_id` is the value that was passed to acquire(). */
typedef void (*pulse_timer_hw_callback_t)(uint8_t internal_id);

/* ── API ──────────────────────────────────────────────────────────── */

/* One-time startup. Zeros the ownership table. Idempotent. */
void pulse_timer_hw_init(void);

/* Reserve a free hardware timer slot from the requested sub-pool and
 * bind it to `internal_id`.
 *   `internal_id` -- key used for all subsequent calls; also passed
 *                    back to `cb` when the pulse fires
 *   `kind`        -- SHORT (16-bit) or LONG (32-bit); strict, no
 *                    fallback if the sub-pool is exhausted
 *   `cb`          -- ISR-context callback to run on UIF
 * Returns true on success, false if the requested sub-pool is full. */
bool pulse_timer_hw_acquire(uint8_t                   internal_id,
                            pulse_timer_hw_kind_t     kind,
                            pulse_timer_hw_callback_t cb);

/* Release the slot bound to `internal_id`. Stops the timer if running,
 * disables its interrupt, and clears the binding. Safe to call from
 * task context (not ISR). No-op if `internal_id` is not bound. */
void pulse_timer_hw_release(uint8_t internal_id);

/* Maximum pulse_us allowed for the slot bound to `internal_id`.
 * Returns 0 if not bound. Use this to validate pulse_us BEFORE
 * calling start(); start() asserts the bound. */
uint32_t pulse_timer_hw_get_max_pulse_us(uint8_t internal_id);

/* Arm a one-shot pulse of `pulse_us` microseconds on the slot bound to
 * `internal_id`. `pulse_us` MUST be in the range
 * 1..get_max_pulse_us(internal_id); callers are responsible for
 * checking. Restarting an in-flight pulse is allowed -- the previous
 * countdown is cancelled.
 *
 * Returns true on success, false if `internal_id` has no bound slot
 * (e.g. the sensor was stopped between validation and this call).
 * The callback registered at acquire() will fire once after pulse_us
 * elapses. */
bool pulse_timer_hw_start(uint8_t internal_id, uint32_t pulse_us);

#endif /* DIGITAL_OUT_PULSE_TIMER_H */
