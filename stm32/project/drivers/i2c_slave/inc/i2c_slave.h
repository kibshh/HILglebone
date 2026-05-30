/**
 * I2C slave driver — ISR-driven, register-map-backed.
 *
 * The DUT is the I2C master; the STM32 responds to address matches by
 * serving bytes from a caller-supplied register map, or accepting writes
 * into it (when writes_allowed).  The register map is owned by the caller
 * and must remain valid until i2c_slave_deinit().
 *
 * GPIO configuration for SDA and SCL (open-drain AF) must be done by
 * the caller BEFORE calling i2c_slave_init().
 */

#ifndef I2C_SLAVE_H
#define I2C_SLAVE_H

#include <stdbool.h>
#include <stdint.h>

#include "err_codes.h"

/* ── Peripheral identifiers ───────────────────────────────────────── */

typedef enum
{
    I2C_PERIPH_I2C1 = 0,
    I2C_PERIPH_I2C2 = 1,
    I2C_PERIPH_I2C3 = 2,

    I2C_PERIPH_COUNT,
} i2c_periph_t;

/* ── Register-map configuration ───────────────────────────────────── */

/* Addressing mode — intentionally match I2C_ADDRESS_MODE_* in i2c_sensor.h. */
#define I2C_SLAVE_ADDR_7BIT     0U
#define I2C_SLAVE_ADDR_10BIT    1U

/* Register address width — match I2C_REG_ADDR_WIDTH_*. */
#define I2C_SLAVE_REG_NONE      0U   /* streaming: no register address phase */
#define I2C_SLAVE_REG_8         1U   /* 8-bit register address */
#define I2C_SLAVE_REG_16        2U   /* 16-bit register address */

/* Auto-increment mode — match I2C_AUTO_INC_MODE_*. */
#define I2C_SLAVE_AUTO_INC_NONE  0U
#define I2C_SLAVE_AUTO_INC_READ  1U
#define I2C_SLAVE_AUTO_INC_WRITE 2U
#define I2C_SLAVE_AUTO_INC_BOTH  3U

typedef struct
{
    /* Register map pointer — not owned; must remain valid until deinit. */
    uint8_t  *regmap;
    uint16_t  regmap_len;

    uint8_t   reg_addr_width;   /* I2C_SLAVE_REG_NONE / _8 / _16 */
    uint8_t   auto_inc_mode;    /* I2C_SLAVE_AUTO_INC_* */
    bool      auto_inc_wrap;    /* wrap at regmap_len-1; else saturate */
    bool      writes_allowed;   /* accept DUT writes into regmap */
    bool      general_call;     /* respond to the general-call address (0x00) */
    bool      clock_stretch;    /* enable hardware clock stretching (NOSTRETCH=0) */
} i2c_slave_cfg_t;

/* ── API ──────────────────────────────────────────────────────────── */

/* Initialise the I2C peripheral in slave mode.
 * secondary_addr = 0 disables dual-address (7-bit mode only).
 * Returns ERR_SUCCESS, ERR_PERIPHERAL_BUSY if already claimed,
 * or ERR_INVALID_PARAMETER. */
err_code_t i2c_slave_init(i2c_periph_t          periph,
                          uint32_t              clock_hz,
                          uint8_t               addr_mode,
                          uint16_t              primary_addr,
                          uint16_t              secondary_addr,
                          const i2c_slave_cfg_t *cfg);

/* Disable the I2C peripheral and release the slot. */
err_code_t i2c_slave_deinit(i2c_periph_t periph);

#endif /* I2C_SLAVE_H */
