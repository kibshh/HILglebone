/**
 * Shared SPI peripheral resource layer.
 *
 * Tracks which SPI peripherals are in use and in which role (master /
 * slave), and dispatches SPI1/SPI2 IRQs to whichever driver currently
 * owns the peripheral.  Both spi_master and spi_slave call spi_claim /
 * spi_release internally; sensor backends never touch this API directly.
 */

#ifndef SPI_H
#define SPI_H

#include "err_codes.h"

/* ── Peripheral identifiers ───────────────────────────────────────── */

typedef enum
{
    SPI_PERIPH_SPI1 = 0,
    SPI_PERIPH_SPI2 = 1,

    SPI_PERIPH_COUNT,
} spi_periph_t;

/* ── SPI mode ─────────────────────────────────────────────────────── */

/*
 * Standard SPI modes.  Bit 1 = CPOL, bit 0 = CPHA.
 *   Mode 0: CPOL=0, CPHA=0 — idle low,  sample on rising
 *   Mode 1: CPOL=0, CPHA=1 — idle low,  sample on falling
 *   Mode 2: CPOL=1, CPHA=0 — idle high, sample on falling
 *   Mode 3: CPOL=1, CPHA=1 — idle high, sample on rising
 */
typedef enum
{
    SPI_MODE_0 = 0,
    SPI_MODE_1 = 1,
    SPI_MODE_2 = 2,
    SPI_MODE_3 = 3,

    SPI_MODE_MAX = SPI_MODE_3,
} spi_mode_t;

/* ── Role ─────────────────────────────────────────────────────────── */

typedef enum
{
    SPI_ROLE_MASTER,
    SPI_ROLE_SLAVE,

    SPI_ROLE_MAX = SPI_ROLE_SLAVE,
} spi_role_t;

/* ── ISR dispatch callback ────────────────────────────────────────── */

/* Called from the SPI IRQ handler with the context pointer supplied at
 * claim time.  Registered by spi_master / spi_slave when they claim. */
typedef void (*spi_isr_fn_t)(void *ctx);

/* ── Shared implementation constants ─────────────────────────────── */

/* NVIC priority shared by all SPI IRQs — below FreeRTOS syscall floor
 * so FromISR() functions are legal inside the handler. */
#define SPI_IRQ_PRIORITY    6U

/* Bit positions within the spi_mode_t value used to extract CPOL/CPHA. */
#define SPI_MODE_CPOL_BIT   0x2U
#define SPI_MODE_CPHA_BIT   0x1U

/* ── API ──────────────────────────────────────────────────────────── */

/* Claim a peripheral for exclusive use in `role`.
 * `isr_fn(ctx)` will be called from the SPI IRQ handler while the
 * peripheral is claimed.
 * Returns ERR_PERIPHERAL_BUSY if already claimed, ERR_INVALID_PARAMETER
 * if periph is out of range or isr_fn is NULL. */
err_code_t spi_claim(spi_periph_t   periph,
                     spi_role_t     role,
                     spi_isr_fn_t   isr_fn,
                     void          *ctx);

/* Release a previously-claimed peripheral.
 * Returns ERR_INVALID_PARAMETER if periph is out of range. */
err_code_t spi_release(spi_periph_t periph);

/* Enable / disable the RCC clock for the given SPI peripheral. */
void spi_rcc_enable(spi_periph_t periph);
void spi_rcc_disable(spi_periph_t periph);

/* Configure NVIC priority and enable / disable the SPI IRQ.
 * spi_nvic_enable also sets SPI_IRQ_PRIORITY before enabling. */
void spi_nvic_enable(spi_periph_t periph);
void spi_nvic_disable(spi_periph_t periph);

#endif /* SPI_H */
