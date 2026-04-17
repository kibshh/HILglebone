/**
 * I2C sensor emulation backend.
 *
 * For this iteration the backend owns the configuration and register map
 * and handles the protocol lifecycle (SETUP / SET_OUTPUT / STOP); the
 * actual I2C-slave hardware driver is not wired in yet. Adding it later
 * means hooking `i2c_sensor_setup()` into a real peripheral init and
 * letting the slave IRQ read from `i2c_sensor_state_t::regmap`.
 *
 * The on-wire payload layouts mirror `protocol/i2c-sensors-spec.md` §3 and §4;
 * all offsets into those payloads are pulled from the macros below so the
 * spec and the code can be cross-checked side by side.
 */

#ifndef I2C_SENSOR_H
#define I2C_SENSOR_H

#include <stdint.h>

/* ── I2C-specific error codes (slice 0x40..0x5F of the protocol range) ── */

#define ERR_I2C_NO_FREE_PERIPHERAL  0x40U
#define ERR_I2C_CLOCK_UNSUPPORTED   0x41U
#define ERR_I2C_ADDR_CONFLICT       0x42U
#define ERR_I2C_ADDR_RESERVED       0x43U
#define ERR_I2C_REGMAP_TOO_LARGE    0x44U
#define ERR_I2C_BAD_ADDR_MODE       0x45U
#define ERR_I2C_REGISTER_OOB        0x46U
#define ERR_I2C_STRETCH_EXCEEDS_BUS 0x47U
#define ERR_I2C_SMBUS_REQUIRED      0x48U
#define ERR_I2C_UNSUPPORTED_FEATURE 0x49U

/* ── Capacity ─────────────────────────────────────────────────────── */

/* STM32F401RE has three I2C peripherals (I2C1/2/3), all wired to the same
 * bus reaching the DUT; one sensor per peripheral. Section 2 of the spec. */
#define I2C_SENSOR_MAX_COUNT        3U

/* Hard cap on register-map size per sensor. Worst case 3 × 1024 = 3 KB
 * out of a 32 KB FreeRTOS heap, leaving room for SPI and future backends. */
#define I2C_SENSOR_REGMAP_MAX       1024U

/* STM32F401 tops out at Fast-mode 400 kHz (no Fm+). */
#define I2C_SENSOR_CLOCK_MIN_HZ     10000U
#define I2C_SENSOR_CLOCK_MAX_HZ     400000U

/* 7-bit address space (NXP UM10204 Table 4). */
#define I2C_7BIT_ADDR_MAX           0x7FU
#define I2C_7BIT_RESERVED_LOW       0x07U   /* 0x00..0x07 reserved */
#define I2C_7BIT_RESERVED_HIGH      0x78U   /* 0x78..0x7F reserved */

/* 10-bit address space (no reserved ranges). */
#define I2C_10BIT_ADDR_MAX          0x3FFU

/* ── CMD_SETUP_SENSOR I2C config layout (after the generic protocol_id byte) ── */

#define I2C_CFG_OFFSET_CLOCK_HZ             0U      /* u32 LE */
#define I2C_CFG_OFFSET_ADDRESS_MODE         4U      /* u8    */
#define I2C_CFG_OFFSET_PRIMARY_ADDR         5U      /* u16 LE */
#define I2C_CFG_OFFSET_SECONDARY_ADDR       7U      /* u16 LE */
#define I2C_CFG_OFFSET_FLAGS                9U      /* u8    */
#define I2C_CFG_OFFSET_REG_ADDR_WIDTH       10U     /* u8    */
#define I2C_CFG_OFFSET_REG_ADDR_ENDIAN      11U     /* u8    */
#define I2C_CFG_OFFSET_AUTO_INC_MODE        12U     /* u8    */
#define I2C_CFG_OFFSET_REGISTER_COUNT       13U     /* u16 LE */
#define I2C_CFG_OFFSET_RESPONSE_DELAY_US    15U     /* u16 LE */
#define I2C_CFG_OFFSET_CLOCK_STRETCH_MAX_US 17U     /* u16 LE */
#define I2C_CFG_OFFSET_HAS_PRESET           19U     /* u8    */
#define I2C_CFG_OFFSET_PRESET_REG_START     20U     /* u16 LE (present only if has_preset) */
#define I2C_CFG_OFFSET_PRESET_VALUES_LEN    22U     /* u16 LE (present only if has_preset) */
#define I2C_CFG_OFFSET_PRESET_VALUES        24U     /* N bytes (present only if has_preset) */

