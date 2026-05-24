#ifndef HELPERS_H
#define HELPERS_H

#include <assert.h>
#include <stdint.h>

/* ── Little-endian readers ────────────────────────────────────────── */

static inline uint16_t read_u16_le(const uint8_t *data)
{
    assert(data != NULL);
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static inline uint32_t read_u32_le(const uint8_t *data)
{
    assert(data != NULL);
    return (uint32_t)data[0]
         | ((uint32_t)data[1] << 8)
         | ((uint32_t)data[2] << 16)
         | ((uint32_t)data[3] << 24);
}

/* ── Big-endian writers ───────────────────────────────────────────── */

static inline void write_u32_be(uint8_t *out, uint32_t value)
{
    assert(out != NULL);
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >>  8);
    out[3] = (uint8_t)(value);
}

#endif /* HELPERS_H */
