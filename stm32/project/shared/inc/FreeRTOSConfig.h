#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/*
 * HILglebone FreeRTOS Configuration
 *
 * Target: STM32F401RE (Cortex-M4F, 84 MHz, 96 KB RAM, 512 KB Flash)
 *
 * Reference: https://www.freertos.org/a00110.html
 */

/* ── Core settings ── */
#define configUSE_PREEMPTION                     1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  1
#define configUSE_TICKLESS_IDLE                  0
#define configCPU_CLOCK_HZ                       ((uint32_t)84000000)
#define configTICK_RATE_HZ                       ((TickType_t)1000)
#define configMAX_PRIORITIES                     8
#define configMINIMAL_STACK_SIZE                 ((uint16_t)128)  /* words = 512 bytes */
#define configMAX_TASK_NAME_LEN                  16
#define configUSE_16_BIT_TICKS                   0
#define configIDLE_SHOULD_YIELD                  1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES    1
#define configENABLE_BACKWARD_COMPATIBILITY      0
#define configMESSAGE_BUFFER_LENGTH_TYPE         size_t

/* ── Memory allocation ── */
#define configSUPPORT_STATIC_ALLOCATION          0
#define configSUPPORT_DYNAMIC_ALLOCATION         1
#define configTOTAL_HEAP_SIZE                    ((size_t)(32 * 1024))  /* 32 KB of 96 KB */
#define configAPPLICATION_ALLOCATED_HEAP         0
#define configHEAP_CLEAR_MEMORY_ON_FREE          1

/* ── Hook functions ── */
#define configUSE_IDLE_HOOK                      0
#define configUSE_TICK_HOOK                      0
#define configUSE_MALLOC_FAILED_HOOK             1
#define configCHECK_FOR_STACK_OVERFLOW           2  /* both pattern and watermark check */

/* ── Kernel features ── */
#define configUSE_MUTEXES                        1
#define configUSE_RECURSIVE_MUTEXES              1
#define configUSE_COUNTING_SEMAPHORES            1
#define configUSE_QUEUE_SETS                     0
#define configQUEUE_REGISTRY_SIZE                8
#define configUSE_TIMERS                         1
#define configTIMER_TASK_PRIORITY                (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH                 10
#define configTIMER_TASK_STACK_DEPTH             (configMINIMAL_STACK_SIZE * 2)

/* ── Co-routines (unused) ── */
#define configUSE_CO_ROUTINES                    0

/* ── Runtime stats (disabled for now, enable later for profiling) ── */
#define configGENERATE_RUN_TIME_STATS            0
#define configUSE_TRACE_FACILITY                 0
#define configUSE_STATS_FORMATTING_FUNCTIONS     0

/* ── INCLUDE API functions ── */
#define INCLUDE_vTaskPrioritySet                 1
#define INCLUDE_uxTaskPriorityGet                1
#define INCLUDE_vTaskDelete                      1
#define INCLUDE_vTaskSuspend                     1
#define INCLUDE_vTaskDelayUntil                  1
#define INCLUDE_vTaskDelay                       1
#define INCLUDE_xTaskGetSchedulerState           1
#define INCLUDE_xTaskGetCurrentTaskHandle        1
#define INCLUDE_uxTaskGetStackHighWaterMark      1

/* ── Cortex-M4 interrupt priority configuration ── */

/*
 * The STM32F4 uses 4 priority bits (16 levels). CMSIS defines these as
 * 0 (highest) to 15 (lowest). FreeRTOS needs the raw register values,
 * which are the priority shifted left into the top bits of a byte.
 *
 * __NVIC_PRIO_BITS is defined by the CMSIS device header (stm32f4xx.h).
 */
#ifdef __NVIC_PRIO_BITS
    #define configPRIO_BITS __NVIC_PRIO_BITS
#else
    #define configPRIO_BITS 4
#endif

/* Lowest interrupt priority (used for PendSV and SysTick). */
#define configKERNEL_INTERRUPT_PRIORITY         (15 << (8 - configPRIO_BITS))

/*
 * Highest ISR priority that may call FreeRTOS API functions.
 * ISRs at priority 0-4 (numerically lower = higher priority) are
 * "above" FreeRTOS and must NOT call any FreeRTOS *FromISR() API.
 * ISRs at priority 5-15 may safely use FreeRTOS API.
 */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    (5 << (8 - configPRIO_BITS))

/* ── Assert ── */
#define configASSERT(x) if ((x) == 0) { taskDISABLE_INTERRUPTS(); for (;;); }

/* ── Map FreeRTOS handler names to the CMSIS vector table names ── */
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

#endif /* FREERTOS_CONFIG_H */
