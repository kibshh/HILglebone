/**
 * SPI slave driver — ISR-driven, full-duplex.
 *
 * The DUT is the SPI master; the STM32 responds with a pre-loaded TX
 * buffer. The buffer is persistent: it is served circularly on every
 * DUT transaction until replaced via spi_slave_set_tx().
 *
 * GPIO configuration for MISO, SCK, NSS (and optionally MOSI) must be
 * done by the caller BEFORE calling spi_slave_init().
 */

#ifndef SPI_SLAVE_H
#define SPI_SLAVE_H

#include <stdint.h>

#include "err_codes.h"
#include "spi.h"

/* Maximum TX buffer length in bytes. */
#define SPI_SLAVE_TX_BUF_MAX    64U

/* ── API ──────────────────────────────────────────────────────────── */

/* Initialise the SPI peripheral in slave mode with hardware NSS.
 * `data_bits` must be 8 (16-bit frames not yet supported).
 * Returns ERR_SUCCESS, ERR_PERIPHERAL_BUSY if already claimed,
 * or ERR_INVALID_PARAMETER if any parameter is invalid. */
err_code_t spi_slave_init(spi_periph_t periph,
                          spi_mode_t   mode,
                          uint8_t      data_bits);

/* Disable the SPI peripheral and release the peripheral slot.
 * Returns ERR_SUCCESS or ERR_INVALID_PARAMETER. */
err_code_t spi_slave_deinit(spi_periph_t periph);

/* Replace the TX response buffer.  The new content takes effect on the
 * next DUT transaction (an in-flight transfer is never interrupted).
 * Returns ERR_SUCCESS or ERR_INVALID_PARAMETER. */
err_code_t spi_slave_set_tx(spi_periph_t   periph,
                            const uint8_t *buf,
                            uint8_t        len);

#endif /* SPI_SLAVE_H */
