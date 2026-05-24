#include "digital_out_sensor.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "timers.h"

#include "gpio.h"
#include "hw_timer.h"
#include "helpers.h"
#include "err_codes.h"
#include "protocol.h"
#include "sensor_manager.h"

/* ── Per-sensor state ─────────────────────────────────────────────── */

typedef struct
{
    bool          in_use;

    /* gpio_port is kept separately so we can index pin_owned[] without
     * doing a reverse lookup from the GPIO_TypeDef* in gpio_pin. */
    gpio_port_t   gpio_port;
    gpio_pin_t    gpio_pin;

    uint8_t       timer_kind;
    hw_timer_id_t timer_id;       /* explicit timer (used iff timer_kind=HARDWARE) */

    TimerHandle_t sw_timer;
    uint8_t       pulse_revert_level;
} digital_out_state_t;

static digital_out_state_t states[DIGITAL_OUT_SENSOR_MAX_COUNT];

static uint16_t pin_owned[GPIO_PORT_MAX + 1U];

/* ── Slot helpers ─────────────────────────────────────────────────── */

#define DIGITAL_OUT_INTERNAL_ID_NONE    0xFFU

static uint8_t alloc_slot(void)
{
    for (uint8_t i = 0; i < DIGITAL_OUT_SENSOR_MAX_COUNT; ++i)
    {
        if (!states[i].in_use)
        {
            return i;
        }
    }
    return DIGITAL_OUT_INTERNAL_ID_NONE;
}

/* ── Pin level helper ─────────────────────────────────────────────── */

static void drive_level(gpio_pin_t pin, uint8_t level)
{
    if (level == DIGITAL_OUT_LEVEL_HIGH)
    {
        gpio_set(pin);
    }
    else
    {
        gpio_reset(pin);
    }
}

/* ── Pulse revert callback (shared by SW timer and HW timer ISR) ──── */

static void revert_to_idle_level(uint8_t internal_id)
{
    if (internal_id >= DIGITAL_OUT_SENSOR_MAX_COUNT)
    {
        return;
    }

    digital_out_state_t *s = &states[internal_id];
    if (!s->in_use)
    {
        return;
    }

    drive_level(s->gpio_pin, s->pulse_revert_level);
}

static void sw_timer_callback(TimerHandle_t timer)
{
    uint8_t idx = (uint8_t)(uintptr_t)pvTimerGetTimerID(timer);
    revert_to_idle_level(idx);
}

static void hw_timer_callback(uint8_t internal_id)
{
    revert_to_idle_level(internal_id);
}

/* ── Validation ───────────────────────────────────────────────────── */

