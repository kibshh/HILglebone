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
#include "spi.h"

/* Maximum achievable SPI clock per peripheral (APB clock / 2). */
#define SPI_MASTER_SPI1_MAX_CLOCK_HZ    42000000UL   /* APB2 84 MHz / 2 */
#define SPI_MASTER_SPI2_MAX_CLOCK_HZ    21000000UL   /* APB1 42 MHz / 2 */

/* ── Transfer callback ────────────────────────────────────────────── */

/* Called from ISR context when the last byte has been shifted out.
 * Keep it short: toggle CS/LDAC GPIOs and give a FreeRTOS semaphore. */
typedef void (*spi_master_callback_t)(void *ctx);

/* ── API ──────────────────────────────────────────────────────────── */

/* Initialise the SPI peripheral at the requested baud rate and mode.
 * The actual clock may be slightly slower due to integer division.
 * Returns ERR_SUCCESS, ERR_PERIPHERAL_BUSY if already claimed,
 * or ERR_INVALID_PARAMETER if any parameter is invalid. */
err_code_t spi_master_init(spi_periph_t periph,
                           uint32_t     clock_hz,
                           spi_mode_t   mode);

/* Disable the SPI peripheral and release the peripheral slot.
 * Returns ERR_SUCCESS or ERR_INVALID_PARAMETER if `periph` is out of range. */
err_code_t spi_master_deinit(spi_periph_t periph);

/* Begin an asynchronous transmit of `len` bytes from `buf`.
 * `cb(ctx)` is called from the SPI RXNE ISR when the transfer is done.
 * Returns ERR_SUCCESS, ERR_PERIPHERAL_BUSY if already transferring, or
 * ERR_INVALID_PARAMETER if the peripheral is not initialised or parameters are invalid. */
err_code_t spi_master_write(spi_periph_t           periph,
                            const uint8_t         *buf,
                            uint8_t                len,
                            spi_master_callback_t  cb,
                            void                  *ctx);

#endif /* SPI_MASTER_H */
