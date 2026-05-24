#include "pwm_sensor.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "gpio.h"
#include "helpers.h"
#include "hw_timer.h"
#include "err_codes.h"
#include "protocol.h"
#include "sensor_manager.h"

/* ── Per-sensor state ─────────────────────────────────────────────── */

typedef struct
{
    bool          in_use;
    gpio_port_t   gpio_port;
    gpio_pin_t    gpio_pin;
    hw_timer_id_t timer_id;
    uint8_t       channel;
} pwm_sensor_state_t;

/* Slot layout: timer_id * HW_TIMER_MAX_CHANNELS + (channel - 1). */
static pwm_sensor_state_t states[PWM_SLOT_POOL_SIZE];

static uint16_t pin_owned[GPIO_PORT_MAX + 1U];

/* ── Slot helpers ─────────────────────────────────────────────────── */

/* Each (timer, channel) pair maps deterministically to a unique slot. */
static uint8_t slot_for(hw_timer_id_t timer_id, uint8_t channel)
{
    return (uint8_t)((uint8_t)timer_id * HW_TIMER_MAX_CHANNELS
                     + (channel - 1U));
}

/* ── CCR helpers ──────────────────────────────────────────────────── */

static uint32_t compute_ccr(uint32_t arr, uint16_t duty)
{
    if (duty == 0U)           return 0U;
    if (duty >= PWM_DUTY_MAX) return arr;
    return ((arr + 1U) * duty) / PWM_DUTY_MAX;
}

static void channel_configure(TIM_TypeDef *tim, uint8_t ch, uint32_t ccr)
{
    assert(tim != NULL);
    switch (ch)
    {
    case 1:
        tim->CCMR1 = (tim->CCMR1 & ~(TIM_CCMR1_OC1M | TIM_CCMR1_OC1PE))
                   | TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1PE;
        tim->CCR1   = ccr;
        tim->CCER  |= TIM_CCER_CC1E;
        break;
    case 2:
        tim->CCMR1 = (tim->CCMR1 & ~(TIM_CCMR1_OC2M | TIM_CCMR1_OC2PE))
                   | TIM_CCMR1_OC2M_1 | TIM_CCMR1_OC2M_2 | TIM_CCMR1_OC2PE;
        tim->CCR2   = ccr;
        tim->CCER  |= TIM_CCER_CC2E;
        break;
    case 3:
        tim->CCMR2 = (tim->CCMR2 & ~(TIM_CCMR2_OC3M | TIM_CCMR2_OC3PE))
                   | TIM_CCMR2_OC3M_1 | TIM_CCMR2_OC3M_2 | TIM_CCMR2_OC3PE;
        tim->CCR3   = ccr;
        tim->CCER  |= TIM_CCER_CC3E;
        break;
    case 4:
        tim->CCMR2 = (tim->CCMR2 & ~(TIM_CCMR2_OC4M | TIM_CCMR2_OC4PE))
                   | TIM_CCMR2_OC4M_1 | TIM_CCMR2_OC4M_2 | TIM_CCMR2_OC4PE;
        tim->CCR4   = ccr;
        tim->CCER  |= TIM_CCER_CC4E;
        break;
    default: break;
    }
}

static void channel_set_ccr(TIM_TypeDef *tim, uint8_t ch, uint32_t ccr)
{
    assert(tim != NULL);
    switch (ch)
    {
    case 1: tim->CCR1 = ccr; break;
    case 2: tim->CCR2 = ccr; break;
    case 3: tim->CCR3 = ccr; break;
    case 4: tim->CCR4 = ccr; break;
    default: break;
    }
}

