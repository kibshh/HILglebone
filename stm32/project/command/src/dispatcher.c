#include "dispatcher.h"

#include <assert.h>
#include <stddef.h>

#include "err_codes.h"
#include "dac_sensor.h"
#include "digital_out_sensor.h"
#include "hw_timer.h"
#include "i2c_sensor.h"
#include "protocol.h"
#include "protocol_encoder.h"
#include "pwm_sensor.h"
#include "sensor_manager.h"

/* ── Command handlers ─────────────────────────────────────────────── */

static void handle_sync(const parsed_frame_t *f)
{
    assert(f != NULL);

    (void)protocol_send_ack(PROTO_TYPE_CMD_SYNC,
                            ERR_SUCCESS,
                            PROTO_SENSOR_ID_NONE,
                            f->seq);
}

static void handle_setup(const parsed_frame_t *f)
{
    assert(f != NULL);

    if (f->len < CMD_SETUP_HEADER_SIZE)
    {
        (void)protocol_send_ack(PROTO_TYPE_CMD_SETUP_SENSOR,
                                ERR_MALFORMED_PAYLOAD,
                                PROTO_SENSOR_ID_NONE,
                                f->seq);
        return;
    }

    uint8_t        protocol_id = f->payload[CMD_SETUP_OFFSET_PROTOCOL_ID];
    const uint8_t *cfg         = &f->payload[CMD_SETUP_OFFSET_CFG];
    uint16_t       cfg_len     = (uint16_t)(f->len - CMD_SETUP_HEADER_SIZE);

    uint8_t sensor_id = PROTO_SENSOR_ID_NONE;
    err_code_t err;

    switch (protocol_id)
    {
    case PROTO_ID_I2C:
        err = i2c_sensor_setup(cfg, cfg_len, &sensor_id);
        break;

    case PROTO_ID_DIGITAL_OUT:
        err = digital_out_sensor_setup(cfg, cfg_len, &sensor_id);
        break;

    case PROTO_ID_PWM:
        err = pwm_sensor_setup(cfg, cfg_len, &sensor_id);
        break;

    case PROTO_ID_DAC:
        err = dac_sensor_setup(cfg, cfg_len, &sensor_id);
        break;

    case PROTO_ID_SPI:
    case PROTO_ID_DIGITAL_IN:
    case PROTO_ID_FREQ:
    case PROTO_ID_ONEWIRE:
    case PROTO_ID_CAN:
    default:
        (void)protocol_send_ack(PROTO_TYPE_CMD_SETUP_SENSOR,
                                ERR_UNSUPPORTED,
                                PROTO_SENSOR_ID_NONE,
                                f->seq);
        return;
    }

    if (err != ERR_SUCCESS)
    {
        sensor_id = PROTO_SENSOR_ID_NONE;
    }

    (void)protocol_send_ack(PROTO_TYPE_CMD_SETUP_SENSOR, err, sensor_id, f->seq);
}

static void handle_set_output(const parsed_frame_t *f)
{
    assert(f != NULL);

    if (f->len < CMD_SET_OUTPUT_HEADER_SIZE)
    {
        (void)protocol_send_ack(PROTO_TYPE_CMD_SET_OUTPUT,
                                ERR_MALFORMED_PAYLOAD,
                                PROTO_SENSOR_ID_NONE,
                                f->seq);
        return;
    }

    uint8_t        sensor_id  = f->payload[CMD_SET_OUTPUT_OFFSET_SENSOR_ID];
    const uint8_t *values     = &f->payload[CMD_SET_OUTPUT_OFFSET_VALUE];
    uint16_t       values_len = (uint16_t)(f->len - CMD_SET_OUTPUT_HEADER_SIZE);

    sensor_slot_t *slot = sensor_manager_lookup(sensor_id);
    if (slot == NULL)
    {
        (void)protocol_send_ack(PROTO_TYPE_CMD_SET_OUTPUT,
                                ERR_INVALID_SENSOR_ID,
                                sensor_id,
                                f->seq);
        return;
    }

    err_code_t err;
    switch (slot->protocol_id)
    {
    case PROTO_ID_I2C:
        err = i2c_sensor_set_output(slot->internal_id, values, values_len);
        break;

    case PROTO_ID_DIGITAL_OUT:
        err = digital_out_sensor_set_output(slot->internal_id, values, values_len);
        break;

    case PROTO_ID_PWM:
        err = pwm_sensor_set_output(slot->internal_id, values, values_len);
        break;

    case PROTO_ID_DAC:
        err = dac_sensor_set_output(slot->internal_id, values, values_len);
        break;

    default:
        err = ERR_INVALID_PARAMETER;
        break;
    }

    (void)protocol_send_ack(PROTO_TYPE_CMD_SET_OUTPUT, err, sensor_id, f->seq);
}

static void handle_stop(const parsed_frame_t *f)
{
    assert(f != NULL);

    if (f->len < CMD_STOP_PAYLOAD_SIZE)
    {
        (void)protocol_send_ack(PROTO_TYPE_CMD_STOP_SENSOR,
                                ERR_MALFORMED_PAYLOAD,
                                PROTO_SENSOR_ID_NONE,
                                f->seq);
        return;
    }

    uint8_t sensor_id = f->payload[CMD_STOP_OFFSET_SENSOR_ID];

    sensor_slot_t *slot = sensor_manager_lookup(sensor_id);
    if (slot == NULL)
    {
        (void)protocol_send_ack(PROTO_TYPE_CMD_STOP_SENSOR,
                                ERR_INVALID_SENSOR_ID,
                                sensor_id,
                                f->seq);
        return;
    }

    err_code_t err;
    switch (slot->protocol_id)
    {
    case PROTO_ID_I2C:
        err = i2c_sensor_stop(slot->internal_id);
        break;

    case PROTO_ID_DIGITAL_OUT:
        err = digital_out_sensor_stop(slot->internal_id);
        break;

    case PROTO_ID_PWM:
        err = pwm_sensor_stop(slot->internal_id);
        break;

    case PROTO_ID_DAC:
        err = dac_sensor_stop(slot->internal_id);
        break;

    default:
        err = ERR_INVALID_PARAMETER;
        break;
    }

    if (err == ERR_SUCCESS)
    {
        sensor_manager_release(sensor_id);
    }

    (void)protocol_send_ack(PROTO_TYPE_CMD_STOP_SENSOR, err, sensor_id, f->seq);
}

/* ── Public API ───────────────────────────────────────────────────── */

void dispatcher_init(void)
{
    sensor_manager_init();
    hw_timer_init();
    i2c_sensor_init();
    digital_out_sensor_init();
    pwm_sensor_init();
    dac_sensor_init();
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

    case PROTO_TYPE_CMD_SCENARIO:
        (void)protocol_send_ack(frame->type,
                                ERR_NOT_IMPLEMENTED,
                                PROTO_SENSOR_ID_NONE,
                                frame->seq);
        break;

    default:
        (void)protocol_send_ack(frame->type,
                                ERR_UNKNOWN_CMD,
                                PROTO_SENSOR_ID_NONE,
                                frame->seq);
        break;
    }
}
