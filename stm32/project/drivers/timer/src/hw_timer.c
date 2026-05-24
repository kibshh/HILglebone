#include "hw_timer.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "err_codes.h"

/* One-shot pulse: 1 µs per tick (PSC = 83 at 84 MHz). */
#define PULSE_PSC           (HW_TIMER_CLOCK_HZ / 1000000UL - 1UL)

/* NVIC priority -- same band as UART ISR, below FreeRTOS syscall floor.
 * Pulse ISR callbacks only write BSRR (atomic), no FreeRTOS calls. */
#define HW_TIMER_IRQ_PRIO   6U

/* ── Internal timer state ─────────────────────────────────────────── */

typedef enum
{
    STATE_FREE  = 0,
    STATE_PULSE,
    STATE_PWM,
} hw_timer_state_t;

typedef struct
{
    TIM_TypeDef        *tim;
    IRQn_Type           irq;
    uint32_t            rcc_apb1_mask;  /* non-zero if on APB1 */
    uint32_t            rcc_apb2_mask;  /* non-zero if on APB2 */
    hw_timer_width_t    width;
    uint8_t             max_ch;         /* max channel number (1-indexed) */

    hw_timer_state_t    state;

    /* PULSE fields */
    uint8_t             pulse_token;
    hw_timer_pulse_cb_t pulse_cb;

    /* PWM fields */
    uint32_t            psc;
    uint32_t            arr;
    uint8_t             pwm_ref_count;
} hw_timer_entry_t;

static hw_timer_entry_t timers[HW_TIMER_COUNT];

/* ── Init ─────────────────────────────────────────────────────────── */

void hw_timer_init(void)
{
    memset(timers, 0, sizeof(timers));

    timers[HW_TIMER_TIM2]  = (hw_timer_entry_t){ .tim = TIM2,  .irq = TIM2_IRQn,                 .rcc_apb1_mask = RCC_APB1ENR_TIM2EN,  .width = HW_TIMER_WIDTH_32, .max_ch = 4 };
    timers[HW_TIMER_TIM3]  = (hw_timer_entry_t){ .tim = TIM3,  .irq = TIM3_IRQn,                 .rcc_apb1_mask = RCC_APB1ENR_TIM3EN,  .width = HW_TIMER_WIDTH_16, .max_ch = 4 };
    timers[HW_TIMER_TIM4]  = (hw_timer_entry_t){ .tim = TIM4,  .irq = TIM4_IRQn,                 .rcc_apb1_mask = RCC_APB1ENR_TIM4EN,  .width = HW_TIMER_WIDTH_16, .max_ch = 4 };
    timers[HW_TIMER_TIM5]  = (hw_timer_entry_t){ .tim = TIM5,  .irq = TIM5_IRQn,                 .rcc_apb1_mask = RCC_APB1ENR_TIM5EN,  .width = HW_TIMER_WIDTH_32, .max_ch = 4 };
    timers[HW_TIMER_TIM9]  = (hw_timer_entry_t){ .tim = TIM9,  .irq = TIM1_BRK_TIM9_IRQn,        .rcc_apb2_mask = RCC_APB2ENR_TIM9EN,  .width = HW_TIMER_WIDTH_16, .max_ch = 2 };
    timers[HW_TIMER_TIM10] = (hw_timer_entry_t){ .tim = TIM10, .irq = TIM1_UP_TIM10_IRQn,        .rcc_apb2_mask = RCC_APB2ENR_TIM10EN, .width = HW_TIMER_WIDTH_16, .max_ch = 1 };
    timers[HW_TIMER_TIM11] = (hw_timer_entry_t){ .tim = TIM11, .irq = TIM1_TRG_COM_TIM11_IRQn,   .rcc_apb2_mask = RCC_APB2ENR_TIM11EN, .width = HW_TIMER_WIDTH_16, .max_ch = 1 };
}

/* ── Clock helpers ────────────────────────────────────────────────── */

static void clock_enable(const hw_timer_entry_t *e)
{
    assert(e != NULL);
    if (e->rcc_apb1_mask)
    {
        RCC->APB1ENR |= e->rcc_apb1_mask;
        (void)RCC->APB1ENR;
    }
    else
    {
        RCC->APB2ENR |= e->rcc_apb2_mask;
        (void)RCC->APB2ENR;
    }
}

static void clock_disable(const hw_timer_entry_t *e)
{
    assert(e != NULL);
    if (e->rcc_apb1_mask)
    {
        RCC->APB1ENR &= ~e->rcc_apb1_mask;
    }
    else
    {
        RCC->APB2ENR &= ~e->rcc_apb2_mask;
    }
}

/* ── Frequency helpers ────────────────────────────────────────────── */

