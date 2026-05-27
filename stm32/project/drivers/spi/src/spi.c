#include "spi.h"

#include <stdbool.h>
#include <stddef.h>

#include "stm32f4xx.h"

/* ── Hardware mapping ─────────────────────────────────────────────── */

typedef struct
{
    IRQn_Type irq;
} spi_hw_t;

static const spi_hw_t hw[SPI_PERIPH_COUNT] = {
    [SPI_PERIPH_SPI1] = { .irq = SPI1_IRQn },
    [SPI_PERIPH_SPI2] = { .irq = SPI2_IRQn },
};

/* ── Claim table ──────────────────────────────────────────────────── */

typedef struct
{
    bool         in_use;
    spi_role_t   role;
    spi_isr_fn_t isr_fn;
    void        *isr_ctx;
} spi_slot_t;

static spi_slot_t slots[SPI_PERIPH_COUNT];

err_code_t spi_claim(spi_periph_t  periph,
                     spi_role_t    role,
                     spi_isr_fn_t  isr_fn,
                     void         *ctx)
{
    if (periph >= SPI_PERIPH_COUNT || role > SPI_ROLE_MAX || isr_fn == NULL)
        return ERR_INVALID_PARAMETER;
    if (slots[periph].in_use)
        return ERR_PERIPHERAL_BUSY;

    slots[periph].in_use  = true;
    slots[periph].role    = role;
    slots[periph].isr_fn  = isr_fn;
    slots[periph].isr_ctx = ctx;
    return ERR_SUCCESS;
}

err_code_t spi_release(spi_periph_t periph)
{
    if (periph >= SPI_PERIPH_COUNT)
        return ERR_INVALID_PARAMETER;

    slots[periph].in_use  = false;
    slots[periph].isr_fn  = NULL;
    slots[periph].isr_ctx = NULL;
    return ERR_SUCCESS;
}

/* ── Shared RCC helpers ───────────────────────────────────────────── */

void spi_rcc_enable(spi_periph_t periph)
{
    if (periph == SPI_PERIPH_SPI1)
    {
        RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
        (void)RCC->APB2ENR;
    }
    else
    {
        RCC->APB1ENR |= RCC_APB1ENR_SPI2EN;
        (void)RCC->APB1ENR;
    }
}

void spi_rcc_disable(spi_periph_t periph)
{
    if (periph == SPI_PERIPH_SPI1)
        RCC->APB2ENR &= ~RCC_APB2ENR_SPI1EN;
    else
        RCC->APB1ENR &= ~RCC_APB1ENR_SPI2EN;
}

/* ── Shared NVIC helpers ──────────────────────────────────────────── */

void spi_nvic_enable(spi_periph_t periph)
{
    NVIC_SetPriority(hw[periph].irq, SPI_IRQ_PRIORITY);
    NVIC_EnableIRQ(hw[periph].irq);
}

void spi_nvic_disable(spi_periph_t periph)
{
    NVIC_DisableIRQ(hw[periph].irq);
}

/* ── IRQ handlers ─────────────────────────────────────────────────── */

void SPI1_IRQHandler(void)
{
    if (slots[SPI_PERIPH_SPI1].isr_fn != NULL)
        slots[SPI_PERIPH_SPI1].isr_fn(slots[SPI_PERIPH_SPI1].isr_ctx);
}

void SPI2_IRQHandler(void)
{
    if (slots[SPI_PERIPH_SPI2].isr_fn != NULL)
        slots[SPI_PERIPH_SPI2].isr_fn(slots[SPI_PERIPH_SPI2].isr_ctx);
}
