#ifndef HELPERS_H
#define HELPERS_H

#include <assert.h>
#include <stdint.h>

/* ── Little-endian readers ────────────────────────────────────────── */

static inline uint16_t read_u16_le(const uint8_t *data)
{
    assert(data != NULL);

    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static inline uint32_t read_u32_le(const uint8_t *data)
{
    assert(data != NULL);

    return (uint32_t)data[0]
         | ((uint32_t)data[1] << 8)
         | ((uint32_t)data[2] << 16)
         | ((uint32_t)data[3] << 24);
}

/* ── GPIO register helpers ────────────────────────────────────────── */

/* MODER: 2 bits per pin.
 *   00 = input   01 = output   10 = alternate function   11 = analog */
#define GPIO_MODER_INPUT        0x0U
#define GPIO_MODER_OUTPUT       0x1U
#define GPIO_MODER_AF           0x2U
#define GPIO_MODER_ANALOG       0x3U

#define GPIO_MODER_MASK(pin)            (0x3U  << ((pin) * 2U))
#define GPIO_MODER_VAL(pin, mode)       ((uint32_t)(mode) << ((pin) * 2U))

#define GPIO_SET_MODER(gpio, pin, mode)                  \
    do {                                                 \
        (gpio)->MODER &= ~GPIO_MODER_MASK(pin);          \
        (gpio)->MODER |=  GPIO_MODER_VAL(pin, (mode));   \
    } while (0)

/* OSPEEDR: 2 bits per pin.
 *   00 = low   01 = medium   10 = high   11 = very high */
#define GPIO_SPEED_LOW          0x0U
#define GPIO_SPEED_MEDIUM       0x1U
#define GPIO_SPEED_HIGH         0x2U
#define GPIO_SPEED_VERY_HIGH    0x3U

#define GPIO_OSPEEDR_MASK(pin)          (0x3U  << ((pin) * 2U))
#define GPIO_OSPEEDR_VAL(pin, spd)      ((uint32_t)(spd) << ((pin) * 2U))

#define GPIO_SET_SPEED(gpio, pin, spd)                       \
    do {                                                     \
        (gpio)->OSPEEDR &= ~GPIO_OSPEEDR_MASK(pin);          \
        (gpio)->OSPEEDR |=  GPIO_OSPEEDR_VAL(pin, (spd));    \
    } while (0)

/* PUPDR: 2 bits per pin.
 *   00 = none   01 = pull-up   10 = pull-down */
#define GPIO_PUPDR_NONE         0x0U
#define GPIO_PUPDR_PULLUP       0x1U
#define GPIO_PUPDR_PULLDOWN     0x2U

#define GPIO_PUPDR_MASK(pin)            (0x3U  << ((pin) * 2U))
#define GPIO_PUPDR_VAL(pin, pu)         ((uint32_t)(pu) << ((pin) * 2U))

#define GPIO_SET_PULL(gpio, pin, pu)                     \
    do {                                                 \
        (gpio)->PUPDR &= ~GPIO_PUPDR_MASK(pin);          \
        (gpio)->PUPDR |=  GPIO_PUPDR_VAL(pin, (pu));     \
    } while (0)

/* OTYPER: 1 bit per pin.   0 = push-pull   1 = open-drain */
#define GPIO_SET_OTYPE_PP(gpio, pin)    ((gpio)->OTYPER &= ~(1U << (pin)))
#define GPIO_SET_OTYPE_OD(gpio, pin)    ((gpio)->OTYPER |=  (1U << (pin)))

/* AFR: 4 bits per pin; AFR[0] covers pins 0-7, AFR[1] covers pins 8-15. */
#define GPIO_AFR_IDX(pin)               ((pin) >> 3U)
#define GPIO_AFR_POS(pin)               (((pin) & 0x7U) * 4U)
#define GPIO_AFR_MASK(pin)              (0xFU  << GPIO_AFR_POS(pin))
#define GPIO_AFR_VAL(pin, af)           ((uint32_t)(af) << GPIO_AFR_POS(pin))

#define GPIO_SET_AF(gpio, pin, af)                                      \
    do {                                                                \
        (gpio)->AFR[GPIO_AFR_IDX(pin)] &= ~GPIO_AFR_MASK(pin);         \
        (gpio)->AFR[GPIO_AFR_IDX(pin)] |=  GPIO_AFR_VAL(pin, (af));    \
    } while (0)

/* Pin output control.
 * SET/RESET use BSRR -- atomic, safe to call from any context.
 * TOGGLE uses ODR -- not atomic; only use when a single context owns the pin. */
#define GPIO_SET_PIN(gpio, pin)         ((gpio)->BSRR =  (1U << (pin)))
#define GPIO_RESET_PIN(gpio, pin)       ((gpio)->BSRR =  (1U << ((pin) + 16U)))
#define GPIO_TOGGLE_PIN(gpio, pin)      ((gpio)->ODR  ^= (1U << (pin)))

#endif /* HELPERS_H */
