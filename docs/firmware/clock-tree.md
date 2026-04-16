# STM32F401RE Clock Tree

How HILglebone firmware takes a raw oscillator input and produces every
clock the firmware actually uses. This note covers the PLL math, the
bus-prescaler choices, and the flash/voltage scaling that has to match.
Anyone tuning the clock tree (different target SYSCLK, swapping HSE for
a different crystal, enabling USB, etc.) should read this first.

## Targets

| Clock  | Value   | Why                                                |
|--------|---------|----------------------------------------------------|
| SYSCLK | 84 MHz  | F401 max -- headroom for USART1, SPI, timers       |
| HCLK   | 84 MHz  | AHB = SYSCLK; CPU + DMA + memory bus               |
| PCLK1  | 42 MHz  | APB1 cap is 42 MHz, so we divide by 2              |
| PCLK2  | 84 MHz  | APB2 cap is 84 MHz, no division                    |
| PLL48  | 48 MHz  | USB full-speed clock -- reserved for future use    |

84 MHz is the part's documented ceiling (DS10086 §3.5, RM0368 §3.5.1).
We run at the ceiling because USART1 BRR math, SPI dividers, and timer
resolution all get better with a faster bus, and none of the firmware
is thermal- or power-constrained.

## Input sources

Selected at compile time via the `USE_HSE` CMake option:

| Source | Frequency | Notes                                           |
|--------|-----------|-------------------------------------------------|
| HSE    | 8 MHz     | Crystal, or ST-Link MCO on Nucleo (bypass mode) |
| HSI    | 16 MHz    | Internal RC oscillator; ±1% trim at 25 °C        |

HSE is the default because crystal-driven timing is required for
anything emulating a sensor at a defined frequency (I2C bit rate,
SPI SCK, PWM period, UART baud). HSI is fine for bring-up when the
crystal isn't soldered yet, but don't ship with it.

## PLL math

The F401 PLL has four dividers: M (input prescaler), N (multiplier),
P (system clock divider), Q (USB/SDIO divider). The recommended VCO
input is 2 MHz (RM0368 §6.3.2), and the VCO output must sit between
100 MHz and 432 MHz. Everything below is chosen to land inside both
windows and hit the targets above.

```
source ──► /M ──► VCO_in ──► ×N ──► VCO_out ──► /P ──► SYSCLK
                                         └────► /Q ──► PLL48
```

Plugging in the HILglebone choices:

| Param | Value    | Constraint satisfied                      |
|-------|----------|-------------------------------------------|
| M     | 4 (HSE) / 8 (HSI) | `source / M = 2 MHz` (VCO input sweet spot) |
| N     | 168      | `VCO_in × N = 336 MHz` (inside 100..432)  |
| P     | 4        | `VCO_out / P = 84 MHz` (SYSCLK target)    |
| Q     | 7        | `VCO_out / Q = 48 MHz` (USB spec)         |

Note the source-dependent M: the *rest* of the PLL is identical
whether you boot from HSE or HSI, because M is the only thing that
changes when the input frequency doubles. That's why `clock.c` has
two M constants but just one N/P/Q each.

### P encoding gotcha

`PLLP` in `RCC_PLLCFGR` is a 2-bit field encoding /2, /4, /6, /8 as
register values 0, 1, 2, 3 (RM0368 §6.3.2). We want /4, so the bit
pattern written is `1`, not `4`. In `clock.c` this is called out
explicitly:

```c
#define CLOCK_PLL_P_DIV         4U      /* human value: divide by 4 */
#define CLOCK_PLL_P_REGVAL      1U      /* actual bits written to PLLP */
```

If you change the target SYSCLK, remember to update *both* constants
consistently -- they're separate for readability, not redundancy.

## Bus prescalers

```
SYSCLK = 84 MHz
   │
   ├─ AHB  /1 ─► HCLK  = 84 MHz   (CPU, DMA, Flash, CMSIS SysTick)
   │
   ├─ APB1 /2 ─► PCLK1 = 42 MHz   (USART2, I2C1..3, SPI2/3, TIM2..5, TIM6, TIM7)
   │                                └─► TIMxCLK1 = 84 MHz (see below)
   │
   └─ APB2 /1 ─► PCLK2 = 84 MHz   (USART1, USART6, SPI1, SPI4, ADC1, TIM1, TIM9..11)
                                    └─► TIMxCLK2 = 84 MHz
```

The AHB prescaler is left at reset default (/1). APB2 is also left at
/1. Only APB1 is explicitly programmed, because its reset default is
/1 and 84 MHz would exceed the 42 MHz APB1 cap -- leaving it at reset
would corrupt APB1 peripherals.

### The APB1 timer quirk

