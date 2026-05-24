/**
 * DAC (analog output) sensor backend — TI DAC8568 over SPI.
 *
 * Each sensor instance drives one DAC8568 channel (A..H).  Multiple
 * channels on the same chip share the SPI peripheral, CS pin, LDAC
 * pin, and reference configuration.  The first SETUP initialises the
 * SPI bus and enables the internal reference if requested; subsequent
 * SETUPs on the same peripheral join the existing instance.
 *
 * Wire layouts mirror docs/protocol/dac-spec.md §4 and §5.
 */

#ifndef DAC_SENSOR_H
#define DAC_SENSOR_H

#include <stdint.h>

#include "err_codes.h"

/* ── Capacity ─────────────────────────────────────────────────────── */

#define DAC_CHANNELS_PER_CHIP   8U      /* A..H */
#define DAC_SENSOR_MAX_COUNT    16U     /* 2 SPI peripherals × 8 channels */
#define DAC_SPI_FRAME_SIZE      4U      /* DAC8568 command is always a 32-bit big-endian word */
#define DAC_SPI_TX_TIMEOUT_MS   10U     /* max wait for SPI transfer completion semaphore */

/* ── Channel / reference enums (wire values) ─────────────────────── */

#define DAC_CHANNEL_A           0U
#define DAC_CHANNEL_B           1U
#define DAC_CHANNEL_C           2U
#define DAC_CHANNEL_D           3U
#define DAC_CHANNEL_E           4U
#define DAC_CHANNEL_F           5U
#define DAC_CHANNEL_G           6U
#define DAC_CHANNEL_H           7U
#define DAC_CHANNEL_MAX         DAC_CHANNEL_H

#define DAC_REFERENCE_EXTERNAL        0U
#define DAC_REFERENCE_INTERNAL_STATIC 1U
#define DAC_REFERENCE_INTERNAL_FLEX   2U
#define DAC_REFERENCE_MAX             DAC_REFERENCE_INTERNAL_FLEX

/* DAC8568 command nibbles (bits 27:24 of the 32-bit SPI frame). */
#define DAC_CMD_WRITE_AND_LOAD        0x3U   /* write input register N and update channel N */
#define DAC_CMD_REF_SETUP             0x8U   /* enable / configure internal reference */

/* Address used with DAC_CMD_REF_SETUP (applies chip-wide, not per-channel). */
#define DAC_ADDR_REF                  0x0U

/* DAC8568 reference-enable command data (sent with DAC_CMD_REF_SETUP).
 * DAC_REF_DATA_EXTERNAL is 0x0000 (power down internal ref) — this matches
 * the chip's power-on default, so no command is sent for the external case. */
#define DAC_REF_DATA_EXTERNAL         0x0000U   /* internal ref powered down (chip default) */
#define DAC_REF_DATA_STATIC           0x0001U   /* static 2.5 V reference */
#define DAC_REF_DATA_FLEX             0x0003U   /* flexible 2.5 V reference */

/* ── LDAC disabled sentinel ───────────────────────────────────────── */

#define DAC_LDAC_PORT_DISABLED  0xFFU   /* ldac_port = 0xFF → LDAC tied to GND */

/* ── CMD_SETUP_SENSOR payload (after the generic protocol_id byte) ── */

#define DAC_CFG_OFFSET_SPI_PERIPH       0U      /* u8  */
#define DAC_CFG_OFFSET_SPI_CLOCK_HZ     1U      /* u32 LE */
#define DAC_CFG_OFFSET_MOSI_PORT        5U      /* u8  */
#define DAC_CFG_OFFSET_MOSI_PIN         6U      /* u8  */
#define DAC_CFG_OFFSET_MOSI_AF          7U      /* u8  */
#define DAC_CFG_OFFSET_SCK_PORT         8U      /* u8  */
#define DAC_CFG_OFFSET_SCK_PIN          9U      /* u8  */
#define DAC_CFG_OFFSET_SCK_AF           10U     /* u8  */
#define DAC_CFG_OFFSET_CS_PORT          11U     /* u8  */
#define DAC_CFG_OFFSET_CS_PIN           12U     /* u8  */
#define DAC_CFG_OFFSET_LDAC_PORT        13U     /* u8  (0xFF = disabled) */
#define DAC_CFG_OFFSET_LDAC_PIN         14U     /* u8  */
#define DAC_CFG_OFFSET_CHANNEL          15U     /* u8  */
#define DAC_CFG_OFFSET_REFERENCE        16U     /* u8  */
#define DAC_CFG_OFFSET_SPI_MODE         17U     /* u8  */
#define DAC_CFG_OFFSET_INITIAL_VALUE    18U     /* u16 LE */
#define DAC_CFG_SIZE                    20U

/* ── CMD_SET_OUTPUT payload (after the generic sensor_id byte) ───── */

#define DAC_SET_OUTPUT_OFFSET_VALUE     0U      /* u16 LE */
#define DAC_SET_OUTPUT_SIZE             2U

/* ── API ──────────────────────────────────────────────────────────── */

/* One-time startup. Must run before any setup call. */
void dac_sensor_init(void);

/* Handle CMD_SETUP_SENSOR for protocol_id = DAC.
 *   `cfg`           = payload bytes starting AFTER the protocol_id byte
 *   `cfg_len`       = length of that slice
 *   `out_sensor_id` = filled with the newly-allocated id on success, or
 *                     PROTO_SENSOR_ID_NONE on failure
 * Returns ERR_SUCCESS or a common / DAC-specific error code. */
err_code_t dac_sensor_setup(const uint8_t *cfg,
                             uint16_t       cfg_len,
                             uint8_t       *out_sensor_id);

/* Handle CMD_SET_OUTPUT for a DAC sensor.  Drives a new analog level on
 * the channel; blocks the protocol task until the SPI transfer completes.
 *   `internal_id` = index returned by dac_sensor_setup
 *   `values`      = payload bytes starting AFTER the generic sensor_id byte
 *   `values_len`  = length of that slice
 * Returns ERR_SUCCESS or an error code. */
err_code_t dac_sensor_set_output(uint8_t        internal_id,
                                  const uint8_t *values,
                                  uint16_t       values_len);

/* Handle CMD_STOP_SENSOR.  Frees the channel; if the last channel on
 * the SPI peripheral, deinitialises SPI and reverts all GPIO pins. */
err_code_t dac_sensor_stop(uint8_t internal_id);

#endif /* DAC_SENSOR_H */
