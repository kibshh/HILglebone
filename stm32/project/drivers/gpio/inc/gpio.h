/**
 * Generic GPIO driver.
 *
 * Provides port-to-handle resolution, clock gating, pin configuration,
 * and level control via a thin, struct-based API.  Designed to be shared
 * across all sensor-simulation backends so they don't each duplicate the
 * same port↔GPIO_TypeDef mapping and register-manipulation logic.
 *
 * All functions are safe to call from task context.  gpio_set / gpio_reset
 * use BSRR (atomic single-cycle write) and are also safe from ISR context.
 */

#ifndef GPIO_H
#define GPIO_H

#include <stdbool.h>
#include <stdint.h>

#include "stm32f4xx.h"

/* ── Port enum ────────────────────────────────────────────────────── */

/* GPIO ports present on the STM32F401RE.  F and G do not exist on this
 * package; they are intentionally omitted.  GPIO_PORT_MAX is the last
 * valid value and can be used for range checks. */
typedef enum
{
    GPIO_PORT_A = 0,
    GPIO_PORT_B,
    GPIO_PORT_C,
    GPIO_PORT_D,
    GPIO_PORT_E,
    GPIO_PORT_H,

    GPIO_PORT_MAX = GPIO_PORT_H,
} gpio_port_t;

#define GPIO_PIN_MAX    15U

/* ── Pin descriptor ───────────────────────────────────────────────── */

typedef struct
{
    GPIO_TypeDef *port;     /* resolved peripheral pointer */
    uint8_t       pin;      /* 0..15 */
} gpio_pin_t;

/* ── Configuration ────────────────────────────────────────────────── */

/* Output type matches OTYPER register encoding. */
typedef enum
{
    GPIO_OUTPUT_PUSH_PULL  = 0,
    GPIO_OUTPUT_OPEN_DRAIN = 1,
} gpio_output_type_t;

/* Slew rate / drive strength matches OSPEEDR encoding. */
typedef enum
{
    GPIO_SPEED_LOW       = 0,
    GPIO_SPEED_MEDIUM    = 1,
    GPIO_SPEED_HIGH      = 2,
    GPIO_SPEED_VERY_HIGH = 3,
} gpio_speed_t;

/* Pull configuration matches PUPDR encoding. */
typedef enum
{
    GPIO_PULL_NONE = 0,
    GPIO_PULL_UP   = 1,
    GPIO_PULL_DOWN = 2,
} gpio_pull_t;

/* Configuration for gpio_configure_output(). */
typedef struct
{
    gpio_pin_t         pin;
    gpio_output_type_t output_type;
    gpio_speed_t       speed;
    gpio_pull_t        pull;
} gpio_output_config_t;

/* Configuration for gpio_configure_input(). */
typedef struct
{
    gpio_pin_t  pin;
    gpio_pull_t pull;
} gpio_input_config_t;

/* ── API ──────────────────────────────────────────────────────────── */

/* Resolve a gpio_port_t enum to the CMSIS peripheral pointer.
 * Returns NULL for values outside GPIO_PORT_A..GPIO_PORT_MAX. */
GPIO_TypeDef *gpio_port_handle(gpio_port_t port);

/* Enable the AHB1 peripheral clock for `port`.  Idempotent.
 * Must be called before any register on that port is touched. */
void gpio_enable_clock(gpio_port_t port);

/* Build a gpio_pin_t from a port enum and pin number.  Does not enable
 * the clock or touch any registers; pure struct construction. */
gpio_pin_t gpio_make_pin(gpio_port_t port, uint8_t pin);

/* Configure a pin as a digital output.  The initial level is driven
 * BEFORE the MODER bits are switched to output, avoiding a glitch. */
void gpio_configure_output(const gpio_output_config_t *cfg,
                           uint8_t                     initial_level);

/* Configure a pin as a digital input with the chosen pull. */
void gpio_configure_input(const gpio_input_config_t *cfg);

/* Reset a pin to power-on-reset state: floating input, no pull,
 * push-pull OTYPER (all MODER/OTYPER/PUPDR fields back to 0).
 * Use this when releasing a pin (e.g. on sensor stop). */
void gpio_reset_to_defaults(gpio_pin_t pin);

/* Drive the pin high (BSRR set-bit). Atomic, ISR-safe. */
void gpio_set(gpio_pin_t pin);

/* Drive the pin low (BSRR reset-bit). Atomic, ISR-safe. */
void gpio_reset(gpio_pin_t pin);

/* Toggle the pin via ODR XOR.  Not atomic -- only call from a single
 * context that owns this pin (e.g. the heartbeat task). */
void gpio_toggle(gpio_pin_t pin);

#endif /* GPIO_H */
