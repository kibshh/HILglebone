#include "gpio.h"

#include <assert.h>

#include "helpers.h"

/* ── Port resolution ──────────────────────────────────────────────── */

GPIO_TypeDef *gpio_port_handle(gpio_port_t port)
{
    switch (port)
    {
    case GPIO_PORT_A: return GPIOA;
    case GPIO_PORT_B: return GPIOB;
    case GPIO_PORT_C: return GPIOC;
    case GPIO_PORT_D: return GPIOD;
    case GPIO_PORT_E: return GPIOE;
    case GPIO_PORT_H: return GPIOH;
    default:          return NULL;
    }
}

/* ── Clock gating ─────────────────────────────────────────────────── */

void gpio_enable_clock(gpio_port_t port)
{
    switch (port)
    {
    case GPIO_PORT_A: RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN; break;
    case GPIO_PORT_B: RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN; break;
    case GPIO_PORT_C: RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN; break;
    case GPIO_PORT_D: RCC->AHB1ENR |= RCC_AHB1ENR_GPIODEN; break;
    case GPIO_PORT_E: RCC->AHB1ENR |= RCC_AHB1ENR_GPIOEEN; break;
    case GPIO_PORT_H: RCC->AHB1ENR |= RCC_AHB1ENR_GPIOHEN; break;
    default: break;
    }
}

/* ── Pin construction ─────────────────────────────────────────────── */

gpio_pin_t gpio_make_pin(gpio_port_t port, uint8_t pin)
{
    assert(port <= GPIO_PORT_MAX);
    assert(pin  <= GPIO_PIN_MAX);

    gpio_pin_t p = {
        .port = gpio_port_handle(port),
        .pin  = pin,
    };
    return p;
}

/* ── Configuration ────────────────────────────────────────────────── */

void gpio_configure_output(const gpio_output_config_t *cfg,
                           uint8_t                     initial_level)
{
    assert(cfg != NULL);
    assert(cfg->pin.port != NULL);

    GPIO_TypeDef *gpio = cfg->pin.port;
    uint8_t       pin  = cfg->pin.pin;

    /* Drive the desired level before switching MODER so the pin never
     * briefly outputs the wrong state as it leaves Hi-Z. */
    if (initial_level)
    {
        GPIO_SET_PIN(gpio, pin);
    }
    else
    {
        GPIO_RESET_PIN(gpio, pin);
    }

    if (cfg->output_type == GPIO_OUTPUT_OPEN_DRAIN)
    {
        GPIO_SET_OTYPE_OD(gpio, pin);
    }
    else
    {
        GPIO_SET_OTYPE_PP(gpio, pin);
    }

    GPIO_SET_SPEED(gpio, pin, (uint32_t)cfg->speed);
    GPIO_SET_PULL (gpio, pin, (uint32_t)cfg->pull);
    GPIO_SET_MODER(gpio, pin, GPIO_MODER_OUTPUT);
}

void gpio_configure_input(const gpio_input_config_t *cfg)
{
    assert(cfg != NULL);
    assert(cfg->pin.port != NULL);

    GPIO_SET_MODER(cfg->pin.port, cfg->pin.pin, GPIO_MODER_INPUT);
    GPIO_SET_PULL (cfg->pin.port, cfg->pin.pin, (uint32_t)cfg->pull);
}

void gpio_configure_af(const gpio_af_config_t *cfg)
{
    assert(cfg != NULL);
    assert(cfg->pin.port != NULL);
    assert(cfg->af <= GPIO_AF_MAX);

    GPIO_TypeDef *gpio = cfg->pin.port;
    uint8_t       pin  = cfg->pin.pin;

    GPIO_SET_AF   (gpio, pin, cfg->af);
    GPIO_SET_SPEED(gpio, pin, (uint32_t)cfg->speed);
    GPIO_SET_PULL (gpio, pin, (uint32_t)cfg->pull);
    if (cfg->output_type == GPIO_OUTPUT_OPEN_DRAIN)
        GPIO_SET_OTYPE_OD(gpio, pin);
    else
        GPIO_SET_OTYPE_PP(gpio, pin);
    GPIO_SET_MODER(gpio, pin, GPIO_MODER_AF);
}

void gpio_reset_to_defaults(gpio_pin_t pin)
{
    assert(pin.port != NULL);

    GPIO_SET_MODER(pin.port, pin.pin, GPIO_MODER_INPUT);
    GPIO_SET_PULL (pin.port, pin.pin, GPIO_PUPDR_NONE);
    GPIO_SET_OTYPE_PP(pin.port, pin.pin);
}

/* ── Level control ────────────────────────────────────────────────── */

void gpio_set(gpio_pin_t pin)
{
    assert(pin.port != NULL);
    GPIO_SET_PIN(pin.port, pin.pin);
}

void gpio_reset(gpio_pin_t pin)
{
    assert(pin.port != NULL);
    GPIO_RESET_PIN(pin.port, pin.pin);
}

void gpio_toggle(gpio_pin_t pin)
{
    assert(pin.port != NULL);
    GPIO_TOGGLE_PIN(pin.port, pin.pin);
}
