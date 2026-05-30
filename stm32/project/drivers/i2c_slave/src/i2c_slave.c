#include "i2c_slave.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "stm32f4xx.h"
#include "err_codes.h"
#include "stm32_clk.h"

/* ── Internal constants ───────────────────────────────────────────── */

/* FREQ field in CR2 = PCLK1 in MHz (must equal APB1 clock / 1 MHz). */
#define I2C_CR2_FREQ_VAL    ((uint32_t)(APB1_CLOCK_HZ / 1000000U))

/* Standard-mode / fast-mode boundaries. */
#define I2C_STD_MAX_HZ      100000U
#define I2C_FAST_MAX_HZ     400000U

/* Address space limits. */
#define I2C_7BIT_ADDR_MAX   0x7FU
#define I2C_10BIT_ADDR_MAX  0x3FFU

/* Bit 14 of OAR1 must always be 1 by software (RM0368 §18.6.3). */
#define I2C_OAR1_RESERVED14 (1U << 14)

/* NVIC priority — same level as SPI so all peripheral ISRs share a floor. */
#define I2C_IRQ_PRIORITY    6U

/* ── Transaction phase ────────────────────────────────────────────── */

typedef enum
{
    I2C_PHASE_IDLE,
    I2C_PHASE_RX_REG_ADDR,  /* collecting register address byte(s) from DUT */
    I2C_PHASE_RX_DATA,      /* DUT writing data bytes into regmap */
    I2C_PHASE_TX,           /* STM32 transmitting regmap bytes to DUT */
} i2c_phase_t;

/* ── Per-peripheral state ─────────────────────────────────────────── */

typedef struct
{
    I2C_TypeDef *i2c;
    bool         initialized;

    /* Register-map reference — not owned. */
    uint8_t     *regmap;
    uint16_t     regmap_len;
    uint8_t      reg_addr_width;
    uint8_t      auto_inc_mode;
    bool         auto_inc_wrap;
    bool         writes_allowed;

    /* Per-transaction state, reset on every STOPF / AF. */
    i2c_phase_t  phase;
    uint16_t     reg_ptr;
    uint16_t     reg_addr_acc;
    uint8_t      reg_addr_cnt;
} i2c_slave_state_t;

/* ── Hardware mapping ─────────────────────────────────────────────── */

typedef struct
{
    IRQn_Type ev_irq;
    IRQn_Type er_irq;
} i2c_hw_t;

static const i2c_hw_t hw[I2C_PERIPH_COUNT] = {
    [I2C_PERIPH_I2C1] = { .ev_irq = I2C1_EV_IRQn, .er_irq = I2C1_ER_IRQn },
    [I2C_PERIPH_I2C2] = { .ev_irq = I2C2_EV_IRQn, .er_irq = I2C2_ER_IRQn },
    [I2C_PERIPH_I2C3] = { .ev_irq = I2C3_EV_IRQn, .er_irq = I2C3_ER_IRQn },
};

static i2c_slave_state_t states[I2C_PERIPH_COUNT] = {
    [I2C_PERIPH_I2C1] = { .i2c = I2C1 },
    [I2C_PERIPH_I2C2] = { .i2c = I2C2 },
    [I2C_PERIPH_I2C3] = { .i2c = I2C3 },
};

/* ── RCC helpers ──────────────────────────────────────────────────── */

static void rcc_enable(i2c_periph_t periph)
{
    switch (periph)
    {
    case I2C_PERIPH_I2C1:
        RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;
        (void)RCC->APB1ENR;   /* read-back fence: ensures the clock is running
                                * before the first peripheral register is touched */
        break;
    case I2C_PERIPH_I2C2:
        RCC->APB1ENR |= RCC_APB1ENR_I2C2EN;
        (void)RCC->APB1ENR;
        break;
    case I2C_PERIPH_I2C3:
        RCC->APB1ENR |= RCC_APB1ENR_I2C3EN;
        (void)RCC->APB1ENR;
        break;
    default:
        break;
    }
}

static void rcc_disable(i2c_periph_t periph)
{
    switch (periph)
    {
    case I2C_PERIPH_I2C1: RCC->APB1ENR &= ~RCC_APB1ENR_I2C1EN; break;
    case I2C_PERIPH_I2C2: RCC->APB1ENR &= ~RCC_APB1ENR_I2C2EN; break;
    case I2C_PERIPH_I2C3: RCC->APB1ENR &= ~RCC_APB1ENR_I2C3EN; break;
    default: break;
    }
}

/* ── Clock register helpers ───────────────────────────────────────── */

