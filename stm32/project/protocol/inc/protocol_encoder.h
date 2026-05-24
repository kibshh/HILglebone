/**
 * Wire-protocol frame encoder.
 *
 * Builds complete frames (START ... CRC) in a caller-supplied scratch buffer
 * and pushes them into the UART TX ring. The layer above talks in semantic
 * terms (`send_ack`, `send_error`) -- it never touches framing bytes.
 */

#ifndef PROTOCOL_ENCODER_H
#define PROTOCOL_ENCODER_H

#include <stdint.h>

#include "err_codes.h"

/* Send an RSP_ACK for the command with sequence number `seq`.
 * `cmd_type` is the TYPE byte of the command being acknowledged;
 * `error_code` distinguishes success (ERR_SUCCESS) from failure;
 * `sensor_id` follows the sensor-id-field semantics in protocol-spec.md.
 * Returns ERR_SUCCESS if the whole frame was enqueued, ERR_OUT_OF_RESOURCES
 * if the UART TX buffer was too full to accept the frame. */
err_code_t protocol_send_ack(uint8_t    cmd_type,
                             err_code_t error_code,
                             uint8_t    sensor_id,
                             uint8_t    seq);

/* Send an unsolicited RSP_ERROR. `sensor_id` is 0 when not sensor-scoped.
 * Returns ERR_SUCCESS or ERR_OUT_OF_RESOURCES. */
err_code_t protocol_send_error(uint8_t    sensor_id,
                               err_code_t error_code,
                               uint8_t    seq);

#endif /* PROTOCOL_ENCODER_H */
