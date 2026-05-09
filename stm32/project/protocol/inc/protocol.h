/**
 * HILglebone wire-protocol constants.
 *
 * Mirrors `protocol/protocol-spec.md` -- every on-the-wire number the firmware
 * touches lives here, so parser / encoder / dispatcher all agree on the same
 * byte layout and no magic numbers leak into the code.
 */

#ifndef PROTOCOL_H
#define PROTOCOL_H

/* ── Frame framing ────────────────────────────────────────────────── */

#define PROTO_START_BYTE            0xAAU

/* Frame layout (byte offsets from the START byte). */
#define PROTO_OFFSET_START          0U
#define PROTO_OFFSET_TYPE           1U
#define PROTO_OFFSET_LEN_LO         2U
#define PROTO_OFFSET_LEN_HI         3U
#define PROTO_OFFSET_SEQ            4U
#define PROTO_OFFSET_PAYLOAD        5U
/* CRC16 follows payload; its offset depends on LEN. */

#define PROTO_HEADER_SIZE           5U      /* START + TYPE + LEN(2) + SEQ */
#define PROTO_CRC_SIZE              2U
#define PROTO_FRAME_OVERHEAD        (PROTO_HEADER_SIZE + PROTO_CRC_SIZE)  /* 7 */

#define PROTO_MAX_PAYLOAD           256U
#define PROTO_MAX_FRAME_SIZE        (PROTO_FRAME_OVERHEAD + PROTO_MAX_PAYLOAD)

/* ── Message types ────────────────────────────────────────────────── */

#define PROTO_TYPE_CMD_SETUP_SENSOR 0x01U
#define PROTO_TYPE_CMD_SET_OUTPUT   0x02U
#define PROTO_TYPE_CMD_STOP_SENSOR  0x03U
#define PROTO_TYPE_CMD_SCENARIO     0x04U
#define PROTO_TYPE_CMD_SYNC         0x05U
#define PROTO_TYPE_RSP_ACK          0x10U
#define PROTO_TYPE_RSP_ERROR        0x11U
#define PROTO_TYPE_STATUS_REPORT    0x20U

/* ── Protocol IDs (inside CMD_SETUP_SENSOR payload) ──────────────── */

#define PROTO_ID_NONE               0x00U
#define PROTO_ID_I2C                0x01U
#define PROTO_ID_SPI                0x02U
#define PROTO_ID_DIGITAL_OUT        0x03U
#define PROTO_ID_DIGITAL_IN         0x04U
#define PROTO_ID_DAC                0x05U
#define PROTO_ID_PWM                0x06U
#define PROTO_ID_FREQ               0x07U
#define PROTO_ID_ONEWIRE            0x08U
#define PROTO_ID_CAN                0x09U

/* ── Sensor IDs ───────────────────────────────────────────────────── */

#define PROTO_SENSOR_ID_NONE        0x00U
#define PROTO_SENSOR_ID_MIN         0x01U
#define PROTO_SENSOR_ID_MAX         0xFFU

/* ── Common error codes (0x00..0x3F) ─────────────────────────────── */

#define ERR_SUCCESS                 0x00U
#define ERR_UNKNOWN_COMMAND         0x01U
#define ERR_MALFORMED_PAYLOAD       0x02U
#define ERR_BAD_CRC                 0x03U
#define ERR_PROTOCOL_UNSUPPORTED    0x04U
#define ERR_OUT_OF_RESOURCES        0x05U
#define ERR_INVALID_SENSOR_ID       0x06U
#define ERR_INVALID_PARAMETER       0x07U
#define ERR_PERIPHERAL_BUSY         0x08U
#define ERR_PIN_CONFLICT            0x09U
#define ERR_UNSUPPORTED_FEATURE     0x0AU
#define ERR_INTERNAL                0x0BU

/* Protocol-specific error codes live in each protocol's own header and
 * carve their own slice out of the 0x40..0xBF range (per the spec):
 *   I2C -> 0x40..0x5F in i2c_sensor.h
 *   ... future protocols extend here. */

/* ── Generic command-payload layouts ─────────────────────────────── */

/* CMD_SETUP_SENSOR payload: protocol_id + protocol-specific cfg. */
#define CMD_SETUP_OFFSET_PROTOCOL_ID    0U
#define CMD_SETUP_OFFSET_CFG            1U
#define CMD_SETUP_HEADER_SIZE           1U

/* CMD_SET_OUTPUT payload: sensor_id + protocol-specific value. */
#define CMD_SET_OUTPUT_OFFSET_SENSOR_ID 0U
#define CMD_SET_OUTPUT_OFFSET_VALUE     1U
#define CMD_SET_OUTPUT_HEADER_SIZE      1U

/* CMD_STOP_SENSOR payload: sensor_id only. */
#define CMD_STOP_OFFSET_SENSOR_ID       0U
#define CMD_STOP_PAYLOAD_SIZE           1U

/* ── RSP_ACK payload layout (3 bytes fixed) ──────────────────────── */

#define RSP_ACK_OFFSET_CMD_TYPE     0U
#define RSP_ACK_OFFSET_ERROR_CODE   1U
#define RSP_ACK_OFFSET_SENSOR_ID    2U
#define RSP_ACK_PAYLOAD_SIZE        3U

/* ── RSP_ERROR payload layout (2 bytes fixed) ────────────────────── */

#define RSP_ERROR_OFFSET_SENSOR_ID  0U
#define RSP_ERROR_OFFSET_ERROR_CODE 1U
#define RSP_ERROR_PAYLOAD_SIZE      2U

#endif /* PROTOCOL_H */
