#include "dispatcher.h"

#include <assert.h>
#include <stddef.h>

#include "i2c_sensor.h"
#include "protocol.h"
#include "protocol_encoder.h"
#include "sensor_manager.h"

/* CMD_SYNC: link-level heartbeat. No sensor state touched; just acknowledge.
 * Later, if we add per-link sequence tracking, this is where we'd reset it. */
static void handle_sync(const parsed_frame_t *f)
{
    assert(f != NULL);

    protocol_send_ack(PROTO_TYPE_CMD_SYNC,
                      ERR_SUCCESS,
                      PROTO_SENSOR_ID_NONE,
                      f->seq);
}

/* CMD_SETUP_SENSOR: peel off protocol_id, hand the rest to the matching
 * backend. Only I2C is wired in for now; other protocol_ids return
 * ERR_PROTOCOL_UNSUPPORTED per the spec. */
static void handle_setup(const parsed_frame_t *f)
{
    assert(f != NULL);

    if (f->len < CMD_SETUP_HEADER_SIZE)
    {
        protocol_send_ack(PROTO_TYPE_CMD_SETUP_SENSOR,
                          ERR_MALFORMED_PAYLOAD,
                          PROTO_SENSOR_ID_NONE,
                          f->seq);
        return;
    }

    uint8_t protocol_id = f->payload[CMD_SETUP_OFFSET_PROTOCOL_ID];
    const uint8_t *cfg  = &f->payload[CMD_SETUP_OFFSET_CFG];
    uint16_t cfg_len    = (uint16_t)(f->len - CMD_SETUP_HEADER_SIZE);

    uint8_t sensor_id = PROTO_SENSOR_ID_NONE;
    uint8_t err;

    switch (protocol_id)
    {
    case PROTO_ID_I2C:
        err = i2c_sensor_setup(cfg, cfg_len, &sensor_id);
        break;

    /* Other protocol IDs are known but not yet implemented. */
    case PROTO_ID_SPI:
    case PROTO_ID_DIGITAL_OUT:
    case PROTO_ID_DIGITAL_IN:
    case PROTO_ID_DAC:
    case PROTO_ID_PWM:
    case PROTO_ID_FREQ:
    case PROTO_ID_ONEWIRE:
    case PROTO_ID_CAN:
        err = ERR_PROTOCOL_UNSUPPORTED;
        break;

    default:
        err = ERR_PROTOCOL_UNSUPPORTED;
        break;
    }

    /* Per the spec, a failed setup returns sensor_id = NONE. The backend
     * is responsible for not touching out_sensor_id on failure, but we
     * enforce it here too so the ACK invariant can't leak a half-committed id. */
    if (err != ERR_SUCCESS)
    {
        sensor_id = PROTO_SENSOR_ID_NONE;
    }

    protocol_send_ack(PROTO_TYPE_CMD_SETUP_SENSOR, err, sensor_id, f->seq);
}

/* CMD_SET_OUTPUT: look up sensor by id, dispatch values bytes to the
 * backend matching the sensor's protocol. */
static void handle_set_output(const parsed_frame_t *f)
{
    assert(f != NULL);

    if (f->len < CMD_SET_OUTPUT_HEADER_SIZE)
    {
        protocol_send_ack(PROTO_TYPE_CMD_SET_OUTPUT,
                          ERR_MALFORMED_PAYLOAD,
                          PROTO_SENSOR_ID_NONE,
                          f->seq);
        return;
    }

    uint8_t sensor_id      = f->payload[CMD_SET_OUTPUT_OFFSET_SENSOR_ID];
    const uint8_t *values  = &f->payload[CMD_SET_OUTPUT_OFFSET_VALUE];
    uint16_t values_len    = (uint16_t)(f->len - CMD_SET_OUTPUT_HEADER_SIZE);

    sensor_slot_t *slot = sensor_manager_lookup(sensor_id);
    if (slot == NULL)
    {
        protocol_send_ack(PROTO_TYPE_CMD_SET_OUTPUT,
                          ERR_INVALID_SENSOR_ID,
                          sensor_id,
                          f->seq);
        return;
    }

    uint8_t err;
    switch (slot->protocol_id)
    {
    case PROTO_ID_I2C:
        err = i2c_sensor_set_output(slot->internal_id, values, values_len);
        break;

    default:
        err = ERR_PROTOCOL_UNSUPPORTED;
        break;
    }

    protocol_send_ack(PROTO_TYPE_CMD_SET_OUTPUT, err, sensor_id, f->seq);
}

/* CMD_STOP_SENSOR: tear down the sensor; echo sensor_id on the ACK. */
static void handle_stop(const parsed_frame_t *f)
{
    assert(f != NULL);

    if (f->len < CMD_STOP_PAYLOAD_SIZE)
    {
        protocol_send_ack(PROTO_TYPE_CMD_STOP_SENSOR,
                          ERR_MALFORMED_PAYLOAD,
                          PROTO_SENSOR_ID_NONE,
                          f->seq);
        return;
    }

    uint8_t sensor_id = f->payload[CMD_STOP_OFFSET_SENSOR_ID];

    sensor_slot_t *slot = sensor_manager_lookup(sensor_id);
    if (slot == NULL)
    {
        protocol_send_ack(PROTO_TYPE_CMD_STOP_SENSOR,
                          ERR_INVALID_SENSOR_ID,
                          sensor_id,
                          f->seq);
        return;
    }

    uint8_t err;
    switch (slot->protocol_id)
    {
    case PROTO_ID_I2C:
        err = i2c_sensor_stop(slot->internal_id);
        break;

    default:
        err = ERR_PROTOCOL_UNSUPPORTED;
        break;
    }

    if (err == ERR_SUCCESS)
    {
        sensor_manager_release(sensor_id);
    }

    protocol_send_ack(PROTO_TYPE_CMD_STOP_SENSOR, err, sensor_id, f->seq);
}

void dispatcher_init(void)
{
    sensor_manager_init();
    i2c_sensor_init();
}

void dispatcher_handle(const parsed_frame_t *frame)
{
    assert(frame != NULL);

    switch (frame->type)
    {
    case PROTO_TYPE_CMD_SYNC:
        handle_sync(frame);
        break;

    case PROTO_TYPE_CMD_SETUP_SENSOR:
        handle_setup(frame);
        break;

    case PROTO_TYPE_CMD_SET_OUTPUT:
        handle_set_output(frame);
        break;

    case PROTO_TYPE_CMD_STOP_SENSOR:
        handle_stop(frame);
        break;

    /* CMD_SCENARIO is reserved in the spec but has no semantics yet. */
    case PROTO_TYPE_CMD_SCENARIO:
        protocol_send_ack(frame->type,
                          ERR_UNSUPPORTED_FEATURE,
                          PROTO_SENSOR_ID_NONE,
                          frame->seq);
        break;

    default:
        protocol_send_ack(frame->type,
                          ERR_UNKNOWN_COMMAND,
                          PROTO_SENSOR_ID_NONE,
                          frame->seq);
        break;
    }
}