static uint8_t validate_cfg(const uint8_t        *cfg,
                            uint16_t              cfg_len,
                            digital_out_state_t  *out_state,
                            gpio_output_config_t *out_gpio_cfg,
                            uint8_t              *out_initial_level)
{
    assert(out_state != NULL);
    assert(out_gpio_cfg != NULL);
    assert(out_initial_level != NULL);
    assert(cfg != NULL || cfg_len == 0U);

    if (cfg_len < DIGITAL_OUT_CFG_SIZE)
    {
        return ERR_MALFORMED_PAYLOAD;
    }

    uint8_t port          = cfg[DIGITAL_OUT_CFG_OFFSET_PORT];
    uint8_t pin           = cfg[DIGITAL_OUT_CFG_OFFSET_PIN];
    uint8_t initial_level = cfg[DIGITAL_OUT_CFG_OFFSET_INITIAL_LEVEL];
    uint8_t output_type   = cfg[DIGITAL_OUT_CFG_OFFSET_OUTPUT_TYPE];
    uint8_t speed         = cfg[DIGITAL_OUT_CFG_OFFSET_SPEED];
    uint8_t pull          = cfg[DIGITAL_OUT_CFG_OFFSET_PULL];
    uint8_t timer_kind    = cfg[DIGITAL_OUT_CFG_OFFSET_TIMER_KIND];
    uint8_t timer_id_u8   = cfg[DIGITAL_OUT_CFG_OFFSET_TIMER_ID];

    if (port > DIGITAL_OUT_PORT_MAX ||
        pin  > DIGITAL_OUT_PIN_MAX  ||
        initial_level > DIGITAL_OUT_LEVEL_HIGH     ||
        output_type   > DIGITAL_OUT_TYPE_MAX       ||
        speed         > DIGITAL_OUT_SPEED_MAX      ||
        pull          > DIGITAL_OUT_PULL_MAX       ||
        timer_kind    > DIGITAL_OUT_TIMER_KIND_MAX ||
        (timer_kind == DIGITAL_OUT_TIMER_HARDWARE && timer_id_u8 >= HW_TIMER_COUNT))
    {
        return ERR_INVALID_PARAMETER;
    }

    if (pin_owned[port] & (1U << pin))
    {
        return ERR_PIN_CONFLICT;
    }

    /* DIGITAL_OUT_PORT_* and gpio_port_t share identical numeric values
     * (both: 0=A, 1=B, 2=C, 3=D, 4=E, 5=H), so a direct cast is safe. */
    gpio_port_t gpio_port = (gpio_port_t)port;
    gpio_pin_t  gpio_pin  = gpio_make_pin(gpio_port, pin);

    out_state->gpio_port  = gpio_port;
    out_state->gpio_pin   = gpio_pin;
    out_state->timer_kind = timer_kind;
    out_state->timer_id   = (hw_timer_id_t)timer_id_u8;

    out_gpio_cfg->pin         = gpio_pin;
    out_gpio_cfg->output_type = (gpio_output_type_t)output_type;
    out_gpio_cfg->speed       = (gpio_speed_t)speed;
    out_gpio_cfg->pull        = (gpio_pull_t)pull;

    *out_initial_level = initial_level;
    return ERR_SUCCESS;
}

/* ── Public API ───────────────────────────────────────────────────── */

void digital_out_sensor_init(void)
{
    for (unsigned i = 0; i < DIGITAL_OUT_SENSOR_MAX_COUNT; ++i)
    {
        memset(&states[i], 0, sizeof(states[i]));
    }
    memset(pin_owned, 0, sizeof(pin_owned));

    /* hw_timer_init() is called from dispatcher_init() before this. */
}

uint8_t digital_out_sensor_setup(const uint8_t *cfg,
                                 uint16_t       cfg_len,
                                 uint8_t       *out_sensor_id)
{
    assert(cfg != NULL || cfg_len == 0U);
    assert(out_sensor_id != NULL);

    *out_sensor_id = PROTO_SENSOR_ID_NONE;

    uint8_t idx = alloc_slot();
    if (idx == DIGITAL_OUT_INTERNAL_ID_NONE)
    {
        return ERR_OUT_OF_RESOURCES;
    }

    digital_out_state_t  *s             = &states[idx];
    gpio_output_config_t  gpio_cfg      = {0};
    uint8_t               initial_level = DIGITAL_OUT_LEVEL_LOW;

    uint8_t err = validate_cfg(cfg, cfg_len, s, &gpio_cfg, &initial_level);
    if (err != ERR_SUCCESS)
    {
        return err;
    }

    if (s->timer_kind == DIGITAL_OUT_TIMER_HARDWARE)
    {
        if (!hw_timer_pulse_acquire(s->timer_id, idx, hw_timer_callback))
        {
            return ERR_OUT_OF_RESOURCES;
        }
    }

    pin_owned[s->gpio_port] |= (1U << s->gpio_pin.pin);

    gpio_enable_clock(s->gpio_port);
    gpio_configure_output(&gpio_cfg, initial_level);

    s->sw_timer           = NULL;
    s->pulse_revert_level = DIGITAL_OUT_LEVEL_LOW;
    s->in_use             = true;

    uint8_t id = sensor_manager_register(PROTO_ID_DIGITAL_OUT, idx);
    if (id == PROTO_SENSOR_ID_NONE)
    {
        gpio_reset_to_defaults(s->gpio_pin);
        if (s->timer_kind == DIGITAL_OUT_TIMER_HARDWARE)
        {
            hw_timer_pulse_release(idx);
        }
        pin_owned[s->gpio_port] &= (uint16_t)~(1U << s->gpio_pin.pin);
        memset(s, 0, sizeof(*s));
        return ERR_OUT_OF_RESOURCES;
    }

    *out_sensor_id = id;
    return ERR_SUCCESS;
}

