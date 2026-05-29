/**
 * SPI slave sensor backend.
 *
 * Handles CMD_SETUP_SENSOR / CMD_SET_OUTPUT / CMD_STOP_SENSOR for
 * protocol_id = PROTO_ID_SPI (0x02).  Each sensor instance occupies
 * one SPI peripheral; at most one sensor per peripheral.
 *
 * Payload formats are defined in docs/protocol/spi-sensors-spec.md.
 */

#ifndef SPI_SLAVE_SENSOR_H
#define SPI_SLAVE_SENSOR_H

#include <stdint.h>

#include "err_codes.h"
#include "spi.h"

/* ── Capacity ─────────────────────────────────────────────────────── */

#define SPI_SLAVE_SENSOR_MAX_COUNT  ((uint8_t)SPI_PERIPH_COUNT)   /* one per SPI peripheral */

/* ── CMD_SETUP_SENSOR payload offsets (after protocol_id byte) ────── */

#define SPI_SLAVE_CFG_OFFSET_SPI_PERIPH     0U      /* u8  */
#define SPI_SLAVE_CFG_OFFSET_SPI_MODE       1U      /* u8  */
#define SPI_SLAVE_CFG_OFFSET_MISO_PORT      2U      /* u8  */
#define SPI_SLAVE_CFG_OFFSET_MISO_PIN       3U      /* u8  */
#define SPI_SLAVE_CFG_OFFSET_MISO_AF        4U      /* u8  */
#define SPI_SLAVE_CFG_OFFSET_SCK_PORT       5U      /* u8  */
#define SPI_SLAVE_CFG_OFFSET_SCK_PIN        6U      /* u8  */
#define SPI_SLAVE_CFG_OFFSET_SCK_AF         7U      /* u8  */
#define SPI_SLAVE_CFG_OFFSET_NSS_PORT       8U      /* u8  */
#define SPI_SLAVE_CFG_OFFSET_NSS_PIN        9U      /* u8  */
#define SPI_SLAVE_CFG_OFFSET_NSS_AF         10U     /* u8  */
#define SPI_SLAVE_CFG_OFFSET_MOSI_PORT      11U     /* u8  (0xFF = not used) */
#define SPI_SLAVE_CFG_OFFSET_MOSI_PIN       12U     /* u8  ignored if mosi_port = 0xFF */
#define SPI_SLAVE_CFG_OFFSET_MOSI_AF        13U     /* u8  ignored if mosi_port = 0xFF */
#define SPI_SLAVE_CFG_OFFSET_TX_BUF_LEN     14U     /* u16 LE */
#define SPI_SLAVE_CFG_OFFSET_TX_BUF         16U     /* u8[tx_buf_len] */
#define SPI_SLAVE_CFG_MIN_SIZE              16U     /* minimum bytes before tx_buf */

/* ── CMD_SET_OUTPUT payload offsets (after sensor_id byte) ──────────*/

#define SPI_SLAVE_SET_OUTPUT_OFFSET_TX_BUF_LEN  0U  /* u16 LE */
#define SPI_SLAVE_SET_OUTPUT_OFFSET_TX_BUF      2U  /* u8[tx_buf_len] */
#define SPI_SLAVE_SET_OUTPUT_MIN_SIZE           2U  /* minimum bytes before tx_buf */

/* MOSI disabled sentinel (same pattern as DAC_LDAC_PORT_DISABLED). */
#define SPI_SLAVE_MOSI_PORT_DISABLED    0xFFU

/* ── API ──────────────────────────────────────────────────────────── */

void spi_slave_sensor_init(void);

err_code_t spi_slave_sensor_setup(const uint8_t *cfg,
                                  uint16_t       cfg_len,
                                  uint8_t       *out_sensor_id);

err_code_t spi_slave_sensor_set_output(uint8_t        internal_id,
                                       const uint8_t *values,
                                       uint16_t       values_len);

err_code_t spi_slave_sensor_stop(uint8_t internal_id);

#endif /* SPI_SLAVE_SENSOR_H */