/* Compute PSC and ARR for `freq_hz` on a timer of the given `width`.
 * Maximises ARR (best duty resolution) within the counter's bit-width.
 * Returns true on success, false if the frequency is unachievable. */
static err_code_t compute_psc_arr(uint32_t         freq_hz,
                                  hw_timer_width_t width,
                                  uint32_t        *out_psc,
                                  uint32_t        *out_arr)
{
    assert(out_psc != NULL);
    assert(out_arr != NULL);

    if (freq_hz == 0U || freq_hz > HW_TIMER_FREQ_MAX_HZ)
    {
        return ERR_CODE_ARG;
    }

    uint64_t max_arr = (width == HW_TIMER_WIDTH_32)
                       ? HW_TIMER_ARR_MAX_32
                       : HW_TIMER_ARR_MAX_16;

    uint64_t period = (uint64_t)HW_TIMER_CLOCK_HZ / freq_hz;
    uint64_t psc_p1 = (period + max_arr) / (max_arr + 1UL);
    if (psc_p1 == 0UL) psc_p1 = 1UL;
    if (psc_p1 > (uint64_t)HW_TIMER_PSC_MAX + 1UL) return ERR_CODE_ARG;

    uint64_t arr = period / psc_p1 - 1UL;
    if (arr > max_arr) return ERR_CODE_ARG;

    *out_psc = (uint32_t)(psc_p1 - 1UL);
    *out_arr = (uint32_t)arr;
    return ERR_CODE_OK;
}

/* ── Pulse helpers ────────────────────────────────────────────────── */

static hw_timer_entry_t *find_pulse_entry(uint8_t user_token)
{
    for (unsigned i = 0; i < HW_TIMER_COUNT; ++i)
    {
        if (timers[i].state == STATE_PULSE &&
            timers[i].pulse_token == user_token)
        {
            return &timers[i];
        }
    }
    return NULL;
}

static void pulse_configure_idle(hw_timer_entry_t *e)
{
    assert(e != NULL);
    TIM_TypeDef *tim = e->tim;

    tim->CR1  = 0U;
    tim->DIER = 0U;
    tim->SR   = 0U;
    tim->CNT  = 0U;
    tim->PSC  = PULSE_PSC;
    tim->ARR  = 0U;
    tim->CR1  = TIM_CR1_OPM | TIM_CR1_URS;
    tim->EGR  = TIM_EGR_UG;
    tim->SR   = 0U;
}

/* ── Public API ───────────────────────────────────────────────────── */

uint8_t hw_timer_max_channel(hw_timer_id_t id)
{
    if (id >= HW_TIMER_COUNT) return 0U;
    return timers[id].max_ch;
}

err_code_t hw_timer_pulse_acquire(hw_timer_id_t       id,
                                  uint8_t             user_token,
                                  hw_timer_pulse_cb_t cb)
{
    assert(cb != NULL);

    if (id >= HW_TIMER_COUNT)        return ERR_CODE_ARG;

    hw_timer_entry_t *e = &timers[id];
    if (e->state != STATE_FREE)      return ERR_CODE_BUSY;

    clock_enable(e);
    pulse_configure_idle(e);

    e->pulse_token = user_token;
    e->pulse_cb    = cb;
    e->state       = STATE_PULSE;

    NVIC_SetPriority(e->irq, HW_TIMER_IRQ_PRIO);
    NVIC_EnableIRQ(e->irq);
    return ERR_CODE_OK;
}

void hw_timer_pulse_release(uint8_t user_token)
{
    hw_timer_entry_t *e = find_pulse_entry(user_token);
    if (e == NULL) return;

    NVIC_DisableIRQ(e->irq);

    TIM_TypeDef *tim = e->tim;
    tim->CR1  = 0U;
    tim->DIER = 0U;
    tim->SR   = 0U;

    clock_disable(e);

    e->pulse_token = 0U;
    e->pulse_cb    = NULL;
    e->state       = STATE_FREE;
}

uint32_t hw_timer_pulse_max_us(uint8_t user_token)
{
    const hw_timer_entry_t *e = find_pulse_entry(user_token);
    if (e == NULL) return 0U;
    return (e->width == HW_TIMER_WIDTH_32)
           ? HW_TIMER_MAX_PULSE_US_32
           : HW_TIMER_MAX_PULSE_US_16;
}

