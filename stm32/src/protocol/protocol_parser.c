#include "protocol_parser.h"

#include <assert.h>

#include "crc16.h"
#include "protocol.h"

/* Absorb one byte into the running CRC. CRC covers TYPE + LEN(2) + SEQ +
 * PAYLOAD per the spec, so every byte except START and CRC itself feeds in. */
static inline void crc_eat(protocol_parser_t *p, uint8_t b)
{
    p->crc_running = crc16_ccitt_update(p->crc_running, &b, 1U);
}

void protocol_parser_reset(protocol_parser_t *p)
{
    p->state        = PARSER_STATE_WAIT_START;
    p->payload_idx  = 0U;
    p->expected_len = 0U;
    p->crc_running  = CRC16_INIT;
    p->crc_recv     = 0U;
}

bool protocol_parser_feed(protocol_parser_t *p, uint8_t byte)
{
    assert(p != NULL);

    switch (p->state)
    {
    case PARSER_STATE_WAIT_START:
        if (byte == PROTO_START_BYTE)
        {
            /* Start of a new frame: prime the running CRC fresh. */
            p->crc_running = CRC16_INIT;
            p->state       = PARSER_STATE_READ_TYPE;
        }
        /* Any non-START byte here is noise: discard and stay. */
        return false;

    case PARSER_STATE_READ_TYPE:
        p->frame.type = byte;
        crc_eat(p, byte);
        p->state = PARSER_STATE_READ_LEN_LO;
        return false;

    case PARSER_STATE_READ_LEN_LO:
        p->expected_len = byte;
        crc_eat(p, byte);
        p->state = PARSER_STATE_READ_LEN_HI;
        return false;

    case PARSER_STATE_READ_LEN_HI:
        p->expected_len |= (uint16_t)byte << 8;
        crc_eat(p, byte);

        if (p->expected_len > PROTO_MAX_PAYLOAD)
        {
            /* Over-length frame: abort and resync. */
            protocol_parser_reset(p);
            return false;
        }

        p->frame.len = p->expected_len;
        p->state     = PARSER_STATE_READ_SEQ;
        return false;

    case PARSER_STATE_READ_SEQ:
        p->frame.seq = byte;
        crc_eat(p, byte);

        p->payload_idx = 0U;
        p->state = (p->expected_len == 0U)
                   ? PARSER_STATE_READ_CRC_LO
                   : PARSER_STATE_READ_PAYLOAD;
        return false;

    case PARSER_STATE_READ_PAYLOAD:
        if (p->payload_idx >= PROTO_MAX_PAYLOAD)
        {
            protocol_parser_reset(p);
            return false;
        }
        p->frame.payload[p->payload_idx++] = byte;
        crc_eat(p, byte);

        if (p->payload_idx >= p->expected_len)
        {
            p->state = PARSER_STATE_READ_CRC_LO;
        }
        return false;

    case PARSER_STATE_READ_CRC_LO:
        p->crc_recv = byte;
        p->state    = PARSER_STATE_READ_CRC_HI;
        return false;

    case PARSER_STATE_READ_CRC_HI:
    {
        p->crc_recv |= (uint16_t)byte << 8;
        bool ok = (p->crc_recv == p->crc_running);
        protocol_parser_reset(p);
        return ok;
    }
    }

    /* Unreachable; treat as resync. */
    protocol_parser_reset(p);
    return false;
}
