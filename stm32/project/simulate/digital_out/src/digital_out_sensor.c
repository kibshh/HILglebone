#include "digital_out_sensor.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "stm32f4xx.h"
#include "FreeRTOS.h"
#include "timers.h"

#include "digital_out_pulse_timer.h"
#include "helpers.h"
#include "protocol.h"
#include "sensor_manager.h"

/* ── Per-sensor state ─────────────────────────────────────────────── */

typedef struct
{
    bool        in_use;

    /* Wire-level config, kept verbatim so we can revert correctly on STOP. */
    uint8_t       port;
    uint8_t       pin;
    uint8_t       output_type;
    uint8_t       speed;
    uint8_t       pull;

    /* Pulse-timing strategy chosen at setup; pinned for the sensor's
     * lifetime. Determines whether sw_timer or hw_slot is in play. */
    uint8_t       timer_kind;     /* DIGITAL_OUT_TIMER_SOFTWARE / _HARDWARE */
    uint8_t       pulse_range;    /* DIGITAL_OUT_PULSE_RANGE_SHORT / _LONG (used iff HW) */

    /* Software-timer handle for one-shot pulses. Created lazily on the
     * first pulse request and reused for subsequent pulses on the same
     * sensor; deleted in stop(). Only used when timer_kind = SOFTWARE. */
    TimerHandle_t sw_timer;

    /* Level the pulse expires back to. Captured when the pulse is armed
     * so we don't have to re-derive it in the timer callback (and so a
     * second SET_OUTPUT during the pulse window can change it). */
    uint8_t       pulse_revert_level;
} digital_out_state_t;

/* Static pool. No heap allocation for the bookkeeping; the FreeRTOS
 * timer object below is the only dynamic resource per sensor. */
static digital_out_state_t states[DIGITAL_OUT_SENSOR_MAX_COUNT];

/* Pin reservation bitmask: bit `pin` of `pin_owned[port]` is set when
 * that (port, pin) is currently claimed by a digital_out sensor. Lets
 * us reject ERR_PIN_CONFLICT without scanning all slots. */
static uint16_t pin_owned[DIGITAL_OUT_PORT_MAX + 1U];

/* ── Port helpers ─────────────────────────────────────────────────── */

#define DIGITAL_OUT_INTERNAL_ID_NONE    0xFFU

/* Map port enum -> CMSIS GPIO_TypeDef*. Returns NULL for unknown values. */
static GPIO_TypeDef *port_to_gpio(uint8_t port)
{
    switch (port)
    {
    case DIGITAL_OUT_PORT_A: return GPIOA;
    case DIGITAL_OUT_PORT_B: return GPIOB;
    case DIGITAL_OUT_PORT_C: return GPIOC;
    case DIGITAL_OUT_PORT_D: return GPIOD;
    case DIGITAL_OUT_PORT_E: return GPIOE;
    case DIGITAL_OUT_PORT_H: return GPIOH;
    default: return NULL;
    }
}

/* Enable the AHB1 clock for the given port. RCC bits are sparse on
 * F401RE -- A..E are bits 0..4, H is bit 7. */
static void port_enable_clock(uint8_t port)
{
    switch (port)
    {
    case DIGITAL_OUT_PORT_A: RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN; break;
    case DIGITAL_OUT_PORT_B: RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN; break;
    case DIGITAL_OUT_PORT_C: RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN; break;
    case DIGITAL_OUT_PORT_D: RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN; break;
    case DIGITAL_OUT_PORT_E: RCC->AHB1ENR |= RCC_AHB1ENR_GPIOEEN; break;
    case DIGITAL_OUT_PORT_H: RCC->AHB1ENR |= RCC_AHB1ENR_GPIOHEN; break;
    default: break;
    }
}

/* ── Slot helpers ─────────────────────────────────────────────────── */

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

/* Drive the pin to `level`. Uses BSRR (atomic) so this is safe from any
 * context including timer callbacks. */
static void drive_pin(const digital_out_state_t *s, uint8_t level)
{
    GPIO_TypeDef *gpio = port_to_gpio(s->port);
    assert(gpio != NULL);

    if (level == DIGITAL_OUT_LEVEL_HIGH)
    {
        GPIO_SET_PIN(gpio, s->pin);
    }
    else
    {
        GPIO_RESET_PIN(gpio, s->pin);
    }
}

/* Common revert routine called from both the SW-timer task callback
 * and the HW-timer ISR. Idempotent and safe to invoke after stop: an
 * `!in_use` slot means a stop() raced with a pending fire, so just bail. */
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

    drive_pin(s, s->pulse_revert_level);
}

