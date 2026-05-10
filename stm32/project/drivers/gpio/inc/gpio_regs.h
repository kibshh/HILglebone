/**
 * GPIO register-level macros.
 *
 * Direct read-modify-write helpers for the STM32F4 GPIO peripheral
 * registers.  These are the building blocks used internally by gpio.c
 * and by drivers that initialise their own pins without going through
 * the full gpio_*_config_t API (e.g. uart.c, app_init.c).
 *
 * Prefer the typed functions in gpio.h for new code; reach for these
 * macros only when struct construction overhead is unreasonable
 * (typically during one-shot init sequences).
 */

#ifndef GPIO_REGS_H
#define GPIO_REGS_H

#include <stdint.h>

#include "stm32f4xx.h"

/* ── MODER (mode register) ────────────────────────────────────────── */
/* 2 bits per pin:  00=input  01=output  10=alternate-function  11=analog */

#define GPIO_MODER_INPUT        0x0U
#define GPIO_MODER_OUTPUT       0x1U
#define GPIO_MODER_AF           0x2U
#define GPIO_MODER_ANALOG       0x3U

#define GPIO_MODER_MASK(pin)        (0x3U  << ((pin) * 2U))
#define GPIO_MODER_VAL(pin, mode)   ((uint32_t)(mode) << ((pin) * 2U))

#define GPIO_SET_MODER(gpio, pin, mode)                 \
    do {                                                \
        (gpio)->MODER &= ~GPIO_MODER_MASK(pin);         \
        (gpio)->MODER |=  GPIO_MODER_VAL(pin, (mode));  \
    } while (0)

/* ── OSPEEDR (output speed register) ─────────────────────────────── */
/* 2 bits per pin:  00=low  01=medium  10=high  11=very-high */

#define GPIO_OSPEEDR_MASK(pin)      (0x3U  << ((pin) * 2U))
#define GPIO_OSPEEDR_VAL(pin, spd)  ((uint32_t)(spd) << ((pin) * 2U))

#define GPIO_SET_SPEED(gpio, pin, spd)                      \
    do {                                                    \
        (gpio)->OSPEEDR &= ~GPIO_OSPEEDR_MASK(pin);         \
        (gpio)->OSPEEDR |=  GPIO_OSPEEDR_VAL(pin, (spd));   \
    } while (0)

/* ── PUPDR (pull-up / pull-down register) ─────────────────────────── */
/* 2 bits per pin:  00=none  01=pull-up  10=pull-down */

#define GPIO_PUPDR_NONE         0x0U
#define GPIO_PUPDR_PULLUP       0x1U
#define GPIO_PUPDR_PULLDOWN     0x2U

#define GPIO_PUPDR_MASK(pin)        (0x3U  << ((pin) * 2U))
#define GPIO_PUPDR_VAL(pin, pu)     ((uint32_t)(pu) << ((pin) * 2U))

#define GPIO_SET_PULL(gpio, pin, pu)                    \
    do {                                                \
        (gpio)->PUPDR &= ~GPIO_PUPDR_MASK(pin);         \
        (gpio)->PUPDR |=  GPIO_PUPDR_VAL(pin, (pu));    \
    } while (0)

/* ── OTYPER (output type register) ───────────────────────────────── */
/* 1 bit per pin:  0=push-pull  1=open-drain */

#define GPIO_SET_OTYPE_PP(gpio, pin)    ((gpio)->OTYPER &= ~(1U << (pin)))
#define GPIO_SET_OTYPE_OD(gpio, pin)    ((gpio)->OTYPER |=  (1U << (pin)))

/* ── AFR (alternate function registers) ──────────────────────────── */
/* 4 bits per pin; AFR[0] covers pins 0..7, AFR[1] covers pins 8..15 */

#define GPIO_AFR_IDX(pin)           ((pin) >> 3U)
#define GPIO_AFR_POS(pin)           (((pin) & 0x7U) * 4U)
#define GPIO_AFR_MASK(pin)          (0xFU  << GPIO_AFR_POS(pin))
#define GPIO_AFR_VAL(pin, af)       ((uint32_t)(af) << GPIO_AFR_POS(pin))

#define GPIO_SET_AF(gpio, pin, af)                                     \
    do {                                                               \
        (gpio)->AFR[GPIO_AFR_IDX(pin)] &= ~GPIO_AFR_MASK(pin);        \
        (gpio)->AFR[GPIO_AFR_IDX(pin)] |=  GPIO_AFR_VAL(pin, (af));   \
    } while (0)

/* ── BSRR / ODR (pin output control) ─────────────────────────────── */
/* SET/RESET use BSRR -- atomic, safe from any context including ISRs.
 * TOGGLE uses ODR XOR -- not atomic; only call when one context owns the pin. */

#define GPIO_SET_PIN(gpio, pin)     ((gpio)->BSRR =  (1U << (pin)))
#define GPIO_RESET_PIN(gpio, pin)   ((gpio)->BSRR =  (1U << ((pin) + 16U)))
#define GPIO_TOGGLE_PIN(gpio, pin)  ((gpio)->ODR  ^= (1U << (pin)))

#endif /* GPIO_REGS_H */
