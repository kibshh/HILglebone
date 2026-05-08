#include "digital_out_pulse_timer.h"

#include <assert.h>
#include <stddef.h>

#include "stm32f4xx.h"

/* ── Slot table ───────────────────────────────────────────────────── */

/* Which APB bus owns the timer's RCC enable bit. Required because
 * TIM2/TIM5 sit on APB1 while TIM9/10/11 sit on APB2. */
typedef enum
{
    BUS_APB1,
    BUS_APB2,
} pulse_bus_t;

/* Maps the abstract slot index to the concrete TIM peripheral plus the
 * IRQ vector that fires its update interrupt. The 16-bit slots
 * (TIM9/10/11) share their IRQ vectors with TIM1 sub-functions on
 * STM32F401RE; TIM1 is not used in this firmware so the handlers can
 * treat those vectors as TIM<N>-only. TIM2/TIM5 have dedicated vectors. */
typedef struct
{
    TIM_TypeDef               *tim;
    IRQn_Type                  irq;

    pulse_bus_t                bus;
    uint32_t                   rcc_mask;     /* APB1ENR or APB2ENR bit */
    uint32_t                   max_pulse_us; /* 16-bit slots: 0xFFFF; 32-bit: 0xFFFFFFFF */

    pulse_timer_hw_callback_t  cb;           /* registered at acquire() */
    uint8_t                    internal_id;  /* opaque tag for the callback */
    uint8_t                    in_use;
} pulse_slot_t;

static pulse_slot_t slots[PULSE_TIMER_HW_MAX_COUNT];

/* Both APB1 and APB2 timer clocks run at 84 MHz here:
 *   APB2 prescaler = /1 -> APB2 = 84 MHz, timer clock = 84 MHz
 *   APB1 prescaler = /2 -> APB1 = 42 MHz, timer clock = 84 MHz (×2 rule)
 * So a single PSC value gives a 1 µs tick on every timer in the pool. */
#define PULSE_TIMER_HW_PRESCALER    (84U - 1U)

/* NVIC priority. Same band as the UART ISR (6) -- below the FreeRTOS
 * syscall priority floor (5), so the ISRs may NOT call FromISR APIs.
 * They don't: they only write BSRR via the registered callback. */
#define PULSE_TIMER_HW_IRQ_PRIORITY 6U

#define SLOT_IN_USE     1U
#define SLOT_FREE       0U

/* ── One-time init ────────────────────────────────────────────────── */

void pulse_timer_hw_init(void)
{
    /* Slot index -> peripheral mapping. The order matches the
     * pulse_timer_hw_slot_t enum in the header; if you reorder the
     * enum, reorder these too. acquire() filters by `kind`, so the
     * order doesn't determine selection priority any more. */
    slots[PULSE_TIMER_HW_SLOT_TIM2] = (pulse_slot_t){
        .tim          = TIM2,
        .irq          = TIM2_IRQn,
        .bus          = BUS_APB1,
        .rcc_mask     = RCC_APB1ENR_TIM2EN,
        .max_pulse_us = PULSE_TIMER_HW_MAX_PULSE_US_32,
    };
    slots[PULSE_TIMER_HW_SLOT_TIM5] = (pulse_slot_t){
        .tim          = TIM5,
        .irq          = TIM5_IRQn,
        .bus          = BUS_APB1,
        .rcc_mask     = RCC_APB1ENR_TIM5EN,
        .max_pulse_us = PULSE_TIMER_HW_MAX_PULSE_US_32,
    };
    slots[PULSE_TIMER_HW_SLOT_TIM9] = (pulse_slot_t){
        .tim          = TIM9,
        .irq          = TIM1_BRK_TIM9_IRQn,
        .bus          = BUS_APB2,
        .rcc_mask     = RCC_APB2ENR_TIM9EN,
        .max_pulse_us = PULSE_TIMER_HW_MAX_PULSE_US_16,
    };
    slots[PULSE_TIMER_HW_SLOT_TIM10] = (pulse_slot_t){
        .tim          = TIM10,
        .irq          = TIM1_UP_TIM10_IRQn,
        .bus          = BUS_APB2,
        .rcc_mask     = RCC_APB2ENR_TIM10EN,
        .max_pulse_us = PULSE_TIMER_HW_MAX_PULSE_US_16,
    };
    slots[PULSE_TIMER_HW_SLOT_TIM11] = (pulse_slot_t){
        .tim          = TIM11,
        .irq          = TIM1_TRG_COM_TIM11_IRQn,
        .bus          = BUS_APB2,
        .rcc_mask     = RCC_APB2ENR_TIM11EN,
        .max_pulse_us = PULSE_TIMER_HW_MAX_PULSE_US_16,
    };

    for (unsigned i = 0; i < PULSE_TIMER_HW_MAX_COUNT; ++i)
    {
        slots[i].cb          = NULL;
        slots[i].internal_id = 0U;
        slots[i].in_use      = SLOT_FREE;
    }
}