When an APB prescaler is > 1, the timer kernel clock on that bus is
**doubled** relative to PCLK (RM0368 §6.2). Concretely: APB1 is /2,
so PCLK1 = 42 MHz, but timers on APB1 (TIM2..5, TIM6, TIM7) still see
84 MHz. This is deliberate -- it means TIMx on APB1 has the same
resolution as TIMx on APB2, so PWM period math doesn't care which bus
the timer lives on.

If you ever drop APB1 to /4 or /8, re-check this: the doubler only
kicks in when the divider is greater than 1, and it's always ×2
regardless of the divider value.

## Flash wait states

At 84 MHz and 3.3 V the flash needs **2 wait states** to keep up
(RM0368 §3.5.1):

| V<sub>DD</sub> | 0 WS | 1 WS  | 2 WS  | 3 WS  |
|--------|------|-------|-------|-------|
| 2.7 -- 3.6 V | ≤ 30 | ≤ 60 | ≤ 90 | n/a |
| 2.4 -- 2.7 V | ≤ 24 | ≤ 48 | ≤ 72 | ≤ 96 |
| (MHz) |

The wait-state constant `FLASH_ACR_LATENCY_2WS` is applied **before**
switching SYSCLK to PLL. The order matters: the CPU can only run
faster than 30 MHz once the flash is ready to feed it instructions at
that rate. Inverting the order (switch clock first, then add WS)
fault-triggers instantly.

Alongside the wait states we also enable:

| Bit      | Purpose                                                   |
|----------|-----------------------------------------------------------|
| ICEN     | Instruction cache -- lets the prefetch buffer actually win |
| DCEN     | Data cache -- same, for loads from flash                   |
| PRFTEN   | Prefetch -- preload next flash line while current one runs |

All three are cost-free to leave on and give a significant IPC boost
at 84 MHz. Disable them only when debugging flash-bus-timing weirdness.

## Voltage scaling

The PWR regulator voltage scale caps the CPU frequency (RM0368 §5.4.1):

| VOS  | Max SYSCLK |
|------|------------|
| 3 (reset default) | 64 MHz |
| 2    | 84 MHz     |
| 1    | not available on F401 (F407+ only) |

We need 84 MHz, so `clock_init()` bumps `PWR->CR` to VOS scale 2 right
after enabling the PWR peripheral clock. Like the flash WS, this has
to happen before the PLL comes online, otherwise the CPU brownouts on
the first fast fetch.

## Bring-up sequence

`clock_init()` in [`stm32/src/app/clock.c`](../../stm32/src/app/clock.c)
walks the steps in this exact order; reordering is unsafe.

```
1. Enable PWR peripheral clock           (APB1ENR |= PWREN)
2. Raise voltage scale to VOS 2          (PWR->CR |= VOS_1)
3. (HSE path only) start HSE, wait HSERDY
4. Configure flash accelerator           (2 WS + ICEN + DCEN + PRFTEN)
5. Write RCC->PLLCFGR                    (M, N, P, Q, source)
6. Enable PLL, wait PLLRDY
7. Set APB1 divider to /2
8. Switch SYSCLK to PLL, wait until RCC_CFGR_SWS reads PLL
9. SystemCoreClockUpdate()               (keep CMSIS cache in sync)
```

Step 8's wait-on-SWS is the important one: the hardware clocks over
on its own schedule (next edge of both old and new clocks must line
up), so until `SWS` reports PLL, the CPU is still on HSI/HSE.
`SystemCoreClockUpdate()` at the end is a CMSIS helper that reads
RCC back and updates the global `SystemCoreClock` variable; any
library that derives timing from it (including SysTick drivers) will
be wrong until this is called.

## Changing the target

The main knobs, ordered by how much else has to change when you turn
them:

- **Different SYSCLK (still inside PLL range)**: update `CLOCK_PLL_P_DIV`
  and `CLOCK_PLL_P_REGVAL`, check flash WS table, check APB1 prescaler
  (may be able to drop to /1 if SYSCLK ≤ 42 MHz).
- **Different HSE frequency**: update `CLOCK_PLL_M_HSE` so the VCO
  input stays at 2 MHz. N/P/Q unchanged.
- **Enable USB**: the 48 MHz PLL48 output is already generated -- just
  enable the USB peripheral clock and OTGFS module; no change here.
- **SYSCLK > 84 MHz**: not supported by this part. Swap to F411
  (100 MHz) or F446 (180 MHz) and re-derive everything; VOS table
  also shifts on those parts.

## References

- RM0368 (F401 reference manual) §6 -- RCC, PLL, bus prescalers
- RM0368 §3.5.1 -- flash latency table
- RM0368 §5.4.1 -- PWR voltage scaling
- DS10086 (F401RE datasheet) §3.5 -- absolute max SYSCLK