/* FreeRTOS software-timer callback. The slot's internal_id is stashed
 * in the timer's pvTimerID so we can locate state without a global lookup. */
static void sw_timer_callback(TimerHandle_t timer)
{
    uint8_t idx = (uint8_t)(uintptr_t)pvTimerGetTimerID(timer);
    revert_to_idle_level(idx);
}

/* HW-timer ISR callback. Runs in interrupt context; only safe ops are
 * BSRR writes (drive_pin uses BSRR). No FreeRTOS APIs called here. */
static void hw_timer_callback(uint8_t internal_id)
{
    revert_to_idle_level(internal_id);
}

/* ── Validation ───────────────────────────────────────────────────── */

static uint8_t validate_cfg(const uint8_t       *cfg,
                            uint16_t             cfg_len,
                            digital_out_state_t *out,
                            uint8_t             *out_initial_level)
{
    assert(out != NULL);
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
    uint8_t pulse_range   = cfg[DIGITAL_OUT_CFG_OFFSET_PULSE_RANGE];

    if (port > DIGITAL_OUT_PORT_MAX ||
        pin  > DIGITAL_OUT_PIN_MAX  ||
        initial_level > DIGITAL_OUT_LEVEL_HIGH      ||
        output_type   > DIGITAL_OUT_TYPE_MAX        ||
        speed         > DIGITAL_OUT_SPEED_MAX       ||
        pull          > DIGITAL_OUT_PULL_MAX        ||
        timer_kind    > DIGITAL_OUT_TIMER_KIND_MAX  ||
        pulse_range   > DIGITAL_OUT_PULSE_RANGE_MAX)
    {
        return ERR_INVALID_PARAMETER;
    }

    if (pin_owned[port] & (1U << pin))
    {
        return ERR_PIN_CONFLICT;
    }

    out->port               = port;
    out->pin                = pin;
    out->output_type        = output_type;
    out->speed              = speed;
    out->pull               = pull;
    out->timer_kind         = timer_kind;
    out->pulse_range        = pulse_range;
    *out_initial_level      = initial_level;

    return ERR_SUCCESS;
}

/* ── Hardware bring-up ────────────────────────────────────────────── */

static void apply_pin_config(const digital_out_state_t *s, uint8_t initial_level)
{
    GPIO_TypeDef *gpio = port_to_gpio(s->port);
    assert(gpio != NULL);

    /* Set the initial level *before* switching MODER to output, so the
     * pin doesn't briefly drive the wrong way as it leaves Hi-Z. */
    if (initial_level == DIGITAL_OUT_LEVEL_HIGH)
    {
        GPIO_SET_PIN(gpio, s->pin);
    }
    else
    {
        GPIO_RESET_PIN(gpio, s->pin);
    }

    if (s->output_type == DIGITAL_OUT_TYPE_OPEN_DRAIN)
    {
        GPIO_SET_OTYPE_OD(gpio, s->pin);
    }
    else
    {
        GPIO_SET_OTYPE_PP(gpio, s->pin);
    }

    GPIO_SET_SPEED(gpio, s->pin, s->speed);
    GPIO_SET_PULL (gpio, s->pin, s->pull);
    GPIO_SET_MODER(gpio, s->pin, GPIO_MODER_OUTPUT);
}

/* Revert pin to the safest cold state: floating input, no pulls,
 * push-pull (the default after reset). */
static void revert_pin(const digital_out_state_t *s)
{
    GPIO_TypeDef *gpio = port_to_gpio(s->port);
    assert(gpio != NULL);

    GPIO_SET_MODER(gpio, s->pin, GPIO_MODER_INPUT);
    GPIO_SET_PULL (gpio, s->pin, GPIO_PUPDR_NONE);
    GPIO_SET_OTYPE_PP(gpio, s->pin);
}

/* ── Public API ───────────────────────────────────────────────────── */