static uint32_t compute_ccr(uint32_t clock_hz)
{
    /* CCR controls the SCL half-period in units of PCLK1 cycles.
     * Standard mode: equal Tlow and Thigh, so CCR = PCLK1 / (2 × freq).
     * Fast mode DUTY=0 gives Tlow:Thigh = 2:1, so CCR = PCLK1 / (3 × freq).
     * I2C_CCR_FS must be set in the CCR register to select fast mode. */
    if (clock_hz <= I2C_STD_MAX_HZ)
        return APB1_CLOCK_HZ / (2U * clock_hz);

    return I2C_CCR_FS | (APB1_CLOCK_HZ / (3U * clock_hz));
}

static uint32_t compute_trise(uint32_t clock_hz)
{
    /* TRISE is the maximum SCL/SDA rise time expressed as a count of
     * PCLK1 cycles + 1.  The peripheral uses it to know when a rising
     * edge is valid and won't mis-sample a slow-rising signal.
     * I2C spec: standard mode max rise = 1000 ns, fast mode max = 300 ns. */
    if (clock_hz <= I2C_STD_MAX_HZ)
        return I2C_CR2_FREQ_VAL + 1U;                     /* 1000 ns / (1/PCLK1_MHz) + 1 */

    return (I2C_CR2_FREQ_VAL * 3U) / 10U + 1U;            /* 300 ns / (1/PCLK1_MHz) + 1  */
}

/* ── Regmap byte helpers ──────────────────────────────────────────── */

static uint8_t regmap_read(i2c_slave_state_t *s)
{
    assert(s->regmap != NULL);

    if (s->reg_ptr >= s->regmap_len)
        return 0x00U;

    uint8_t byte = s->regmap[s->reg_ptr];

    bool do_inc = (s->auto_inc_mode == I2C_SLAVE_AUTO_INC_READ ||
                   s->auto_inc_mode == I2C_SLAVE_AUTO_INC_BOTH);
    if (do_inc)
    {
        if (s->reg_ptr + 1U < s->regmap_len)
            s->reg_ptr++;
        else if (s->auto_inc_wrap)
            s->reg_ptr = 0U;
        /* else saturate at last register */
    }

    return byte;
}

static void regmap_write(i2c_slave_state_t *s, uint8_t byte)
{
    assert(s->regmap != NULL);

    if (!s->writes_allowed || s->reg_ptr >= s->regmap_len)
        return;

    s->regmap[s->reg_ptr] = byte;

    bool do_inc = (s->auto_inc_mode == I2C_SLAVE_AUTO_INC_WRITE ||
                   s->auto_inc_mode == I2C_SLAVE_AUTO_INC_BOTH);
    if (do_inc)
    {
        if (s->reg_ptr + 1U < s->regmap_len)
            s->reg_ptr++;
        else if (s->auto_inc_wrap)
            s->reg_ptr = 0U;
    }
}

/* ── ISR helpers ──────────────────────────────────────────────────── */

static void ev_isr(i2c_slave_state_t *s)
{
    I2C_TypeDef *i2c = s->i2c;
    uint32_t     sr1 = i2c->SR1;

    /* ADDR: address matched.  The hardware-mandated clear sequence is:
     * read SR1 (done above), then read SR2.  Reading SR2 atomically clears
     * the ADDR flag, releases any clock stretch held since the match, and
     * exposes the TRA bit that tells us the transfer direction. */
    if (sr1 & I2C_SR1_ADDR)
    {
        uint32_t sr2 = i2c->SR2;

        if (sr2 & I2C_SR2_TRA)
        {
            /* Slave transmitter: DUT wants to read from us.
             * The hardware sets TXE immediately after ADDR is cleared
             * (DR is empty).  With ITBUFEN set, TXE fires the event ISR
             * again — the TX branch below handles it in the same or the
             * next invocation, with clock stretch holding SCL low until
             * DR is written. */
            s->phase = I2C_PHASE_TX;
        }
        else
        {
            s->reg_addr_acc = 0U;
            s->reg_addr_cnt = 0U;

            if (s->reg_addr_width == I2C_SLAVE_REG_NONE)
            {
                /* Streaming sensor: no register address phase. */
                s->reg_ptr = 0U;
                s->phase   = I2C_PHASE_RX_DATA;
            }
            else
            {
                s->phase = I2C_PHASE_RX_REG_ADDR;
            }
        }
    }

    /* Byte received from DUT. */
    if (sr1 & I2C_SR1_RXNE)
    {
        uint8_t byte = (uint8_t)i2c->DR;

        if (s->phase == I2C_PHASE_RX_REG_ADDR)
        {
            uint8_t expected = (s->reg_addr_width == I2C_SLAVE_REG_16) ? 2U : 1U;

            /* Accumulate register address bytes MSB-first (big-endian). */
            s->reg_addr_acc = (uint16_t)((s->reg_addr_acc << 8) | byte);

            s->reg_addr_cnt++;

            if (s->reg_addr_cnt >= expected)
            {
                s->reg_ptr = s->reg_addr_acc;
                /* Transition to RX_DATA regardless of what comes next.
                 * If the DUT sends more bytes it is writing to registers
                 * (RXNE events continue in RX_DATA).  If the DUT issues
                 * a repeated START for a read, the ADDR handler fires
                 * again with TRA=1 and switches us to I2C_PHASE_TX. */
                s->phase = I2C_PHASE_RX_DATA;
            }
        }
        else if (s->phase == I2C_PHASE_RX_DATA)
        {
            regmap_write(s, byte);
        }
    }

    /* TXE: DR is empty — load the next byte so the shift register is fed
     * before the master clocks the next bit.  Clock stretching (NOSTRETCH=0)
     * holds SCL low until DR is written, so there is no race.
     * BTF in TX mode means both DR and the shift register have been shifted
     * out; TXE is always set when BTF is set, so checking both with a single
     * OR and one DR write is correct — the write clears both flags. */
    if (s->phase == I2C_PHASE_TX)
    {
        if (sr1 & (I2C_SR1_TXE | I2C_SR1_BTF))
            i2c->DR = regmap_read(s);
    }

    /* STOPF: stop condition detected on the bus (slave-receiver path only;
     * slave-transmitter ends with an AF in the error ISR instead).
     * Hardware-mandated clear sequence: read SR1 (done at top of ISR),
     * then write any value to CR1 while PE=1.  We write the current CR1
     * value back to satisfy this without changing any other bits. */
    if (sr1 & I2C_SR1_STOPF)
    {
        i2c->CR1 |= I2C_CR1_PE;
        s->phase = I2C_PHASE_IDLE;
    }
}