#define I2C_CFG_HAS_PRESET_YES              1U
#define I2C_CFG_HAS_PRESET_NO               0U

#define I2C_CFG_SIZE_NO_PRESET              20U     /* bytes, when has_preset = 0 */
#define I2C_CFG_SIZE_WITH_PRESET_HEADER     24U     /* bytes before preset_values */

/* ── Flags bitfield ───────────────────────────────────────────────── */

#define I2C_FLAG_GENERAL_CALL_ENABLE        (1U << 0)
#define I2C_FLAG_SMBUS_MODE                 (1U << 1)
#define I2C_FLAG_PEC_REQUIRED               (1U << 2)
#define I2C_FLAG_AUTO_INC_WRAP              (1U << 3)
#define I2C_FLAG_DUT_WRITES_ALLOWED         (1U << 4)
#define I2C_FLAG_INTERNAL_PULLUPS           (1U << 5)
#define I2C_FLAG_CLOCK_STRETCH_ENABLE       (1U << 6)
#define I2C_FLAG_RESERVED_MASK              (1U << 7)

/* ── Field enumerants ─────────────────────────────────────────────── */

#define I2C_ADDRESS_MODE_7BIT               0U
#define I2C_ADDRESS_MODE_10BIT              1U

#define I2C_REG_ADDR_WIDTH_NONE             0U
#define I2C_REG_ADDR_WIDTH_8                1U
#define I2C_REG_ADDR_WIDTH_16               2U

#define I2C_REG_ADDR_ENDIAN_BIG             0U
#define I2C_REG_ADDR_ENDIAN_LITTLE          1U

#define I2C_AUTO_INC_MODE_NONE              0U
#define I2C_AUTO_INC_MODE_READ              1U
#define I2C_AUTO_INC_MODE_WRITE             2U
#define I2C_AUTO_INC_MODE_BOTH              3U

/* ── CMD_SET_OUTPUT I2C value layout (after the generic sensor_id byte) ── */

#define I2C_SET_OUTPUT_OFFSET_REG_START     0U      /* u16 LE */
#define I2C_SET_OUTPUT_OFFSET_VALUE_LEN     2U      /* u16 LE */
#define I2C_SET_OUTPUT_OFFSET_VALUES        4U      /* N bytes */
#define I2C_SET_OUTPUT_HEADER_SIZE          4U

/* ── API ──────────────────────────────────────────────────────────── */

/* One-time startup. Must run before any setup call. */
void i2c_sensor_init(void);

/* Handle CMD_SETUP_SENSOR for protocol_id = I2C.
 *   `cfg`     = payload bytes starting AFTER the protocol_id byte
 *   `cfg_len` = length of that slice
 *   `out_sensor_id` = filled with the newly-allocated id on success, or
 *                     PROTO_SENSOR_ID_NONE on failure
 * Returns ERR_SUCCESS or a common/I2C-specific error code. */
uint8_t i2c_sensor_setup(const uint8_t *cfg, uint16_t cfg_len, uint8_t *out_sensor_id);

/* Handle CMD_SET_OUTPUT for an I2C sensor.
 *   `internal_id`  = index returned by i2c_sensor_setup
 *   `values`       = payload bytes starting AFTER the generic sensor_id byte
 *   `values_len`   = length of that slice
 * Returns ERR_SUCCESS or an error code. */
uint8_t i2c_sensor_set_output(uint8_t internal_id, const uint8_t *values, uint16_t values_len);

/* Handle CMD_STOP_SENSOR for an I2C sensor. Frees peripheral slot + reg map. */
uint8_t i2c_sensor_stop(uint8_t internal_id);

#endif /* I2C_SENSOR_H */
