/**
 * CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, no xor-out).
 *
 * Used by the wire protocol to protect every frame. See `protocol.h` for
 * the polynomial and init constants.
 */

#ifndef CRC16_H
#define CRC16_H

#include <stddef.h>
#include <stdint.h>

/* CRC-16/CCITT-FALSE covers TYPE + LEN(2) + SEQ + PAYLOAD. */
#define CRC16_INIT              0xFFFFU
#define CRC16_POLY              0x1021U
#define CRC16_MSBIT_MASK        0x8000U
#define CRC16_BITS_IN_BYTE      8U

/* Compute CRC over `len` bytes of `data`, starting from `seed`.
 * Pass `CRC16_INIT` for the initial call; pass the previous return
 * value to continue a running CRC across non-contiguous buffers. */
uint16_t crc16_ccitt_update(uint16_t seed, const uint8_t *data, size_t len);

#endif /* CRC16_H */
