#include "spi_slave.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "stm32f4xx.h"
#include "err_codes.h"

typedef struct
{
    SPI_TypeDef  *spi;

    bool          initialized;

    uint8_t       tx_buf[SPI_SLAVE_TX_BUF_MAX];
    uint8_t       tx_len;
    uint8_t       tx_idx;   /* circular index into tx_buf */
} spi_slave_state_t;

static spi_slave_state_t states[SPI_PERIPH_COUNT] = {
    [SPI_PERIPH_SPI1] = { .spi = SPI1 },
    [SPI_PERIPH_SPI2] = { .spi = SPI2 },
};

/* ── ISR ──────────────────────────────────────────────────────────── */

static void spi_slave_isr(void *ctx)
{
    spi_slave_state_t *s   = (spi_slave_state_t *)ctx;
    SPI_TypeDef       *spi = s->spi;
    uint32_t           sr  = spi->SR;

    /* Drain RX first to prevent OVR — every shifted-out byte causes a
     * byte to be shifted in; that byte must be read before the next
     * RXNE event arrives. */
    if (sr & SPI_SR_RXNE)
    {
        (void)spi->DR;
    }

    /* Load next TX byte (circular).  If the DUT clocks more bytes than
     * the buffer holds, output 0x00 for the excess. */
    if ((sr & SPI_SR_TXE) && (spi->CR2 & SPI_CR2_TXEIE))
    {
        uint8_t byte = 0x00U;
        if (s->tx_idx < s->tx_len)
        {
            byte = s->tx_buf[s->tx_idx];
            s->tx_idx++;
            if (s->tx_idx >= s->tx_len)
                s->tx_idx = 0U;
        }
        spi->DR = byte;
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

err_code_t spi_slave_init(spi_periph_t periph,
                          spi_mode_t   mode)
{
    if (periph >= SPI_PERIPH_COUNT || mode > SPI_MODE_MAX)
        return ERR_INVALID_PARAMETER;

    spi_slave_state_t *s = &states[periph];

    err_code_t err = spi_claim(periph, SPI_ROLE_SLAVE, spi_slave_isr, s);
    if (err != ERR_SUCCESS)
        return err;

    spi_rcc_enable(periph);

    SPI_TypeDef *spi = s->spi;
    spi->CR1 = 0U;
    spi->CR2 = 0U;

    uint32_t cpol = ((uint32_t)mode & SPI_MODE_CPOL_BIT) ? SPI_CR1_CPOL : 0U;
    uint32_t cpha = ((uint32_t)mode & SPI_MODE_CPHA_BIT) ? SPI_CR1_CPHA : 0U;

    /* Slave mode: MSTR=0, hardware NSS (SSM=0), 8-bit, MSB first. */
    spi->CR1 = cpol | cpha;
    spi->CR1 |= SPI_CR1_SPE;

    s->tx_idx = 0U;
    s->initialized = true;

    spi_nvic_enable(periph);

    /* Preload 0x00 so the shift register has something to clock out
     * if the master asserts NSS before spi_slave_set_tx is called. */
    spi->DR = 0x00U;

    /* Enable RXNEIE now so OVR is prevented if the master clocks during
     * setup. TXEIE is deferred to spi_slave_set_tx when real data exists. */
    spi->CR2 = SPI_CR2_RXNEIE;

    return ERR_SUCCESS;
}

err_code_t spi_slave_deinit(spi_periph_t periph)
{
    if (periph >= SPI_PERIPH_COUNT)
        return ERR_INVALID_PARAMETER;

    spi_slave_state_t *s = &states[periph];

    spi_nvic_disable(periph);

    s->spi->CR1 = 0U;
    s->spi->CR2 = 0U;

    spi_rcc_disable(periph);

    s->initialized = false;
    s->tx_len      = 0U;
    s->tx_idx      = 0U;

    (void)spi_release(periph);
    return ERR_SUCCESS;
}

err_code_t spi_slave_set_tx(spi_periph_t   periph,
                            const uint8_t *buf,
                            uint8_t        len)
{
    if (periph >= SPI_PERIPH_COUNT || buf == NULL || len == 0U || len > SPI_SLAVE_TX_BUF_MAX)
        return ERR_INVALID_PARAMETER;

    spi_slave_state_t *s = &states[periph];
    if (!s->initialized)
        return ERR_INVALID_PARAMETER;

    /* Disable TXEIE while updating the buffer so the ISR cannot read
     * a partially-written state. RXNEIE stays active so OVR is
     * prevented if the DUT happens to be mid-transaction. */
    s->spi->CR2 &= ~SPI_CR2_TXEIE;

    memcpy(s->tx_buf, buf, len);
    s->tx_len = len;
    s->tx_idx = 0U;

    /* Preload byte 0 so the peripheral has fresh data ready. */
    s->spi->DR  = s->tx_buf[0];
    s->tx_idx   = (len > 1U) ? 1U : 0U;

    s->spi->CR2 |= SPI_CR2_TXEIE;
    return ERR_SUCCESS;
}
