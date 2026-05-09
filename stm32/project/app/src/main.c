/**
 * HILglebone STM32 Real-Time Engine -- entry point.
 *
 * Responsibilities here are deliberately tiny:
 *   - Delegate all hardware/task bring-up to `app_init()`
 *   - Start the FreeRTOS scheduler
 *
 * Everything else -- clocks, UART, protocol parser/dispatcher, sensor
 * backends, tasks -- lives in dedicated modules under src/app/, src/drivers/,
 * src/protocol/, src/command/. That lets this file stay stable even as the
 * project grows.
 */

#include "stm32f4xx.h"

#include "FreeRTOS.h"
#include "task.h"

#include "app_init.h"

int main(void)
{
    app_init();

    /* Never returns unless heap is too small for the idle/timer task. */
    vTaskStartScheduler();

    /* Heap exhaustion on startup. Fall through to halt; the FreeRTOS hook
     * below also handles runtime malloc failures the same way. */
    for (;;)
        ;
}

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
