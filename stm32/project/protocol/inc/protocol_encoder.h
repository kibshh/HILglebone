/**
 * Wire-protocol frame encoder.
 *
 * Builds complete frames (START ... CRC) in a caller-supplied scratch buffer
 * and pushes them into the UART TX ring. The layer above talks in semantic
 * terms (`send_ack`, `send_error`) -- it never touches framing bytes.
 */

#ifndef PROTOCOL_ENCODER_H
#define PROTOCOL_ENCODER_H

#include <stdbool.h>
#include <stdint.h>

/* Send an RSP_ACK for the command with sequence number `seq`.
 * `cmd_type` is the TYPE byte of the command being acknowledged;
 * `error_code` distinguishes success (`ERR_SUCCESS`) from failure;
 * `sensor_id` follows the sensor-id-field semantics in protocol-spec.md
 * (new id on setup-success, 0 on setup-failure, echoed otherwise).
 * Returns true if the whole frame was enqueued. */
bool protocol_send_ack(uint8_t cmd_type,
                       uint8_t error_code,
                       uint8_t sensor_id,
                       uint8_t seq);

/* Send an unsolicited RSP_ERROR. `sensor_id` is 0 when not sensor-scoped.
 * `seq` should be a fresh sequence number chosen by the caller for
 * unsolicited traffic. Returns true if the whole frame was enqueued. */
bool protocol_send_error(uint8_t sensor_id,
                         uint8_t error_code,
                         uint8_t seq);

#endif /* PROTOCOL_ENCODER_H */
