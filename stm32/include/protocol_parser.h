/**
 * Wire-protocol frame parser.
 *
 * Accepts bytes one at a time (typically fed from the UART RX ring buffer)
 * and assembles complete frames. CRC is checked before a frame is reported
 * as valid; frames with bad CRC or bad framing are dropped silently and
 * the parser re-syncs on the next START byte -- this matches the spec's
 * "silently drop, let the peer retransmit" error handling.
 */

#ifndef PROTOCOL_PARSER_H
#define PROTOCOL_PARSER_H

#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"

typedef enum
{
    PARSER_STATE_WAIT_START = 0,
    PARSER_STATE_READ_TYPE,
    PARSER_STATE_READ_LEN_LO,
    PARSER_STATE_READ_LEN_HI,
    PARSER_STATE_READ_SEQ,
    PARSER_STATE_READ_PAYLOAD,
    PARSER_STATE_READ_CRC_LO,
    PARSER_STATE_READ_CRC_HI
} protocol_parser_state_t;

typedef struct
{
    uint8_t  type;
    uint16_t len;
    uint8_t  seq;
    uint8_t  payload[PROTO_MAX_PAYLOAD];
} parsed_frame_t;

typedef struct
{
    protocol_parser_state_t state;
    uint16_t                payload_idx;
    uint16_t                expected_len;
    uint16_t                crc_running;
    uint16_t                crc_recv;
    parsed_frame_t          frame;          /* assembled in-place; valid after feed() returns true */
} protocol_parser_t;

/* Reset parser to idle (waiting for START). Safe to call any time. */
void protocol_parser_reset(protocol_parser_t *p);

/* Feed one byte. Returns true if this byte completed a frame with a valid
 * CRC; the completed frame is then available in `p->frame` and remains
 * valid until the next call to feed() or reset(). Returns false otherwise
 * (mid-frame, bad CRC, or bad framing -- the parser resyncs on the next
 * START byte). */
bool protocol_parser_feed(protocol_parser_t *p, uint8_t byte);

#endif /* PROTOCOL_PARSER_H */
