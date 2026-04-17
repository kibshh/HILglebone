#include "app_init.h"

#include "stm32f4xx.h"

#include "FreeRTOS.h"
#include "task.h"

#include "clock.h"
#include "dispatcher.h"
#include "helpers.h"
#include "protocol_parser.h"
#include "uart.h"

/* ── Tunables ─────────────────────────────────────────────────────── */

/* Heartbeat LED: PA5 on Nucleo-F401RE (LD2). */
#define HEARTBEAT_LED_PIN           5U
#define HEARTBEAT_PERIOD_MS         500U

/* Heartbeat is cosmetic; keep it at the lowest priority. */
#define TASK_HEARTBEAT_PRIORITY     1U
#define TASK_HEARTBEAT_STACK_WORDS  configMINIMAL_STACK_SIZE

/* Protocol task handles wire I/O; runs above heartbeat so incoming bytes
 * don't get starved, but below the FreeRTOS timer service (priority 7). */
#define TASK_PROTOCOL_PRIORITY      3U
#define TASK_PROTOCOL_STACK_WORDS   (configMINIMAL_STACK_SIZE * 3)

/* ── Heartbeat task ───────────────────────────────────────────────── */

static void heartbeat_led_init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    GPIO_SET_MODER(GPIOA, HEARTBEAT_LED_PIN, GPIO_MODER_OUTPUT);
}

static void task_heartbeat(void *params)
{
    (void)params;

    for (;;)
    {
        GPIO_TOGGLE_PIN(GPIOA, HEARTBEAT_LED_PIN);
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
    }
}

/* ── Protocol task ────────────────────────────────────────────────── */

static void task_protocol(void *params)
{
    (void)params;

    /* Register ourselves with the UART driver so the RX ISR notifies us
     * when bytes land, instead of polling. Must happen before we block on
     * the notification, obviously. */
    uart_set_rx_notify_task(xTaskGetCurrentTaskHandle());

    static protocol_parser_t parser;

    protocol_parser_reset(&parser);

    for (;;)
    {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        uint8_t b;
        while (uart_rx_pop(&b))
        {
            if (protocol_parser_feed(&parser, b))
            {
                dispatcher_handle(&parser.frame);
            }
        }
    }
}

/* ── Public API ───────────────────────────────────────────────────── */

void app_init(void)
{
    /* Core + clocks. SystemInit came from the CMSIS startup before main, so
     * here we only raise the system clock to 84 MHz. */
    clock_init();

    /* On-board peripherals. */
    heartbeat_led_init();
    uart_init();

    /* Protocol layer: sensor manager + backends. No hardware actions; just
     * zeroes out in-memory tables. */
    dispatcher_init();

    /* Tasks. Protocol task is created first so it exists when heartbeat
     * (lower priority) starts, though FreeRTOS doesn't care about order. */
    (void)xTaskCreate(task_protocol,
                      "protocol",
                      TASK_PROTOCOL_STACK_WORDS,
                      NULL,
                      TASK_PROTOCOL_PRIORITY,
                      NULL);

    (void)xTaskCreate(task_heartbeat,
                      "heartbeat",
                      TASK_HEARTBEAT_STACK_WORDS,
                      NULL,
                      TASK_HEARTBEAT_PRIORITY,
                      NULL);
}
