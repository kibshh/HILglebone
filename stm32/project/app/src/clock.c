#include "clock.h"

#include "stm32f4xx.h"

/*
 * Target: SYSCLK = 84 MHz, USB = 48 MHz.
 *
 * PLL math (both HSE and HSI paths):
 *   VCO input  = source / M  = 2 MHz (recommended window)
 *   VCO output = VCO_in * N  = 336 MHz
 *   SYSCLK     = VCO_out / P = 84 MHz
 *   USB        = VCO_out / Q = 48 MHz
 *
 * Source-dependent M so the VCO input is always 2 MHz:
 *   HSE (8 MHz):  M = 4
 *   HSI (16 MHz): M = 8
 */
#define CLOCK_PLL_N             168U
#define CLOCK_PLL_P_DIV         4U      /* register value 1 means /4; see PLLP encoding */
#define CLOCK_PLL_P_REGVAL      1U
#define CLOCK_PLL_Q             7U
#define CLOCK_PLL_M_HSE         4U
#define CLOCK_PLL_M_HSI         8U

/* Flash wait states: 0 WS up to 30 MHz, 1 WS up to 60 MHz, 2 WS up to 90 MHz
 * at 3.3 V (RM0368 §3.5.1). 84 MHz -> 2 WS. */
#define CLOCK_FLASH_LATENCY     FLASH_ACR_LATENCY_2WS

void clock_init(void)
{
    /* Enable the power interface so we can set the voltage scale. */
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;

    /* Scale 2 is sufficient up to 84 MHz (RM0368 §5.4.1). */
    PWR->CR |= PWR_CR_VOS_1;

#ifdef USE_HSE
    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY))
        ;
#endif

    /* Pre-enable the flash accelerator before switching to a faster clock. */
    FLASH->ACR = CLOCK_FLASH_LATENCY
               | FLASH_ACR_ICEN
               | FLASH_ACR_DCEN
               | FLASH_ACR_PRFTEN;

    RCC->PLLCFGR = (CLOCK_PLL_N        << RCC_PLLCFGR_PLLN_Pos)
                 | (CLOCK_PLL_P_REGVAL << RCC_PLLCFGR_PLLP_Pos)
                 | (CLOCK_PLL_Q        << RCC_PLLCFGR_PLLQ_Pos)
#ifdef USE_HSE
                 | (CLOCK_PLL_M_HSE    << RCC_PLLCFGR_PLLM_Pos)
                 | RCC_PLLCFGR_PLLSRC_HSE;
#else
                 | (CLOCK_PLL_M_HSI    << RCC_PLLCFGR_PLLM_Pos)
                 | RCC_PLLCFGR_PLLSRC_HSI;
#endif

    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY))
        ;

    /* APB1 = /2 (max 42 MHz at 84 MHz SYSCLK); APB2 = /1 (default). */
    RCC->CFGR |= RCC_CFGR_PPRE1_DIV2;

    /* Switch to PLL. */
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL)
        ;

    SystemCoreClockUpdate();
}
