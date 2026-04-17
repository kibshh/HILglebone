#ifndef HELPERS_H
#define HELPERS_H

#include <assert.h>
#include <stdint.h>

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

#endif /* HELPERS_H */
