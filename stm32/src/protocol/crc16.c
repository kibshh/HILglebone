#include <assert.h>

#include "crc16.h"

/* Calculation specific constants */
#define CRC16_POLY              0x1021U
#define CRC16_MSBIT_MASK        0x8000U
#define CRC16_BITS_IN_BYTE      8U

/* Bitwise implementation. Small, branch-light, no table -- fine at 115200
 * baud (max one frame of ~260 B every ~23 ms; CRC cost is trivial). Swap for
 * a table-driven version later if we raise the baud rate into Mbit territory. */
uint16_t crc16_ccitt_update(uint16_t seed, const uint8_t *data, size_t len)
{
    /* Calling this with len > 0 and no buffer is a bug in the caller, not
     * something to hide behind an early-return. Compiles to nothing in
     * release builds (-DNDEBUG), traps in debug. */
    assert(data != NULL || len == 0);

    uint16_t crc = seed;

    for (size_t i = 0; i < len; ++i)
    {
        crc ^= (uint16_t)data[i] << CRC16_BITS_IN_BYTE;

        for (unsigned b = 0; b < CRC16_BITS_IN_BYTE; ++b)
        {
            if (crc & CRC16_MSBIT_MASK)
            {
                crc = (crc << 1) ^ CRC16_POLY;
            }
            else
            {
                crc <<= 1;
            }
        }
    }

    return crc;
}
