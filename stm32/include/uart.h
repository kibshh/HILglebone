/**
 * USART1 driver -- IRQ-driven, SPSC ring buffers, FreeRTOS-aware.
 *
 * Pinout (STM32F401RE / Nucleo F401RE):
 *   PA9  = USART1_TX  (AF7)
 *   PA10 = USART1_RX  (AF7)
 *
 * Design:
 *   - RX ring buffer: filled by ISR (producer), drained by task (consumer).
 *     When a byte arrives the ISR also task-notifies the receiver task so
 *     it can wake without polling.
 *   - TX ring buffer: filled by task (producer), drained by ISR (consumer).
 *     `uart_tx_push()` kicks the TXE interrupt so the ISR starts draining.
 *   - Priority is set below `configMAX_SYSCALL_INTERRUPT_PRIORITY` so the
 *     ISR can safely call `*FromISR()` FreeRTOS API.
 */

#ifndef UART_H
#define UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#define UART_BAUD_RATE              115200U

/* USART1 is on APB2. After `clock.c` brings SYSCLK to 84 MHz with APB2=/1,
 * PCLK2 = 84 MHz. We pin this here rather than reading it at runtime so
 * the BRR calculation is a compile-time constant. */
#define UART_PCLK2_HZ               84000000U

/* Ring-buffer sizes. Must be powers of two -- the driver uses mask, not
 * modulo, for index wrap, which also makes head/tail updates single-store
 * (SPSC lock-free on Cortex-M). Largest frame is ~263 B, so 512 comfortably
 * holds a full frame plus slack. */
#define UART_RX_BUFFER_SIZE         512U
#define UART_TX_BUFFER_SIZE         512U

/* NVIC priority for USART1_IRQn. Must be numerically >=
 * configMAX_SYSCALL_INTERRUPT_PRIORITY (5) so FromISR calls are legal. */
#define UART_IRQ_PRIORITY           6U

/* Configure GPIO, clock, USART1, and enable the RXNE interrupt.
 * Call after the system clock is up but before starting the scheduler. */
void uart_init(void);

/* Register the task to wake on RX-byte arrival. Pass NULL to disable.
 * Typically called from the protocol task itself right after creation. */
void uart_set_rx_notify_task(TaskHandle_t task);

/* Non-blocking single-byte dequeue. Returns true on success and writes
 * the byte through `out`; false if the RX buffer is empty. */
bool uart_rx_pop(uint8_t *out);

/* Non-blocking bulk enqueue into TX buffer. Returns the number of bytes
 * actually queued (may be < len if the buffer fills up). Starts draining
 * via TXE interrupt automatically. */
size_t uart_tx_push(const uint8_t *data, size_t len);

#endif /* UART_H */