uint8_t digital_out_sensor_set_output(uint8_t        internal_id,
                                      const uint8_t *values,
                                      uint16_t       values_len)
{
    assert(internal_id < DIGITAL_OUT_SENSOR_MAX_COUNT);
    assert(values != NULL || values_len == 0U);

    digital_out_state_t *s = &states[internal_id];
    if (!s->in_use)
    {
        return ERR_INVALID_SENSOR_ID;
    }

    if (values_len < DIGITAL_OUT_SET_OUTPUT_SIZE)
    {
        return ERR_MALFORMED_PAYLOAD;
    }

    uint8_t  level    = values[DIGITAL_OUT_SET_OUTPUT_OFFSET_LEVEL];
    uint32_t pulse_us = read_u32_le(&values[DIGITAL_OUT_SET_OUTPUT_OFFSET_PULSE_US]);

    if (level > DIGITAL_OUT_LEVEL_HIGH)
    {
        return ERR_INVALID_PARAMETER;
    }

    if (s->timer_kind == DIGITAL_OUT_TIMER_HARDWARE &&
        pulse_us > hw_timer_pulse_max_us(internal_id))
    {
        return ERR_INVALID_PARAMETER;
    }

    if (s->timer_kind == DIGITAL_OUT_TIMER_SOFTWARE && s->sw_timer != NULL)
    {
        (void)xTimerStop(s->sw_timer, 0);
    }

    drive_level(s->gpio_pin, level);

    if (pulse_us == 0U)
    {
        return ERR_SUCCESS;
    }

    s->pulse_revert_level = (level == DIGITAL_OUT_LEVEL_HIGH)
                            ? DIGITAL_OUT_LEVEL_LOW
                            : DIGITAL_OUT_LEVEL_HIGH;

    if (s->timer_kind == DIGITAL_OUT_TIMER_HARDWARE)
    {
        if (!hw_timer_pulse_start(internal_id, pulse_us))
        {
            return ERR_INTERNAL;
        }
        return ERR_SUCCESS;
    }

    TickType_t ticks = pdMS_TO_TICKS((pulse_us + 999U) / 1000U);
    if (ticks == 0U)
    {
        ticks = 1U;
    }

    if (s->sw_timer == NULL)
    {
        s->sw_timer = xTimerCreate("digout_pulse",
                                   ticks,
                                   pdFALSE,
                                   (void *)(uintptr_t)internal_id,
                                   sw_timer_callback);
        if (s->sw_timer == NULL)
        {
            return ERR_OUT_OF_RESOURCES;
        }
    }
    else
    {
        (void)xTimerChangePeriod(s->sw_timer, ticks, 0);
    }

    if (xTimerStart(s->sw_timer, 0) != pdPASS)
    {
        return ERR_INTERNAL;
    }

    return ERR_SUCCESS;
}

uint8_t digital_out_sensor_stop(uint8_t internal_id)
{
    assert(internal_id < DIGITAL_OUT_SENSOR_MAX_COUNT);

    digital_out_state_t *s = &states[internal_id];
    if (!s->in_use)
    {
        return ERR_INVALID_SENSOR_ID;
    }

    if (s->sw_timer != NULL)
    {
        (void)xTimerStop  (s->sw_timer, 0);
        (void)xTimerDelete(s->sw_timer, 0);
        s->sw_timer = NULL;
    }
    if (s->timer_kind == DIGITAL_OUT_TIMER_HARDWARE)
    {
        hw_timer_pulse_release(internal_id);
    }

    gpio_reset_to_defaults(s->gpio_pin);
    pin_owned[s->gpio_port] &= (uint16_t)~(1U << s->gpio_pin.pin);
    memset(s, 0, sizeof(*s));

    return ERR_SUCCESS;
}
