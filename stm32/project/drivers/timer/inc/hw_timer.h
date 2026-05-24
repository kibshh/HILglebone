/**
 * Unified hardware timer driver.
 *
 * Manages all seven general-purpose timers on the STM32F401RE in a
 * single pool.  Each timer can be allocated in one of two exclusive
 * modes:
 *
 *   PULSE  -- one-shot (OPM), ISR-driven revert.  Any free timer of
 *             the requested bit-width is allocated; the caller uses
 *             their user_token as the only identifier.
 *
 *   PWM    -- continuous output, shared by channel.  The caller
 *             picks the timer by hw_timer_id_t (because the GPIO AF
 *             pins the channel to a specific timer).  Multiple channels
 *             on the same timer share the same frequency.
 *
 * A timer in PULSE mode cannot be opened for PWM and vice-versa.
 *
 * All seven timers run at 84 MHz (APB1 timers: clock × 2 multiplier
 * since APB1 prescaler = /2; APB2 timers: no multiplier since APB2
 * prescaler = /1, both give 84 MHz).
 *
 * Timer properties at a glance:
 *
 *   ID  | Timer | Bus  | Width | Channels | APB clock
 *   ----|-------|------|-------|----------|-----------
 *    0  | TIM2  | APB1 | 32    |    4     |  84 MHz
 *    1  | TIM3  | APB1 | 16    |    4     |  84 MHz
 *    2  | TIM4  | APB1 | 16    |    4     |  84 MHz
 *    3  | TIM5  | APB1 | 32    |    4     |  84 MHz
 *    4  | TIM9  | APB2 | 16    |    2     |  84 MHz
 *    5  | TIM10 | APB2 | 16    |    1     |  84 MHz
 *    6  | TIM11 | APB2 | 16    |    1     |  84 MHz
 */

#ifndef HW_TIMER_H
#define HW_TIMER_H

#include "err_codes.h"

#include <stdint.h>

#include "stm32f4xx.h"

/* ── Timer identifiers ────────────────────────────────────────────── */

/* These values are used directly in the PWM wire protocol (timer field
 * in CMD_SETUP_SENSOR).  Do not reorder without updating the spec. */
typedef enum
{
    HW_TIMER_TIM2  = 0,
    HW_TIMER_TIM3,
    HW_TIMER_TIM4,
    HW_TIMER_TIM5,
    HW_TIMER_TIM9,
    HW_TIMER_TIM10,
    HW_TIMER_TIM11,

    HW_TIMER_COUNT,
    HW_TIMER_NONE  = 0xFF,
} hw_timer_id_t;

/* ── Timer width (informational — not used to request a timer) ─────── */

/* Use hw_timer_id_t to request a specific timer for both pulse and PWM.
 * Width is a property of the timer, not a request parameter. */
typedef enum
{
    HW_TIMER_WIDTH_16 = 0,  /* 16-bit ARR: TIM3, TIM4, TIM9, TIM10, TIM11 */
    HW_TIMER_WIDTH_32 = 1,  /* 32-bit ARR: TIM2, TIM5                      */
} hw_timer_width_t;

/* ── Timer clock & counter limits ────────────────────────────────── */

/* All timers in the pool run at 84 MHz (see hw_timer.c for derivation). */
#define HW_TIMER_CLOCK_HZ           84000000UL

/* PSC is always 16-bit on STM32F4, regardless of timer width. */
#define HW_TIMER_PSC_MAX            0xFFFFUL

/* ARR limits by counter width. */
#define HW_TIMER_ARR_MAX_16         0xFFFFUL
#define HW_TIMER_ARR_MAX_32         0xFFFFFFFFUL

/* Maximum meaningful PWM/pulse frequency.  At CLOCK/2 the period is
 * 2 ticks (PSC=0, ARR=1), which is the minimum that still allows at
 * least two CCR values (0% and 100%).  Higher frequencies would give
 * ARR=0, leaving no duty-cycle resolution. */
#define HW_TIMER_FREQ_MAX_HZ        (HW_TIMER_CLOCK_HZ / 2UL)