/* ── Slot management ──────────────────────────────────────────────── */

/* Bring the timer to a known idle state: stopped, no pending IRQ, fresh
 * prescaler/ARR loaded, one-shot mode armed. Caller must already hold
 * the slot. */
static void slot_configure_idle(pulse_slot_t *s)
{
    assert(s != NULL);
    TIM_TypeDef *tim = s->tim;

    /* Disable counter & clear all status flags before reconfiguring. */
    tim->CR1  = 0U;
    tim->DIER = 0U;
    tim->SR   = 0U;
    tim->CNT  = 0U;

    tim->PSC  = PULSE_TIMER_HW_PRESCALER;
    tim->ARR  = 0U;

    /* OPM = one-pulse: counter auto-stops on update.
     * URS = update request source: only overflow generates UIF, not the
     *       upcoming EGR-driven preload (avoids a spurious early IRQ). */
    tim->CR1  = TIM_CR1_OPM | TIM_CR1_URS;

    /* Force a manual update so PSC (and ARR=0) are loaded into the
     * shadow registers; URS keeps this from setting UIF. */
    tim->EGR  = TIM_EGR_UG;
    tim->SR   = 0U;   /* belt and braces */
}

/* RCC enable / disable abstracted over the bus the slot lives on. */
static void rcc_clock_enable(const pulse_slot_t *s)
{
    assert(s != NULL);
    if (s->bus == BUS_APB1)
    {
        RCC->APB1ENR |= s->rcc_mask;
        /* Dummy read to ensure RCC clock enable has taken effect before continuing */
        (void)RCC->APB1ENR;
    }
    else
    {
        RCC->APB2ENR |= s->rcc_mask;
        (void)RCC->APB2ENR;
    }
}

static void rcc_clock_disable(const pulse_slot_t *s)
{
    assert(s != NULL);
    if (s->bus == BUS_APB1)
    {
        RCC->APB1ENR &= ~s->rcc_mask;
    }
    else
    {
        RCC->APB2ENR &= ~s->rcc_mask;
    }
}

/* Look up the slot currently bound to `internal_id`. Returns NULL if
 * not found -- used by all public functions to resolve the key. */
static pulse_slot_t *find_slot_by_id(uint8_t internal_id)
{
    for (unsigned i = 0; i < PULSE_TIMER_HW_MAX_COUNT; ++i)
    {
        if (slots[i].in_use && slots[i].internal_id == internal_id)
        {
            return &slots[i];
        }
    }
    return NULL;
}

bool pulse_timer_hw_acquire(uint8_t                   internal_id,
                            pulse_timer_hw_kind_t     kind,
                            pulse_timer_hw_callback_t cb)
{
    assert(cb != NULL);

    /* Each `kind` is satisfied only from its own sub-pool. The 32-bit
     * (LONG) slots have max_pulse_us == 0xFFFFFFFF, the 16-bit (SHORT)
     * slots have 0xFFFF -- compare directly to filter. */
    const uint32_t want_max = (kind == PULSE_TIMER_HW_KIND_LONG)
                              ? PULSE_TIMER_HW_MAX_PULSE_US_32
                              : PULSE_TIMER_HW_MAX_PULSE_US_16;

    for (uint8_t i = 0; i < PULSE_TIMER_HW_MAX_COUNT; ++i)
    {
        pulse_slot_t *s = &slots[i];
        if (s->in_use || s->max_pulse_us != want_max)
        {
            continue;
        }

        rcc_clock_enable(s);

        s->cb          = cb;
        s->internal_id = internal_id;
        s->in_use      = SLOT_IN_USE;

        slot_configure_idle(s);

        NVIC_SetPriority(s->irq, PULSE_TIMER_HW_IRQ_PRIORITY);
        NVIC_EnableIRQ(s->irq);

        return true;
    }
    return false;
}

