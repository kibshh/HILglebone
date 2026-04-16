/**
 * STM32F401 system clock setup.
 *
 * Brings SYSCLK to 84 MHz via PLL and fixes AHB/APB prescalers to:
 *   HCLK  = 84 MHz, APB1 = 42 MHz, APB2 = 84 MHz
 * Clock source is selected at compile time by the USE_HSE CMake option.
 */

#ifndef CLOCK_H
#define CLOCK_H

#define SYSTEM_CLOCK_HZ             84000000U
#define APB1_CLOCK_HZ               42000000U
#define APB2_CLOCK_HZ               84000000U

void clock_init(void);

#endif /* CLOCK_H */
