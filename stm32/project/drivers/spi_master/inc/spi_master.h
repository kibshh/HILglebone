/**
 * SPI master driver — ISR-driven, non-blocking transmit.
 *
 * Supports SPI1 (APB2, max 42 MHz) and SPI2 (APB1, max 21 MHz).
 * Transfers are started by spi_master_write() and complete via the SPI
 * RXNE interrupt, which calls the registered callback from ISR context.
 * The caller is responsible for CS / LDAC management; this driver only
 * handles the SPI peripheral registers and the ISR dispatch.
 *
 * GPIO configuration for MOSI and SCK must be done by the caller BEFORE
 * calling spi_master_init().
 */

#ifndef SPI_MASTER_H
#define SPI_MASTER_H

#include <stdint.h>

#include "err_codes.h"

/* ── Peripheral identifiers ───────────────────────────────────────── */

typedef enum
{
    SPI_MASTER_SPI1 = 0,
    SPI_MASTER_SPI2 = 1,

    SPI_MASTER_COUNT,
} spi_master_periph_t;

/* ── SPI mode ─────────────────────────────────────────────────────── */

/* Standard SPI modes.  Bit 1 = CPOL, bit 0 = CPHA.
 *   Mode 0: CPOL=0 CPHA=0 — idle low, sample on rising
 *   Mode 1: CPOL=0 CPHA=1 — idle low, sample on falling
 *   Mode 2: CPOL=1 CPHA=0 — idle high, sample on falling
 *   Mode 3: CPOL=1 CPHA=1 — idle high, sample on rising
 */
typedef enum
{
    SPI_MASTER_MODE_0 = 0,
    SPI_MASTER_MODE_1 = 1,
    SPI_MASTER_MODE_2 = 2,
    SPI_MASTER_MODE_3 = 3,

    SPI_MASTER_MODE_MAX = SPI_MASTER_MODE_3,
} spi_master_mode_t;

/* ── Transfer callback ────────────────────────────────────────────── */

/* Called from ISR context when the last byte has been shifted out.
 * Keep it short: toggle CS/LDAC GPIOs and give a FreeRTOS semaphore. */
typedef void (*spi_master_callback_t)(void *ctx);

/* ── API ──────────────────────────────────────────────────────────── */

/* Initialise the SPI peripheral at the requested baud rate and mode.
 * The actual clock may be slightly slower due to integer division.
 * Returns ERR_CODE_OK or ERR_CODE_ARG if any parameter is invalid. */
err_code_t spi_master_init(spi_master_periph_t periph,
                            uint32_t            clock_hz,
                            spi_master_mode_t   mode);

/* Disable the SPI peripheral and gate its RCC clock.
 * Returns ERR_CODE_OK or ERR_CODE_ARG if `periph` is out of range. */
err_code_t spi_master_deinit(spi_master_periph_t periph);

/* Begin an asynchronous transmit of `len` bytes from `buf`.
 * `cb(ctx)` is called from the SPI RXNE ISR when the transfer is done.
 * Returns ERR_CODE_OK, ERR_CODE_BUSY if already transferring, or ERR_CODE_ARG if
 * the peripheral is not initialised or parameters are invalid. */
err_code_t spi_master_write(spi_master_periph_t    periph,
                             const uint8_t         *buf,
                             uint8_t                len,
                             spi_master_callback_t  cb,
                             void                  *ctx);

#endif /* SPI_MASTER_H */
