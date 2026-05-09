#include "uart.h"

#include <assert.h>

#include "stm32f4xx.h"
#include "FreeRTOS.h"
#include "task.h"

#include "helpers.h"

/* ── Pin assignments ──────────────────────────────────────────────── */

#define UART_TX_PIN                 9U      /* PA9  */
#define UART_RX_PIN                 10U     /* PA10 */
#define UART_GPIO_AF_USART1         7U      /* AF7  */

/* Compile-time sanity: ring buffers must be powers of two so we can mask. */
#if (UART_RX_BUFFER_SIZE == 0U) || \
    ((UART_RX_BUFFER_SIZE & (UART_RX_BUFFER_SIZE - 1U)) != 0U)
#error "UART_RX_BUFFER_SIZE must be a non-zero power of two"
#endif

#if (UART_TX_BUFFER_SIZE == 0U) || \
    ((UART_TX_BUFFER_SIZE & (UART_TX_BUFFER_SIZE - 1U)) != 0U)
#error "UART_TX_BUFFER_SIZE must be a non-zero power of two"
#endif

#define UART_RX_BUFFER_MASK         (UART_RX_BUFFER_SIZE - 1U)
#define UART_TX_BUFFER_MASK         (UART_TX_BUFFER_SIZE - 1U)

/* ── SPSC ring buffers ────────────────────────────────────────────── */

/* RX: producer = ISR, consumer = task.
 * TX: producer = task, consumer = ISR.
 * Head/tail are volatile + single 32-bit stores, so no lock needed on
 * Cortex-M as long as each side only writes its own index. */
static volatile uint8_t  rx_buf[UART_RX_BUFFER_SIZE];
static volatile uint16_t rx_head;  /* ISR writes */
static volatile uint16_t rx_tail;  /* task reads  */

static volatile uint8_t  tx_buf[UART_TX_BUFFER_SIZE];
static volatile uint16_t tx_head;  /* task writes */
static volatile uint16_t tx_tail;  /* ISR reads   */

static TaskHandle_t rx_notify_task;

/* ── Init ─────────────────────────────────────────────────────────── */

void uart_init(void)
{
    /* Clocks */
    RCC->AHB1ENR  |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB2ENR  |= RCC_APB2ENR_USART1EN;

    /* PA9/PA10 into alternate-function mode, AF7 = USART1 */
    GPIO_SET_MODER(GPIOA, UART_TX_PIN, GPIO_MODER_AF);
    GPIO_SET_MODER(GPIOA, UART_RX_PIN, GPIO_MODER_AF);
    GPIO_SET_AF(GPIOA, UART_TX_PIN, UART_GPIO_AF_USART1);
    GPIO_SET_AF(GPIOA, UART_RX_PIN, UART_GPIO_AF_USART1);

    /* Baud: oversample by 16 (default, OVER8=0). USARTDIV = PCLK2 / baud.
     * Value goes directly into BRR -- the lower 4 bits become the fractional
     * part, the rest the mantissa. Integer division here rounds down, which
     * is <0.05% error at 115200 on 84 MHz. */
    USART1->BRR = UART_PCLK2_HZ / UART_BAUD_RATE;

    /* TE + RE + RXNEIE + UE. TXEIE is enabled lazily in uart_tx_push(). */
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_UE;

    /* NVIC: must be at or below the FreeRTOS syscall priority floor so the
     * ISR can call xTaskNotifyFromISR / portYIELD_FROM_ISR. */
    NVIC_SetPriority(USART1_IRQn, UART_IRQ_PRIORITY);
    NVIC_EnableIRQ(USART1_IRQn);
}

void uart_set_rx_notify_task(TaskHandle_t task)
{
    rx_notify_task = task;
}

/* ── Consumer-side (task context) ─────────────────────────────────── */

bool uart_rx_pop(uint8_t *out)
{
    assert(out != NULL);

    uint16_t tail = rx_tail;

    if (tail == rx_head)
    {
        return false;   /* empty */
    }

    *out = rx_buf[tail];
    rx_tail = (uint16_t)((tail + 1U) & UART_RX_BUFFER_MASK);
    return true;
}

size_t uart_tx_push(const uint8_t *data, size_t len)
{
    assert(data != NULL || len == 0U);

    if (len == 0U)
    {
        return 0;
    }

    size_t pushed = 0;

    for (; pushed < len; ++pushed)
    {
        uint16_t head = tx_head;
        uint16_t next = (uint16_t)((head + 1U) & UART_TX_BUFFER_MASK);

        if (next == tx_tail)
        {
            break;  /* buffer full */
        }

        tx_buf[head] = data[pushed];
        tx_head = next;
    }

    if (pushed > 0U)
    {
        USART1->CR1 |= USART_CR1_TXEIE;
    }

    return pushed;
}

/* ── ISR ──────────────────────────────────────────────────────────── */

void USART1_IRQHandler(void)
{
    BaseType_t higher_prio_woken = pdFALSE;
    uint32_t sr = USART1->SR;

    /* RX: byte received. Reading DR also clears RXNE.
     * NOTE: an overrun (ORE) is cleared by a read of SR followed by DR,
     * so we do that unconditionally if ORE is set -- the byte in DR is the
     * most recent good one. */
    if (sr & USART_SR_RXNE)
    {
        uint8_t  b    = (uint8_t)USART1->DR;
        uint16_t head = rx_head;
        uint16_t next = (uint16_t)((head + 1U) & UART_RX_BUFFER_MASK);

        if (next != rx_tail)
        {
            rx_buf[head] = b;
            rx_head = next;
        }
        /* else: buffer full -> drop. Retransmit is the peer's responsibility
         * when its ACK timeout fires. */

        if (rx_notify_task != NULL)
        {
            vTaskNotifyGiveFromISR(rx_notify_task, &higher_prio_woken);
        }
    }
    else if (sr & USART_SR_ORE)
    {
        /* Clear ORE even if RXNE isn't set (shouldn't normally happen, but
         * belt-and-suspenders). SR was already read above; a DR read finishes
         * the clear sequence. */
        (void)USART1->DR;
    }

    /* TX: TDR empty and we've enabled TXEIE -> drain next byte, or stop. */
    if ((sr & USART_SR_TXE) && (USART1->CR1 & USART_CR1_TXEIE))
    {
        uint16_t tail = tx_tail;

        if (tail == tx_head)
        {
            USART1->CR1 &= ~USART_CR1_TXEIE;    /* nothing left; park the ISR */
        }
        else
        {
            USART1->DR = tx_buf[tail];
            tx_tail = (uint16_t)((tail + 1U) & UART_TX_BUFFER_MASK);
        }
    }

    portYIELD_FROM_ISR(higher_prio_woken);
}