/* ── Pulse-length caps ────────────────────────────────────────────── */

/* 1 µs/tick (PSC=83 at 84 MHz), ARR bounded by counter width. */
#define HW_TIMER_MAX_PULSE_US_16    HW_TIMER_ARR_MAX_16   /* 65 535 µs */
#define HW_TIMER_MAX_PULSE_US_32    HW_TIMER_ARR_MAX_32   /* ~71 min   */

/* ── Per-timer channel capacity ───────────────────────────────────── */

/* Largest channel count of any single timer in the pool (TIM2-5). */
#define HW_TIMER_MAX_CHANNELS       4U

/* Maximum channel number for a specific timer (1-indexed). */
uint8_t hw_timer_max_channel(hw_timer_id_t id);

/* ── Pulse callback ───────────────────────────────────────────────── */

/* Invoked from interrupt context when a one-shot pulse expires.
 * `user_token` is the value that was passed to hw_timer_pulse_acquire(). */
typedef void (*hw_timer_pulse_cb_t)(uint8_t user_token);

/* ── API ──────────────────────────────────────────────────────────── */

/* One-time startup.  Must be called before any other hw_timer function.
 * Idempotent. */
void hw_timer_init(void);

/* ── Pulse API (exclusive, one-shot, ISR-driven) ──────────────────── */

/* Reserve a specific timer (`id`) for one-shot pulse use and bind it to
 * `user_token`.  The BBB picks the exact timer so it can coordinate with
 * PWM allocation (a timer cannot be shared between pulse and PWM modes).
 * Returns ERR_SUCCESS on success, ERR_INVALID_PARAMETER if `id` is out of range,
 * ERR_PERIPHERAL_BUSY if the timer is already allocated. */
err_code_t hw_timer_pulse_acquire(hw_timer_id_t       id,
                                  uint8_t             user_token,
                                  hw_timer_pulse_cb_t cb);

/* Release the timer bound to `user_token`.  Stops the counter, disables
 * the interrupt, and gates the clock.  No-op if not bound. */
void hw_timer_pulse_release(uint8_t user_token);

/* Maximum pulse_us the timer bound to `user_token` can handle.
 * Returns 0 if not bound. */
uint32_t hw_timer_pulse_max_us(uint8_t user_token);

/* Arm a one-shot pulse of `pulse_us` microseconds.  Restarting an
 * in-flight pulse cancels the old countdown.
 * Returns ERR_SUCCESS, ERR_INVALID_PARAMETER if not bound or pulse_us is
 * out of range. */
err_code_t hw_timer_pulse_start(uint8_t user_token, uint32_t pulse_us);

/* ── PWM API (shared-by-channel, continuous) ──────────────────────── */

/* Open a specific timer for PWM output at `freq_hz`.
 * - If FREE:  configures the timer (PSC+ARR), starts the counter, and
 *             sets the ref-count to 1.
 * - If already in PWM at the SAME frequency: increments ref-count.
 * - If in PWM at a DIFFERENT frequency: returns ERR_PWM_FREQ_CONFLICT.
 * - If in PULSE mode: returns ERR_PERIPHERAL_BUSY.
 * - Invalid id or unachievable frequency: returns ERR_INVALID_PARAMETER.
 *
 * `*out_arr` is filled with the auto-reload value so the caller can
 * compute CCR for a given duty cycle. */
err_code_t hw_timer_pwm_acquire(hw_timer_id_t id,
                                uint32_t      freq_hz,
                                uint32_t     *out_arr);

/* Release one PWM channel's reference on `id`.  Stops the timer and
 * gates the clock when the last channel is released. */
void hw_timer_pwm_release(hw_timer_id_t id);

/* Return the CMSIS peripheral pointer for `id`.  NULL if id is invalid.
 * The caller uses this to write CCMR, CCER, CCR directly. */
TIM_TypeDef *hw_timer_handle(hw_timer_id_t id);

/* Return the current ARR value programmed on `id`.  Only meaningful
 * when the timer is in PWM mode. */
uint32_t hw_timer_pwm_arr(hw_timer_id_t id);

#endif /* HW_TIMER_H */