void digital_out_sensor_init(void)
{
    for (unsigned i = 0; i < DIGITAL_OUT_SENSOR_MAX_COUNT; ++i)
    {
        memset(&states[i], 0, sizeof(states[i]));
    }
    memset(pin_owned, 0, sizeof(pin_owned));

    pulse_timer_hw_init();
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

    digital_out_state_t *s = &states[idx];
    uint8_t initial_level  = DIGITAL_OUT_LEVEL_LOW;

    uint8_t err = validate_cfg(cfg, cfg_len, s, &initial_level);
    if (err != ERR_SUCCESS)
    {
        return err;
    }

    /* If hardware-timer mode was requested, claim a slot from the
     * caller-selected sub-pool BEFORE we touch any pin state. Failing
     * here is the cheapest abort path -- nothing has been mutated yet. */
    if (s->timer_kind == DIGITAL_OUT_TIMER_HARDWARE)
    {
        pulse_timer_hw_kind_t hw_kind =
            (s->pulse_range == DIGITAL_OUT_PULSE_RANGE_LONG)
            ? PULSE_TIMER_HW_KIND_LONG
            : PULSE_TIMER_HW_KIND_SHORT;

        if (!pulse_timer_hw_acquire(idx, hw_kind, hw_timer_callback))
        {
            return ERR_OUT_OF_RESOURCES;
        }
    }

    /* Reserve the pin and bring up the hardware before publishing the
     * sensor to the manager, so a failure here doesn't leave a dangling
     * external id pointing at half-initialised state. */
    pin_owned[s->port] |= (1U << s->pin);

    port_enable_clock(s->port);
    apply_pin_config(s, initial_level);

    s->sw_timer           = NULL;
    s->pulse_revert_level = DIGITAL_OUT_LEVEL_LOW;
    s->in_use             = true;

    uint8_t id = sensor_manager_register(PROTO_ID_DIGITAL_OUT, idx);
    if (id == PROTO_SENSOR_ID_NONE)
    {
        /* Roll back: revert the pin, release any HW timer, free pin reservation. */
        revert_pin(s);
        if (s->timer_kind == DIGITAL_OUT_TIMER_HARDWARE)
        {
            pulse_timer_hw_release(idx);
        }
        pin_owned[s->port] &= (uint16_t)~(1U << s->pin);
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

    /* HW timers are capped per-slot (16-bit slots: 65 535 µs;
     * 32-bit slots: ~71 minutes). Validate against the actual slot we
     * were given so we don't drive the pin and then have to undo it. */
    if (s->timer_kind == DIGITAL_OUT_TIMER_HARDWARE &&
        pulse_us > pulse_timer_hw_get_max_pulse_us(internal_id))
    {
        return ERR_INVALID_PARAMETER;
    }

    /* Cancel any in-flight pulse before applying the new request. */
    if (s->timer_kind == DIGITAL_OUT_TIMER_SOFTWARE)
    {
        if (s->sw_timer != NULL)
        {
            (void)xTimerStop(s->sw_timer, 0);
        }
    }
    /* For HW mode, pulse_timer_hw_start() already cancels and reprograms;
     * if pulse_us is 0 we leave the slot armed-but-stopped. */

    drive_pin(s, level);

    if (pulse_us == 0U)
    {
        return ERR_SUCCESS;
    }

    s->pulse_revert_level = (level == DIGITAL_OUT_LEVEL_HIGH)
                            ? DIGITAL_OUT_LEVEL_LOW
                            : DIGITAL_OUT_LEVEL_HIGH;

    if (s->timer_kind == DIGITAL_OUT_TIMER_HARDWARE)
    {
        if (!pulse_timer_hw_start(internal_id, pulse_us))
        {
            return ERR_INTERNAL;
        }
        return ERR_SUCCESS;
    }

    /* Software path: schedule the revert. Round up to the next
     * FreeRTOS tick so a sub-tick request still produces an observable
     * pulse. */
    TickType_t ticks = pdMS_TO_TICKS((pulse_us + 999U) / 1000U);
    if (ticks == 0U)
    {
        ticks = 1U;
    }

    if (s->sw_timer == NULL)
    {
        s->sw_timer = xTimerCreate("digout_pulse",
                                   ticks,
                                   pdFALSE,                          /* one-shot */
                                   (void *)(uintptr_t)internal_id,   /* slot index */
                                   sw_timer_callback);
        if (s->sw_timer == NULL)
        {
            /* The pin is at the requested level; we just can't auto-
             * revert. Tell the caller -- they can retry or fall back to
             * sending a second SET_OUTPUT manually. */
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

    /* Tear down whichever pulse mechanism this sensor was using. The
     * other pointer is always NULL/NONE, so checking both is harmless. */
    if (s->sw_timer != NULL)
    {
        (void)xTimerStop  (s->sw_timer, 0);
        (void)xTimerDelete(s->sw_timer, 0);
        s->sw_timer = NULL;
    }
    if (s->timer_kind == DIGITAL_OUT_TIMER_HARDWARE)
    {
        pulse_timer_hw_release(internal_id);
    }

    revert_pin(s);
    pin_owned[s->port] &= (uint16_t)~(1U << s->pin);
    memset(s, 0, sizeof(*s));

    return ERR_SUCCESS;
}
