#include "spi_master.h"

#include <assert.h>
#include <stddef.h>

#include "stm32f4xx.h"
#include "err_codes.h"
#include "stm32_clk.h"

/* ── Master-only constants ────────────────────────────────────────── */

/* CR1 BR[2:0] field: divisor = 2^(BR+1), range 0..7. */
#define SPI_BR_MAX          7U
#define SPI_BR_MASK         0x7U

/* ── Per-peripheral transfer state ───────────────────────────────── */

typedef struct
{
    SPI_TypeDef           *spi;
    uint32_t               apb_clk_hz;

    bool                   initialized;
    bool                   busy;

    const uint8_t         *tx_buf;
    uint8_t                tx_len;
    uint8_t                tx_idx;
    uint8_t                rx_count;

    spi_master_callback_t  cb;
    void                  *cb_ctx;
} spi_master_state_t;

static spi_master_state_t states[SPI_PERIPH_COUNT] = {
    [SPI_PERIPH_SPI1] = { .spi = SPI1, .apb_clk_hz = APB2_CLOCK_HZ },
    [SPI_PERIPH_SPI2] = { .spi = SPI2, .apb_clk_hz = APB1_CLOCK_HZ },
};

/* ── Helpers ──────────────────────────────────────────────────────── */

static uint32_t compute_br(uint32_t apb_clk_hz, uint32_t target_hz)
{
    for (uint32_t br = 0; br <= SPI_BR_MAX; br++)
    {
        if ((apb_clk_hz >> (br + 1U)) <= target_hz)
            return br;
    }
    return SPI_BR_MAX;
}

/* ── ISR handler (registered with the shared spi dispatch layer) ──── */

static void spi_master_isr(void *ctx)
{
    spi_master_state_t *s   = (spi_master_state_t *)ctx;
    SPI_TypeDef        *spi = s->spi;
    uint32_t            sr  = spi->SR;

    /* RX FIRST.  Reading DR clears RXNE; doing this on every event
     * prevents an OVR error from the *next* byte arriving while RXNE
     * is still set from this one.  Completion is signalled by the RX
     * side: the last byte received means the last bit has finished
     * shifting out on MOSI — the slave has fully clocked the frame in.
     *
     * The `tx_idx > 0` guard avoids counting any stale RX that could
     * theoretically be latched before the first TX byte is written;
     * in master mode RXNE cannot fire without a prior TX, but the
     * guard costs nothing and adds robustness against bus glitches. */
    if ((sr & SPI_SR_RXNE) && (s->tx_idx > 0U))
    {
        (void)spi->DR;
        s->rx_count++;

        if (s->rx_count >= s->tx_len)
        {
            /* All bytes physically transmitted.  Disable both
             * interrupts; the TX side already stopped feeding once
             * tx_idx reached tx_len. */
            spi->CR2 &= ~(SPI_CR2_TXEIE | SPI_CR2_RXNEIE);
            s->busy   = false;

            if (s->cb != NULL)
                s->cb(s->cb_ctx);

            return;
        }
    }

    /* TX buffer empty — load the next byte if any remain.  Once the
     * last byte is queued, disable TXEIE; RXNEIE stays on so the
     * trailing RX events still drain and trigger completion. */
    if ((sr & SPI_SR_TXE) && (spi->CR2 & SPI_CR2_TXEIE))
    {
        if (s->tx_idx < s->tx_len)
            spi->DR = s->tx_buf[s->tx_idx++];
        else
            spi->CR2 &= ~SPI_CR2_TXEIE;
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

err_code_t spi_master_init(spi_periph_t periph,
                            uint32_t     clock_hz,
                            spi_mode_t   mode)
{
    if (periph >= SPI_PERIPH_COUNT || clock_hz == 0U || mode > SPI_MODE_MAX)
        return ERR_INVALID_PARAMETER;

    spi_master_state_t *s = &states[periph];

    err_code_t err = spi_claim(periph, SPI_ROLE_MASTER, spi_master_isr, s);
    if (err != ERR_SUCCESS)
        return err;

    spi_rcc_enable(periph);

    SPI_TypeDef *spi = s->spi;

    /* Disable before configuring. */
    spi->CR1 = 0U;
    spi->CR2 = 0U;

    uint32_t br   = compute_br(s->apb_clk_hz, clock_hz);
    /* Bit 1 of the mode value is CPOL; bit 0 is CPHA. */
    uint32_t cpol = ((uint32_t)mode & SPI_MODE_CPOL_BIT) ? SPI_CR1_CPOL : 0U;
    uint32_t cpha = ((uint32_t)mode & SPI_MODE_CPHA_BIT) ? SPI_CR1_CPHA : 0U;

    spi->CR1 = SPI_CR1_MSTR          /* master mode          */
             | SPI_CR1_SSM           /* software CS          */
             | SPI_CR1_SSI           /* NSS internally high  */
             | ((br & SPI_BR_MASK) << SPI_CR1_BR_Pos)
             | cpol
             | cpha;
    /* DFF=0 (8-bit), LSBFIRST=0 (MSB first) — both are 0 by default. */

    spi->CR1 |= SPI_CR1_SPE;

    s->initialized = true;
    s->busy        = false;

    spi_nvic_enable(periph);

    return ERR_SUCCESS;
}

err_code_t spi_master_deinit(spi_periph_t periph)
{
    if (periph >= SPI_PERIPH_COUNT)
        return ERR_INVALID_PARAMETER;

    spi_master_state_t *s = &states[periph];

    spi_nvic_disable(periph);

    s->spi->CR1 = 0U;
    s->spi->CR2 = 0U;

    spi_rcc_disable(periph);

    s->initialized = false;
    s->busy        = false;

    (void)spi_release(periph);
    return ERR_SUCCESS;
}

err_code_t spi_master_write(spi_periph_t           periph,
                            const uint8_t         *buf,
                            uint8_t                len,
                            spi_master_callback_t  cb,
                            void                  *ctx)
{
    assert(buf != NULL);
    assert(len > 0U);
    assert(cb  != NULL);

    if (periph >= SPI_PERIPH_COUNT)      return ERR_INVALID_PARAMETER;
    if (!states[periph].initialized)     return ERR_INVALID_PARAMETER;
    if (states[periph].busy)             return ERR_PERIPHERAL_BUSY;

    spi_master_state_t *s = &states[periph];

    s->tx_buf   = buf;
    s->tx_len   = len;
    s->tx_idx   = 0U;
    s->rx_count = 0U;
    s->cb       = cb;
    s->cb_ctx   = ctx;
    s->busy     = true;

    /* Drain any stale RX data and clear an OVR flag if one is set
     * (the OVR-clear sequence is "read SR, then read DR"). */
    (void)s->spi->SR;
    (void)s->spi->DR;

    /* Enable TXE and RXNE interrupts together.  TXE was set when the
     * peripheral was enabled (buffer empty), so the ISR will fire
     * immediately and start sending byte 0.  RXNE must be enabled
     * from the start: every transmitted byte triggers an RXNE that
     * MUST be drained before the next byte completes shifting,
     * otherwise the peripheral raises an overrun (OVR) error. */
    s->spi->CR2 |= (SPI_CR2_TXEIE | SPI_CR2_RXNEIE);

    return ERR_SUCCESS;
}
