/**
 * HILglebone STM32 Real-Time Engine
 *
 * Entry point for the signal emulation firmware.
 * Initializes clocks and peripherals, then starts the FreeRTOS scheduler.
 */

#include "stm32f4xx.h"
#include "FreeRTOS.h"
#include "task.h"

static void SystemClock_Config(void);

/* ── Task: heartbeat LED on PA5 (Nucleo onboard LED) ── */

static void task_heartbeat(void *params)
{
    (void)params;

    for (;;)
    {
        GPIOA->ODR ^= GPIO_ODR_OD5;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ── Entry point ── */

int main(void)
{
    /* Reset all peripherals, init flash interface */
    SystemInit();

    /* Configure system clock: PLL -> 84 MHz */
    SystemClock_Config();

    /* PA5 as push-pull output (heartbeat LED) */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    GPIOA->MODER &= ~GPIO_MODER_MODER5;
    GPIOA->MODER |= GPIO_MODER_MODER5_0;

    /* TODO: Initialize USART1 (PA9/PA10) for BBB communication */
    /* TODO: Initialize I2C, SPI, PWM, DAC peripherals */

    xTaskCreate(
        task_heartbeat,
        "heartbeat",
        configMINIMAL_STACK_SIZE,
        NULL,
        1,       /* low priority */
        NULL
    );

    /* Start the scheduler — this never returns */
    vTaskStartScheduler();

    /* We only get here if there is not enough heap for the idle task */
    for (;;)
        ;
}

/* ── Clock configuration ── */

/**
 * Configure system clock to 84 MHz using PLL.
 *
 * Clock source selected at compile time via USE_HSE (CMake option):
 *   USE_HSE=ON  (default): HSE (8 MHz) / M=4  -> 2 MHz VCO input
 *   USE_HSE=OFF:           HSI (16 MHz) / M=8  -> 2 MHz VCO input
 *
 * Both paths: VCO input * N=168 = 336 MHz, / P=4 = 84 MHz SYSCLK
 * APB1 = 42 MHz (div 2), APB2 = 84 MHz (div 1)
 */
static void SystemClock_Config(void)
{
    /* Enable power interface clock */
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;

    /* Set voltage scaling to Scale 2 (for up to 84 MHz) */
    PWR->CR |= PWR_CR_VOS_1;

#ifdef USE_HSE
    /* Enable HSE and wait for it to stabilize */
    RCC->CR |= RCC_CR_HSEON;
    /* Wait for HSE to be ready */
    while (!(RCC->CR & RCC_CR_HSERDY))
        ;
#endif

    /* Configure flash latency: 2 wait states for 84 MHz at 3.3V */
    /* Use 0 wait states up to 30 MHz, 1 up to 60 MHz, and 2 up to 90 MHz */
    FLASH->ACR = FLASH_ACR_LATENCY_2WS
               | FLASH_ACR_ICEN
               | FLASH_ACR_DCEN
               | FLASH_ACR_PRFTEN;

    /* PLL configuration
    *
    * Goal:
    *   SYSCLK = 84 MHz
    *   USB    = 48 MHz
    *
    * PLL math:
    *   VCO input  = source / M = 2 MHz (recommended)
    *   VCO output = VCO in * N = 336 MHz
    *   SYSCLK     = VCO out / P = 84 MHz
    *   USB clock  = VCO out / Q = 48 MHz
    *
    * Source-dependent configuration:
    *   HSE (8 MHz):  M = 4  → 8 / 4  = 2 MHz
    *   HSI (16 MHz): M = 8  → 16 / 8 = 2 MHz
    */
    RCC->PLLCFGR = (168u << RCC_PLLCFGR_PLLN_Pos)
                |  (1u   << RCC_PLLCFGR_PLLP_Pos)
                |  (7u   << RCC_PLLCFGR_PLLQ_Pos)
#ifdef USE_HSE
                |  (4u   << RCC_PLLCFGR_PLLM_Pos)
                |  RCC_PLLCFGR_PLLSRC_HSE;
#else
                |  (8u   << RCC_PLLCFGR_PLLM_Pos)
                |  RCC_PLLCFGR_PLLSRC_HSI;
#endif

    /* Enable PLL */
    RCC->CR |= RCC_CR_PLLON;
    /* Wait for PLL to be locked */
    while (!(RCC->CR & RCC_CR_PLLRDY))
        ;

    /* Set APB1 prescaler to /2 (max 42 MHz), APB2 = /1 */
    RCC->CFGR |= RCC_CFGR_PPRE1_DIV2;

    /* Select PLL as system clock source */
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    /* Wait for PLL to be selected as system clock */
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL)
        ;

    /* Update SystemCoreClock variable */
    SystemCoreClockUpdate();
}

/* ── FreeRTOS required hooks ── */

void vApplicationMallocFailedHook(void)
{
    __disable_irq();
    for (;;)
        ;
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;
    __disable_irq();
    for (;;)
        ;
}
