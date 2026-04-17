/**
 * Top-level command dispatcher.
 *
 * Consumes fully-parsed frames (CRC already verified) and routes them to
 * the right protocol backend, then emits exactly one RSP_ACK per command.
 * This is where the generic protocol meets the protocol-specific backends.
 */

#ifndef DISPATCHER_H
#define DISPATCHER_H

#include "protocol_parser.h"

/* One-time bring-up: wire up the sensor manager and all backends. */
void dispatcher_init(void);

/* Handle one incoming frame. Always synchronous, always sends exactly one
 * response (RSP_ACK) unless the command type was already discarded by the
 * parser. */
void dispatcher_handle(const parsed_frame_t *frame);

#endif /* DISPATCHER_H */