static void channel_disable(TIM_TypeDef *tim, uint8_t ch)
{
    assert(tim != NULL);
    switch (ch)
    {
    case 1:
        tim->CCER  &= ~TIM_CCER_CC1E;
        tim->CCMR1 &= ~(TIM_CCMR1_OC1M | TIM_CCMR1_OC1PE);
        tim->CCR1   = 0U;
        break;
    case 2:
        tim->CCER  &= ~TIM_CCER_CC2E;
        tim->CCMR1 &= ~(TIM_CCMR1_OC2M | TIM_CCMR1_OC2PE);
        tim->CCR2   = 0U;
        break;
    case 3:
        tim->CCER  &= ~TIM_CCER_CC3E;
        tim->CCMR2 &= ~(TIM_CCMR2_OC3M | TIM_CCMR2_OC3PE);
        tim->CCR3   = 0U;
        break;
    case 4:
        tim->CCER  &= ~TIM_CCER_CC4E;
        tim->CCMR2 &= ~(TIM_CCMR2_OC4M | TIM_CCMR2_OC4PE);
        tim->CCR4   = 0U;
        break;
    default: break;
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

void pwm_sensor_init(void)
{
    for (unsigned i = 0; i < PWM_SLOT_POOL_SIZE; ++i)
    {
        memset(&states[i], 0, sizeof(states[i]));
    }
    memset(pin_owned, 0, sizeof(pin_owned));
}

err_code_t pwm_sensor_setup(const uint8_t *cfg,
                             uint16_t       cfg_len,
                             uint8_t       *out_sensor_id)
{
    assert(cfg != NULL || cfg_len == 0U);
    assert(out_sensor_id != NULL);

    *out_sensor_id = PROTO_SENSOR_ID_NONE;

    if (cfg_len < PWM_CFG_SIZE)
    {
        return ERR_MALFORMED_PAYLOAD;
    }

    uint8_t  port          = cfg[PWM_CFG_OFFSET_PORT];
    uint8_t  pin           = cfg[PWM_CFG_OFFSET_PIN];
    uint8_t  af            = cfg[PWM_CFG_OFFSET_AF];
    uint8_t  timer_raw     = cfg[PWM_CFG_OFFSET_TIMER];
    uint8_t  channel       = cfg[PWM_CFG_OFFSET_CHANNEL];
    uint32_t freq_hz       = read_u32_le(&cfg[PWM_CFG_OFFSET_FREQ_HZ]);
    uint16_t duty          = read_u16_le(&cfg[PWM_CFG_OFFSET_DUTY_PCT_X100]);

    if (port      >  (uint8_t)GPIO_PORT_MAX   ||
        pin       >  GPIO_PIN_MAX             ||
        af        == 0U || af > GPIO_AF_MAX   ||
        timer_raw <  PWM_TIMER_MIN_VAL        ||
        timer_raw >  PWM_TIMER_MAX_VAL        ||
        channel   <  PWM_CHANNEL_MIN          ||
        channel   >  PWM_CHANNEL_MAX          ||
        duty      >  PWM_DUTY_MAX             ||
        freq_hz   <  PWM_FREQ_MIN_HZ          ||
        freq_hz   >  PWM_FREQ_MAX_HZ)
    {
        return ERR_INVALID_PARAMETER;
    }

    hw_timer_id_t timer_id = (hw_timer_id_t)timer_raw;

    /* Validate channel against this timer's actual channel count. */
    if (channel > hw_timer_max_channel(timer_id))
    {
        return ERR_INVALID_PARAMETER;
    }

    if (pin_owned[port] & (1U << pin))
    {
        return ERR_PIN_CONFLICT;
    }

    uint8_t idx = slot_for(timer_id, channel);
    if (states[idx].in_use)
    {
        return ERR_PWM_CHANNEL_IN_USE;
    }

    /* Acquire the timer (or join an existing PWM session at same freq). */
    uint32_t arr;
    err_code_t err = hw_timer_pwm_acquire(timer_id, freq_hz, &arr);
    if (err != ERR_SUCCESS)
    {
        return err;
    }

    /* Configure GPIO in AF mode. */
    gpio_port_t  gpio_port = (gpio_port_t)port;
    gpio_pin_t   gpio_pin  = gpio_make_pin(gpio_port, pin);

    gpio_enable_clock(gpio_port);

    gpio_af_config_t af_cfg = {
        .pin   = gpio_pin,
        .af    = af,
        .speed = GPIO_SPEED_HIGH,
        .pull  = GPIO_PULL_NONE,
    };
    gpio_configure_af(&af_cfg);

    pin_owned[port] |= (1U << pin);

    /* Configure the timer channel for PWM. */
    TIM_TypeDef *tim = hw_timer_handle(timer_id);
    assert(tim != NULL);

    uint32_t ccr = compute_ccr(arr, duty);
    channel_configure(tim, channel, ccr);

    /* Register with the sensor manager. */
    pwm_sensor_state_t *s = &states[idx];
    s->gpio_port = gpio_port;
    s->gpio_pin  = gpio_pin;
    s->timer_id  = timer_id;
    s->channel   = channel;
    s->in_use    = true;

    uint8_t id = sensor_manager_register(PROTO_ID_PWM, idx);
    if (id == PROTO_SENSOR_ID_NONE)
    {
        channel_disable(tim, channel);
        hw_timer_pwm_release(timer_id);
        gpio_reset_to_defaults(gpio_pin);
        pin_owned[port] &= (uint16_t)~(1U << pin);
        memset(s, 0, sizeof(*s));
        return ERR_OUT_OF_RESOURCES;
    }

    *out_sensor_id = id;
    return ERR_SUCCESS;
}

err_code_t pwm_sensor_set_output(uint8_t        internal_id,
                                  const uint8_t *values,
                                  uint16_t       values_len)
{
    assert(internal_id < PWM_SLOT_POOL_SIZE);
    assert(values != NULL || values_len == 0U);

    pwm_sensor_state_t *s = &states[internal_id];
    if (!s->in_use)
    {
        return ERR_INVALID_SENSOR_ID;
    }

    if (values_len < PWM_SET_OUTPUT_SIZE)
    {
        return ERR_MALFORMED_PAYLOAD;
    }

    uint16_t duty = read_u16_le(&values[PWM_SET_OUTPUT_OFFSET_DUTY]);
    if (duty > PWM_DUTY_MAX)
    {
        return ERR_INVALID_PARAMETER;
    }

    TIM_TypeDef *tim = hw_timer_handle(s->timer_id);
    assert(tim != NULL);

    uint32_t arr = hw_timer_pwm_arr(s->timer_id);
    uint32_t ccr = compute_ccr(arr, duty);
    channel_set_ccr(tim, s->channel, ccr);

    return ERR_SUCCESS;
}

err_code_t pwm_sensor_stop(uint8_t internal_id)
{
    assert(internal_id < PWM_SLOT_POOL_SIZE);

    pwm_sensor_state_t *s = &states[internal_id];
    if (!s->in_use)
    {
        return ERR_INVALID_SENSOR_ID;
    }

    TIM_TypeDef *tim = hw_timer_handle(s->timer_id);
    assert(tim != NULL);

    channel_disable(tim, s->channel);
    hw_timer_pwm_release(s->timer_id);

    gpio_reset_to_defaults(s->gpio_pin);
    pin_owned[(uint8_t)s->gpio_port] &= (uint16_t)~(1U << s->gpio_pin.pin);

    memset(s, 0, sizeof(*s));
    return ERR_SUCCESS;
}
