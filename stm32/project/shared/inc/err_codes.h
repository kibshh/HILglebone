#ifndef ERR_CODES_H
#define ERR_CODES_H

/* Unified error / status codes.
 *
 * Values are the wire-protocol error_code byte sent in RSP_ACK.
 * Every driver and sensor backend returns err_code_t directly — no
 * translation layer is needed between firmware internals and the wire. */

typedef enum
{
    /* ── Common (0x00..0x0D) ──────────────────────────────────────── */
    ERR_SUCCESS                 = 0x00U,
    ERR_UNKNOWN_CMD             = 0x01U,
    ERR_MALFORMED_PAYLOAD       = 0x02U,
    ERR_BAD_CRC                 = 0x03U,
    ERR_UNSUPPORTED             = 0x04U,
    ERR_OUT_OF_RESOURCES        = 0x05U,
    ERR_INVALID_SENSOR_ID       = 0x06U,
    ERR_INVALID_PARAMETER       = 0x07U,
    ERR_PERIPHERAL_BUSY         = 0x08U,
    ERR_PIN_CONFLICT            = 0x09U,
    ERR_NOT_IMPLEMENTED         = 0x0AU,
    ERR_INTERNAL                = 0x0BU,
    ERR_TIMEOUT                 = 0x0CU,
    ERR_EMPTY                   = 0x0DU,

    /* ── I2C-specific (0x40..0x49) ────────────────────────────────── */
    ERR_I2C_NO_FREE_PERIPHERAL  = 0x40U,
    ERR_I2C_CLOCK_UNSUPPORTED   = 0x41U,
    ERR_I2C_ADDR_CONFLICT       = 0x42U,
    ERR_I2C_ADDR_RESERVED       = 0x43U,
    ERR_I2C_REGMAP_TOO_LARGE    = 0x44U,
    ERR_I2C_BAD_ADDR_MODE       = 0x45U,
    ERR_I2C_REGISTER_OOB        = 0x46U,
    ERR_I2C_STRETCH_EXCEEDS_BUS = 0x47U,
    ERR_I2C_SMBUS_REQUIRED      = 0x48U,
    ERR_I2C_UNSUPPORTED         = 0x49U,

    /* ── PWM-specific (0x60..0x61) ────────────────────────────────── */
    ERR_PWM_FREQ_CONFLICT       = 0x60U,
    ERR_PWM_CHANNEL_IN_USE      = 0x61U,

    /* ── DAC-specific (0x80..0x82) ────────────────────────────────── */
    ERR_DAC_PIN_MISMATCH        = 0x80U,
    ERR_DAC_CLOCK_MISMATCH      = 0x81U,
    ERR_DAC_CHANNEL_IN_USE      = 0x82U,
} err_code_t;

#endif /* ERR_CODES_H */