static void er_isr(i2c_slave_state_t *s)
{
    I2C_TypeDef *i2c = s->i2c;

    /* AF (acknowledge failure) is the normal end of a slave-transmitter
     * transaction: the master sends NACK after its last byte to signal it
     * wants no more data. BERR, ARLO and OVR are unexpected bus errors.
     * All of these flags are cleared by reading SR1 then writing 0 to it —
     * the read satisfies the hardware "read SR1 first" requirement and the
     * write-zero clears whichever error bits are set. */
    (void)i2c->SR1;
    i2c->SR1  = 0U;
    s->phase  = I2C_PHASE_IDLE;
}

/* ── IRQ handlers ─────────────────────────────────────────────────── */

void I2C1_EV_IRQHandler(void) { ev_isr(&states[I2C_PERIPH_I2C1]); }
void I2C1_ER_IRQHandler(void) { er_isr(&states[I2C_PERIPH_I2C1]); }
void I2C2_EV_IRQHandler(void) { ev_isr(&states[I2C_PERIPH_I2C2]); }
void I2C2_ER_IRQHandler(void) { er_isr(&states[I2C_PERIPH_I2C2]); }
void I2C3_EV_IRQHandler(void) { ev_isr(&states[I2C_PERIPH_I2C3]); }
void I2C3_ER_IRQHandler(void) { er_isr(&states[I2C_PERIPH_I2C3]); }

/* ── Public API ───────────────────────────────────────────────────── */

static bool cfg_valid(i2c_periph_t periph, uint32_t clock_hz, uint8_t addr_mode,
                      uint16_t primary_addr, uint16_t secondary_addr,
                      const i2c_slave_cfg_t *cfg)
{
    if (periph >= I2C_PERIPH_COUNT)
        return false;
    if (cfg == NULL || cfg->regmap == NULL || cfg->regmap_len == 0U)
        return false;
    if (addr_mode > I2C_SLAVE_ADDR_10BIT)
        return false;
    if (clock_hz == 0U || clock_hz > I2C_FAST_MAX_HZ)
        return false;
    if (addr_mode == I2C_SLAVE_ADDR_7BIT)
    {
        if (primary_addr > I2C_7BIT_ADDR_MAX)
            return false;
        if (secondary_addr != 0U && secondary_addr > I2C_7BIT_ADDR_MAX)
            return false;
    }
    else
    {
        if (primary_addr == 0U || primary_addr > I2C_10BIT_ADDR_MAX)
            return false;
        if (secondary_addr != 0U)
            return false;
    }
    if (cfg->reg_addr_width > I2C_SLAVE_REG_16)
        return false;
    if (cfg->auto_inc_mode > I2C_SLAVE_AUTO_INC_BOTH)
        return false;
    return true;
}

