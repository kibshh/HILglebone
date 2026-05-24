/**
 * PWM output sensor backend.
 *
 * Drives a GPIO pin with a continuous hardware-PWM signal via any of
 * the timers managed by the shared hw_timer driver (TIM2..TIM11).
 * 32-bit timers (TIM2, TIM5) allow higher ARR values for better duty
 * resolution at low frequencies; 16-bit timers (TIM3, TIM4, TIM9,
 * TIM10, TIM11) cap at 65 535 counts.
 *
 * Wire layouts mirror docs/protocol/pwm-spec.md §3 and §4.
 */

#ifndef PWM_SENSOR_H
#define PWM_SENSOR_H

#include <stdint.h>

#include "err_codes.h"
#include "hw_timer.h"

/* ── Capacity ─────────────────────────────────────────────────────── */

/* Internal slot pool size.  slot_for() maps (timer_id, channel) with a
 * flat stride of HW_TIMER_MAX_CHANNELS, so the states array must cover
 * HW_TIMER_COUNT * HW_TIMER_MAX_CHANNELS indices even though some are
 * unreachable (e.g. TIM9 ch3/4, TIM10/11 ch2-4).  Unreachable entries
 * are never populated (in_use stays false).
 * Real capacity: TIM2(4)+TIM3(4)+TIM4(4)+TIM5(4)+TIM9(2)+TIM10(1)+TIM11(1) = 20. */
#define PWM_SLOT_POOL_SIZE          (HW_TIMER_COUNT * HW_TIMER_MAX_CHANNELS)

/* ── Timer field (wire value = hw_timer_id_t) ─────────────────────── */

/* The `timer` byte in the setup payload maps directly to hw_timer_id_t.
 * All seven timers in the pool are available for PWM. */
#define PWM_TIMER_MIN_VAL           ((uint8_t)HW_TIMER_TIM2)
#define PWM_TIMER_MAX_VAL           ((uint8_t)(HW_TIMER_COUNT - 1U))

/* ── Channel bounds ───────────────────────────────────────────────── */

#define PWM_CHANNEL_MIN             1U
#define PWM_CHANNEL_MAX             4U   /* max of any single timer */

/* ── Frequency bounds ─────────────────────────────────────────────── */

#define PWM_FREQ_MIN_HZ             1U
#define PWM_FREQ_MAX_HZ             1000000U

/* ── Duty-cycle bounds ────────────────────────────────────────────── */

#define PWM_DUTY_MAX                10000U   /* 100.00 % */

/* ── CMD_SETUP_SENSOR payload (after the generic protocol_id byte) ── */

#define PWM_CFG_OFFSET_PORT             0U      /* u8  */
#define PWM_CFG_OFFSET_PIN              1U      /* u8  */
#define PWM_CFG_OFFSET_AF               2U      /* u8  */
#define PWM_CFG_OFFSET_TIMER            3U      /* u8  = hw_timer_id_t */
#define PWM_CFG_OFFSET_CHANNEL          4U      /* u8  */
#define PWM_CFG_OFFSET_FREQ_HZ          5U      /* u32 LE */
#define PWM_CFG_OFFSET_DUTY_PCT_X100    9U      /* u16 LE */
#define PWM_CFG_SIZE                    11U

/* ── CMD_SET_OUTPUT payload (after the generic sensor_id byte) ───── */

#define PWM_SET_OUTPUT_OFFSET_DUTY      0U      /* u16 LE */
#define PWM_SET_OUTPUT_SIZE             2U

/* ── API ──────────────────────────────────────────────────────────── */

/* One-time startup. Must run before any setup call. */
void pwm_sensor_init(void);

/* Handle CMD_SETUP_SENSOR for protocol_id = PWM.
 *   `cfg`           = payload bytes starting AFTER the protocol_id byte
 *   `cfg_len`       = length of that slice
 *   `out_sensor_id` = filled with the newly-allocated id on success, or
 *                     PROTO_SENSOR_ID_NONE on failure
 * Returns ERR_SUCCESS or a common / PWM-specific error code. */
err_code_t pwm_sensor_setup(const uint8_t *cfg,
                             uint16_t       cfg_len,
                             uint8_t       *out_sensor_id);

/* Handle CMD_SET_OUTPUT for a PWM sensor.  Updates duty cycle only;
 * frequency is pinned at setup time.
 *   `internal_id` = index returned by pwm_sensor_setup
 *   `values`      = payload bytes starting AFTER the generic sensor_id byte
 *   `values_len`  = length of that slice
 * Returns ERR_SUCCESS or an error code. */
err_code_t pwm_sensor_set_output(uint8_t        internal_id,
                                  const uint8_t *values,
                                  uint16_t       values_len);

/* Handle CMD_STOP_SENSOR.  Disables the PWM channel, reverts the pin to
 * floating input, and releases the timer back to the shared hw_timer pool. */
err_code_t pwm_sensor_stop(uint8_t internal_id);

#endif /* PWM_SENSOR_H */