err_code_t hw_timer_pulse_start(uint8_t user_token, uint32_t pulse_us)
{
    hw_timer_entry_t *e = find_pulse_entry(user_token);
    if (e == NULL) return ERR_CODE_ARG;

    if (pulse_us == 0U || pulse_us > hw_timer_pulse_max_us(user_token))
    {
        return ERR_CODE_ARG;
    }

    TIM_TypeDef *tim = e->tim;
    assert(tim != NULL);

    tim->CR1  &= ~TIM_CR1_CEN;
    tim->CNT   = 0U;
    tim->ARR   = pulse_us - 1U;
    tim->EGR   = TIM_EGR_UG;
    tim->SR    = 0U;
    tim->DIER  = TIM_DIER_UIE;
    tim->CR1  |= TIM_CR1_CEN;
    return ERR_CODE_OK;
}

err_code_t hw_timer_pwm_acquire(hw_timer_id_t id,
                                uint32_t      freq_hz,
                                uint32_t     *out_arr)
{
    assert(out_arr != NULL);

    if (id >= HW_TIMER_COUNT) return ERR_CODE_ARG;

    hw_timer_entry_t *e = &timers[id];

    if (e->state == STATE_PULSE) return ERR_CODE_BUSY;

    uint32_t psc;
    uint32_t arr;
    if (compute_psc_arr(freq_hz, e->width, &psc, &arr) != ERR_CODE_OK)
    {
        return ERR_CODE_ARG;
    }

    if (e->state == STATE_PWM)
    {
        /* Timer already running: e->psc/arr were stored by the first channel
         * that opened this timer.  The newly computed psc/arr come from this
         * call's freq_hz.  If they differ, the two channels want different
         * frequencies -- impossible on a shared ARR. */
        if (e->psc != psc || e->arr != arr)
        {
            return ERR_CODE_CONFLICT;
        }
        e->pwm_ref_count++;
        *out_arr = e->arr;
        return ERR_CODE_OK;
    }

    /* STATE_FREE: configure and start. */
    clock_enable(e);

    TIM_TypeDef *tim = e->tim;
    tim->CR1  = 0U;
    tim->PSC  = psc;
    tim->ARR  = arr;
    tim->CR1  = TIM_CR1_ARPE;
    tim->EGR  = TIM_EGR_UG;
    tim->SR   = 0U;
    tim->CR1 |= TIM_CR1_CEN;

    e->psc           = psc;
    e->arr           = arr;
    e->pwm_ref_count = 1U;
    e->state         = STATE_PWM;

    *out_arr = arr;
    return ERR_CODE_OK;
}

void hw_timer_pwm_release(hw_timer_id_t id)
{
    if (id >= HW_TIMER_COUNT) return;

    hw_timer_entry_t *e = &timers[id];
    if (e->state != STATE_PWM) return;

    if (e->pwm_ref_count > 0U) e->pwm_ref_count--;

    if (e->pwm_ref_count == 0U)
    {
        e->tim->CR1 = 0U;
        clock_disable(e);
        e->psc   = 0U;
        e->arr   = 0U;
        e->state = STATE_FREE;
    }
}

TIM_TypeDef *hw_timer_handle(hw_timer_id_t id)
{
    if (id >= HW_TIMER_COUNT) return NULL;
    return timers[id].tim;
}

uint32_t hw_timer_pwm_arr(hw_timer_id_t id)
{
    if (id >= HW_TIMER_COUNT) return 0U;
    return timers[id].arr;
}

/* ── ISR handlers ─────────────────────────────────────────────────── */

/* Shared dispatch: only fires the callback when the timer is in PULSE
 * mode (PWM timers do not enable UIE, so this will never trigger for
 * them even if the shared TIM1/TIM9-11 vectors fire for TIM1 reasons). */
static void handle_pulse_isr(hw_timer_id_t id)
{
    hw_timer_entry_t *e   = &timers[id];
    TIM_TypeDef      *tim = e->tim;

    if (!(tim->SR & TIM_SR_UIF)) return;
    tim->SR = (uint32_t)~TIM_SR_UIF;

    if (e->state == STATE_PULSE && e->pulse_cb != NULL)
    {
        e->pulse_cb(e->pulse_token);
    }
}

void TIM2_IRQHandler(void)                  { handle_pulse_isr(HW_TIMER_TIM2);  }
void TIM3_IRQHandler(void)                  { handle_pulse_isr(HW_TIMER_TIM3);  }
void TIM4_IRQHandler(void)                  { handle_pulse_isr(HW_TIMER_TIM4);  }
void TIM5_IRQHandler(void)                  { handle_pulse_isr(HW_TIMER_TIM5);  }
void TIM1_BRK_TIM9_IRQHandler(void)         { handle_pulse_isr(HW_TIMER_TIM9);  }
void TIM1_UP_TIM10_IRQHandler(void)         { handle_pulse_isr(HW_TIMER_TIM10); }
void TIM1_TRG_COM_TIM11_IRQHandler(void)    { handle_pulse_isr(HW_TIMER_TIM11); }