void pulse_timer_hw_release(uint8_t internal_id)
{
    pulse_slot_t *s = find_slot_by_id(internal_id);
    if (s == NULL)
    {
        return;
    }

    NVIC_DisableIRQ(s->irq);

    TIM_TypeDef *tim = s->tim;
    tim->CR1  = 0U;
    tim->DIER = 0U;
    tim->SR   = 0U;

    /* Cut the clock to save a few µA -- the slot is back in the pool. */
    rcc_clock_disable(s);

    s->cb          = NULL;
    s->internal_id = 0U;
    s->in_use      = SLOT_FREE;
}

uint32_t pulse_timer_hw_get_max_pulse_us(uint8_t internal_id)
{
    const pulse_slot_t *s = find_slot_by_id(internal_id);
    if (s == NULL)
    {
        return 0U;
    }
    return s->max_pulse_us;
}

bool pulse_timer_hw_start(uint8_t internal_id, uint32_t pulse_us)
{
    pulse_slot_t *s = find_slot_by_id(internal_id);
    if (s == NULL)
    {
        return false;
    }

    if (pulse_us == 0U || pulse_us > s->max_pulse_us)
    {
        return false;
    }

    TIM_TypeDef *tim = s->tim;
    assert(tim != NULL);

    /* Stop and reset before reprogramming, so a re-trigger of an
     * already-running pulse takes effect cleanly. */
    tim->CR1  &= ~TIM_CR1_CEN;
    tim->CNT   = 0U;
    tim->ARR   = pulse_us - 1U;

    /* Reload PSC/ARR via update event. URS=1 (set in configure_idle)
     * ensures this does NOT raise UIF. */
    tim->EGR   = TIM_EGR_UG;
    tim->SR    = 0U;            /* clear any leftover flags */
    tim->DIER  = TIM_DIER_UIE;  /* now enable the update interrupt */
    tim->CR1  |= TIM_CR1_CEN;

    return true;
}

/* ── ISRs ─────────────────────────────────────────────────────────── */

/* Common handler: check the timer's UIF; if set, clear and dispatch. */
static inline void handle_isr(pulse_slot_t *s)
{
    assert(s != NULL);
    TIM_TypeDef *tim = s->tim;

    if (!(tim->SR & TIM_SR_UIF))
    {
        return;
    }
    tim->SR = (uint16_t)~TIM_SR_UIF;

    if (s->in_use && s->cb != NULL)
    {
        s->cb(s->internal_id);
    }
}

/* These handler names are defined by the CMSIS startup vector table.
 * The TIM9/10/11 vectors are shared with TIM1 sub-functions on F401RE;
 * since TIM1 is not used by this firmware the dispatch is unambiguous. */

void TIM2_IRQHandler(void)
{
    handle_isr(&slots[PULSE_TIMER_HW_SLOT_TIM2]);
}

void TIM5_IRQHandler(void)
{
    handle_isr(&slots[PULSE_TIMER_HW_SLOT_TIM5]);
}

void TIM1_BRK_TIM9_IRQHandler(void)
{
    handle_isr(&slots[PULSE_TIMER_HW_SLOT_TIM9]);
}

void TIM1_UP_TIM10_IRQHandler(void)
{
    handle_isr(&slots[PULSE_TIMER_HW_SLOT_TIM10]);
}

void TIM1_TRG_COM_TIM11_IRQHandler(void)
{
    handle_isr(&slots[PULSE_TIMER_HW_SLOT_TIM11]);
}