err_code_t i2c_slave_init(i2c_periph_t          periph,
                          uint32_t              clock_hz,
                          uint8_t               addr_mode,
                          uint16_t              primary_addr,
                          uint16_t              secondary_addr,
                          const i2c_slave_cfg_t *cfg)
{
    if (!cfg_valid(periph, clock_hz, addr_mode, primary_addr, secondary_addr, cfg))
        return ERR_INVALID_PARAMETER;

    i2c_slave_state_t *s = &states[periph];

    if (s->initialized)
        return ERR_PERIPHERAL_BUSY;

    rcc_enable(periph);

    I2C_TypeDef *i2c = s->i2c;

    /* Software reset: SWRST flushes any stuck state left by a previous
     * bus lockup (e.g. slave stuck driving SDA low).  The bit must be
     * cleared by software before any other register is written. */
    i2c->CR1 = I2C_CR1_SWRST;
    i2c->CR1 = 0U;

    /* CR2 FREQ must be set to PCLK1 in MHz before writing CCR/TRISE —
     * the peripheral uses it for internal timing generation.
     * ITEVTEN enables the event ISR for ADDR, BTF, STOPF.
     * ITBUFEN additionally enables it for RXNE and TXE — without this
     * bit, individual data bytes would never generate an interrupt and
     * the register map could not be served byte-by-byte. */
    i2c->CR2 = I2C_CR2_FREQ_VAL | I2C_CR2_ITEVTEN | I2C_CR2_ITERREN | I2C_CR2_ITBUFEN;

    /* Timing registers. */
    i2c->CCR   = compute_ccr(clock_hz);
    i2c->TRISE = compute_trise(clock_hz);

    /* OAR1: primary slave address.  Bit 14 must always be 1 (hardware req). */
    if (addr_mode == I2C_SLAVE_ADDR_7BIT)
        i2c->OAR1 = I2C_OAR1_RESERVED14 | ((uint32_t)primary_addr << 1);
    else
        i2c->OAR1 = I2C_OAR1_RESERVED14 | I2C_OAR1_ADDMODE | primary_addr;

    /* OAR2: secondary address (7-bit dual-addressing only). */
    if (secondary_addr != 0U && addr_mode == I2C_SLAVE_ADDR_7BIT)
        i2c->OAR2 = I2C_OAR2_ENDUAL | ((uint32_t)secondary_addr << 1);
    else
        i2c->OAR2 = 0U;

    /* Populate driver state before enabling IRQs. */
    s->regmap         = cfg->regmap;
    s->regmap_len     = cfg->regmap_len;
    s->reg_addr_width = cfg->reg_addr_width;
    s->auto_inc_mode  = cfg->auto_inc_mode;
    s->auto_inc_wrap   = cfg->auto_inc_wrap;
    s->writes_allowed  = cfg->writes_allowed;
    s->phase           = I2C_PHASE_IDLE;
    s->reg_ptr         = 0U;
    s->reg_addr_acc    = 0U;
    s->reg_addr_cnt    = 0U;
    s->initialized     = true;

    /* CR1: PE enables the peripheral; ACK must be set so the slave
     * acknowledges its own address on each transaction.
     * NOSTRETCH=0 (clock_stretch=true): hardware holds SCL low whenever
     * DR is not ready, giving the ISR time to load the next byte.
     * NOSTRETCH=1 (clock_stretch=false): no hold — ISR must be fast enough
     * to feed DR before the master clocks the next bit.
     * ENGC: respond to the general-call address (0x00) in addition to
     * the configured slave addresses. */
    uint32_t cr1 = I2C_CR1_PE | I2C_CR1_ACK;
    if (!cfg->clock_stretch)
        cr1 |= I2C_CR1_NOSTRETCH;
    if (cfg->general_call)
        cr1 |= I2C_CR1_ENGC;
    i2c->CR1 = cr1;

    NVIC_SetPriority(hw[periph].ev_irq, I2C_IRQ_PRIORITY);
    NVIC_EnableIRQ(hw[periph].ev_irq);
    NVIC_SetPriority(hw[periph].er_irq, I2C_IRQ_PRIORITY);
    NVIC_EnableIRQ(hw[periph].er_irq);

    return ERR_SUCCESS;
}

err_code_t i2c_slave_deinit(i2c_periph_t periph)
{
    if (periph >= I2C_PERIPH_COUNT)
        return ERR_INVALID_PARAMETER;

    i2c_slave_state_t *s = &states[periph];

    NVIC_DisableIRQ(hw[periph].ev_irq);
    NVIC_DisableIRQ(hw[periph].er_irq);

    s->i2c->CR1 = 0U;
    s->i2c->CR2 = 0U;

    rcc_disable(periph);

    s->initialized = false;
    s->regmap      = NULL;
    s->regmap_len  = 0U;
    s->phase       = I2C_PHASE_IDLE;

    return ERR_SUCCESS;
}
