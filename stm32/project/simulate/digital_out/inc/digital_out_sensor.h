/**
 * Digital-output sensor backend.
 *
 * Drives a single GPIO pin per sensor as a discrete signal line that the
 * DUT sees (DRDY, INT, button, enable, ...). The backend handles
 * SETUP / SET_OUTPUT / STOP and schedules optional one-shot pulses via
 * FreeRTOS software timers.
 *
 * Wire layouts mirror docs/protocol/digital-io-spec.md §3 and §4.
 */

#ifndef DIGITAL_OUT_SENSOR_H
#define DIGITAL_OUT_SENSOR_H

#include <stdint.h>

/* ── Capacity ─────────────────────────────────────────────────────── */

/* Same cap as the global sensor table -- if the global table fills up
 * first, the global cap wins. This number sizes our per-backend pool. */
#define DIGITAL_OUT_SENSOR_MAX_COUNT    8U

/* ── Port / pin enums (wire values) ───────────────────────────────── */

/* GPIO ports available on STM32F401RE. F and G are not present on this
 * chip, so they are intentionally absent from this enum. */
#define DIGITAL_OUT_PORT_A              0U
#define DIGITAL_OUT_PORT_B              1U
#define DIGITAL_OUT_PORT_C              2U
#define DIGITAL_OUT_PORT_D              3U
#define DIGITAL_OUT_PORT_E              4U
#define DIGITAL_OUT_PORT_H              5U
#define DIGITAL_OUT_PORT_MAX            DIGITAL_OUT_PORT_H

#define DIGITAL_OUT_PIN_MAX             15U

/* Output type: push-pull or open-drain. */
#define DIGITAL_OUT_TYPE_PUSH_PULL      0U
#define DIGITAL_OUT_TYPE_OPEN_DRAIN     1U
#define DIGITAL_OUT_TYPE_MAX            DIGITAL_OUT_TYPE_OPEN_DRAIN

/* Slew rate / drive strength (matches GPIO_OSPEEDR encoding 0..3). */
#define DIGITAL_OUT_SPEED_LOW           0U
#define DIGITAL_OUT_SPEED_MEDIUM        1U
#define DIGITAL_OUT_SPEED_HIGH          2U
#define DIGITAL_OUT_SPEED_VERY_HIGH     3U
#define DIGITAL_OUT_SPEED_MAX           DIGITAL_OUT_SPEED_VERY_HIGH

/* Pull configuration (matches GPIO_PUPDR encoding 0..2). */
#define DIGITAL_OUT_PULL_NONE           0U
#define DIGITAL_OUT_PULL_UP             1U
#define DIGITAL_OUT_PULL_DOWN           2U
#define DIGITAL_OUT_PULL_MAX            DIGITAL_OUT_PULL_DOWN

/* Pulse timer kind: pinned at setup, fixed for the sensor's lifetime.
 *   SOFTWARE -- FreeRTOS xTimer; ~1 ms resolution; pulse duration unbounded.
 *   HARDWARE -- one of the dedicated TIMs reserved at setup; 1 µs
 *               resolution; pulse-duration cap depends on `pulse_range`. */
#define DIGITAL_OUT_TIMER_SOFTWARE      0U
#define DIGITAL_OUT_TIMER_HARDWARE      1U
#define DIGITAL_OUT_TIMER_KIND_MAX      DIGITAL_OUT_TIMER_HARDWARE

/* Pulse-range sub-pool selector. Only consulted when timer_kind = HARDWARE.
 *   SHORT -- 16-bit timer (TIM9/10/11); pulses up to 65 535 µs.
 *   LONG  -- 32-bit timer (TIM2/5);     pulses up to ~71 minutes. */
#define DIGITAL_OUT_PULSE_RANGE_SHORT   0U
#define DIGITAL_OUT_PULSE_RANGE_LONG    1U
#define DIGITAL_OUT_PULSE_RANGE_MAX     DIGITAL_OUT_PULSE_RANGE_LONG

/* Logical levels. */
#define DIGITAL_OUT_LEVEL_LOW           0U
#define DIGITAL_OUT_LEVEL_HIGH          1U

/* ── CMD_SETUP_SENSOR payload (after the generic protocol_id byte) ── */

#define DIGITAL_OUT_CFG_OFFSET_PORT             0U      /* u8 */
#define DIGITAL_OUT_CFG_OFFSET_PIN              1U      /* u8 */
#define DIGITAL_OUT_CFG_OFFSET_INITIAL_LEVEL    2U      /* u8 */
#define DIGITAL_OUT_CFG_OFFSET_OUTPUT_TYPE      3U      /* u8 */
#define DIGITAL_OUT_CFG_OFFSET_SPEED            4U      /* u8 */
#define DIGITAL_OUT_CFG_OFFSET_PULL             5U      /* u8 */
#define DIGITAL_OUT_CFG_OFFSET_TIMER_KIND       6U      /* u8 */
#define DIGITAL_OUT_CFG_OFFSET_PULSE_RANGE      7U      /* u8 (used iff timer_kind=HARDWARE) */
#define DIGITAL_OUT_CFG_SIZE                    8U

/* ── CMD_SET_OUTPUT payload (after the generic sensor_id byte) ───── */

#define DIGITAL_OUT_SET_OUTPUT_OFFSET_LEVEL     0U      /* u8 */
#define DIGITAL_OUT_SET_OUTPUT_OFFSET_PULSE_US  1U      /* u32 LE */
#define DIGITAL_OUT_SET_OUTPUT_SIZE             5U

/* ── API ──────────────────────────────────────────────────────────── */

/* One-time startup. Must run before any setup call. */
void digital_out_sensor_init(void);

/* Handle CMD_SETUP_SENSOR for protocol_id = DIGITAL_OUT.
 *   `cfg`           = payload bytes starting AFTER the protocol_id byte
 *   `cfg_len`       = length of that slice
 *   `out_sensor_id` = filled with the newly-allocated id on success, or
 *                     PROTO_SENSOR_ID_NONE on failure
 * Returns ERR_SUCCESS or a common error code. */
uint8_t digital_out_sensor_setup(const uint8_t *cfg,
                                 uint16_t       cfg_len,
                                 uint8_t       *out_sensor_id);

/* Handle CMD_SET_OUTPUT for a digital-output sensor.
 *   `internal_id`  = index returned by digital_out_sensor_setup
 *   `values`       = payload bytes starting AFTER the generic sensor_id byte
 *   `values_len`   = length of that slice
 * Returns ERR_SUCCESS or an error code. */
uint8_t digital_out_sensor_set_output(uint8_t        internal_id,
                                      const uint8_t *values,
                                      uint16_t       values_len);

/* Handle CMD_STOP_SENSOR for a digital-output sensor. Cancels any
 * in-flight pulse, reverts the pin to floating input, and frees the slot. */
uint8_t digital_out_sensor_stop(uint8_t internal_id);

#endif /* DIGITAL_OUT_SENSOR_H */
