#include "protocol_encoder.h"

#include <assert.h>

#include "crc16.h"
#include "protocol.h"
#include "uart.h"

/* Build a complete framed message from `type + payload` in `out`. Returns
 * the total number of bytes written (PROTO_FRAME_OVERHEAD + payload_len).
 * Caller guarantees `out` is at least that large. */
static size_t build_frame(uint8_t       *out,
                          uint8_t        type,
                          uint8_t        seq,
                          const uint8_t *payload,
                          uint16_t       payload_len)
{
    assert(out != NULL);
    assert(payload != NULL || payload_len == 0U);

    out[PROTO_OFFSET_START]  = PROTO_START_BYTE;
    out[PROTO_OFFSET_TYPE]   = type;
    out[PROTO_OFFSET_LEN_LO] = (uint8_t)(payload_len & 0xFFU);
    out[PROTO_OFFSET_LEN_HI] = (uint8_t)(payload_len >> 8);
    out[PROTO_OFFSET_SEQ]    = seq;

    for (uint16_t i = 0; i < payload_len; ++i)
    {
        out[PROTO_OFFSET_PAYLOAD + i] = payload[i];
    }

    /* CRC covers TYPE + LEN(2) + SEQ + PAYLOAD -- everything after START,
     * everything before CRC itself. */
    uint16_t crc = crc16_ccitt_update(CRC16_INIT,
                                      &out[PROTO_OFFSET_TYPE],
                                      (size_t)(PROTO_HEADER_SIZE - 1U) + payload_len);

    size_t crc_off = PROTO_OFFSET_PAYLOAD + payload_len;
    out[crc_off]      = (uint8_t)(crc & 0xFFU);
    out[crc_off + 1U] = (uint8_t)(crc >> 8);

    return (size_t)PROTO_FRAME_OVERHEAD + payload_len;
}

/* Enqueue the built frame into the UART TX ring. If the ring can't absorb
 * the whole frame, we report failure -- the caller can decide to back off
 * or drop; we never emit a partial frame because the peer would reject it
 * on CRC mismatch anyway. */
static bool push_frame(const uint8_t *frame, size_t len)
{
    assert(frame != NULL || len == 0U);

    return uart_tx_push(frame, len) == len;
}

bool protocol_send_ack(uint8_t cmd_type,
                       uint8_t error_code,
                       uint8_t sensor_id,
                       uint8_t seq)
{
    uint8_t frame[PROTO_FRAME_OVERHEAD + RSP_ACK_PAYLOAD_SIZE];
    uint8_t payload[RSP_ACK_PAYLOAD_SIZE];

    payload[RSP_ACK_OFFSET_CMD_TYPE]   = cmd_type;
    payload[RSP_ACK_OFFSET_ERROR_CODE] = error_code;
    payload[RSP_ACK_OFFSET_SENSOR_ID]  = sensor_id;

    size_t n = build_frame(frame,
                           PROTO_TYPE_RSP_ACK,
                           seq,
                           payload,
                           RSP_ACK_PAYLOAD_SIZE);

    return push_frame(frame, n);
}

bool protocol_send_error(uint8_t sensor_id,
                         uint8_t error_code,
                         uint8_t seq)
{
    uint8_t frame[PROTO_FRAME_OVERHEAD + RSP_ERROR_PAYLOAD_SIZE];
    uint8_t payload[RSP_ERROR_PAYLOAD_SIZE];

    payload[RSP_ERROR_OFFSET_SENSOR_ID]  = sensor_id;
    payload[RSP_ERROR_OFFSET_ERROR_CODE] = error_code;

    size_t n = build_frame(frame,
                           PROTO_TYPE_RSP_ERROR,
                           seq,
                           payload,
                           RSP_ERROR_PAYLOAD_SIZE);

    return push_frame(frame, n);
}
