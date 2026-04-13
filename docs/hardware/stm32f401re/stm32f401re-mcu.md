# STMicroelectronics STM32F401RE -- MCU Reference

The STM32F401RE is the real-time signal emulator in the HILglebone platform.
It runs FreeRTOS-based firmware that receives scenario commands over UART from the
BeagleBone orchestrator and emulates various sensors and interfaces accordingly.

---

## Core specifications

| Property | Value |
|----------|-------|
| Core | ARM Cortex-M4F (ARMv7E-M, Thumb-2) |
| FPU | Single-precision (FPv4-SP-D16) |
| Max clock | 84 MHz |
| Flash | 512 KB (at 0x0800_0000) |
| SRAM | 96 KB (at 0x2000_0000) |
| Package | LQFP-64 |
| Operating voltage | 1.7V -- 3.6V (typically 3.3V) |
| I/O voltage | 3.3V (5V tolerant on most pins -- check datasheet) |

### Clock tree (as configured by HILglebone firmware)

```
HSE (8 MHz — crystal or ST-Link MCO on Nucleo) or HSI (16 MHz — internal RC oscillator)
  |
  v
PLL: /M=4 (HSE) or /M=8 (HSI) × N=168 /P=4 (VCO in = 2 MHz, VCO out = 336 MHz, SYSCLK = 84 MHz)
  |
  v
SYSCLK = 84 MHz
  |
  +---> AHB prescaler /1  --> HCLK = 84 MHz (CPU, DMA, memory bus)
  |
  +---> APB1 prescaler /2  --> PCLK1 = 42 MHz (USART2, TIM2-5, I2C, SPI2/3)
  |
  +---> APB2 prescaler /1  --> PCLK2 = 84 MHz (USART1, USART6, TIM1, SPI1, ADC)
```

This configuration is set in `SystemClock_Config()` in `stm32/src/main.c`.
HSE can be configured to be used instead of HSI because crystal-based timing is required for
accurate timer/PWM frequencies and SPI/I2C clock rates when simulating sensors.
On the Nucleo-F401RE, HSE is driven by the ST-Link MCO output (8 MHz bypass).
Use HSI if HSE not available.

---

## Memory map

| Start | End | Size | Description |
|-------|-----|------|-------------|
| 0x0000_0000 | 0x0007_FFFF | 512 KB | Alias of Flash (boot mapping) |
| 0x0800_0000 | 0x0807_FFFF | 512 KB | Flash memory |
| 0x1FFF_0000 | 0x1FFF_7A0F | ~31 KB | System memory (ST bootloader ROM) |
| 0x1FFF_7A10 | 0x1FFF_7A1F | 16 B | OTP area |
| 0x1FFF_C000 | 0x1FFF_C007 | 8 B | Option bytes |
| 0x2000_0000 | 0x2001_7FFF | 96 KB | SRAM |
| 0x4000_0000 | 0x4000_7FFF | -- | APB1 peripherals (USART2, TIM2-5, I2C, SPI2/3) |
| 0x4001_0000 | 0x4001_57FF | -- | APB2 peripherals (USART1, USART6, SPI1, ADC, TIM1) |
| 0x4002_0000 | 0x4007_FFFF | -- | AHB1 peripherals (GPIO, DMA, RCC, Flash interface) |
| 0xE000_0000 | 0xE00F_FFFF | -- | Cortex-M4 internal (NVIC, SysTick, FPU, debug) |

### Flash memory (used by linker script)

The linker script (`stm32/STM32_FLASH.ld`) receives `FLASH_SIZE` and `RAM_SIZE`
as `--defsym` symbols from CMake, making it generic across STM32F4 variants:

| Symbol | STM32F401RE value | Source |
|--------|-------------------|--------|
| FLASH_ORIGIN | 0x08000000 | Fixed for all STM32 |
| FLASH_SIZE | 0x80000 (512 KB) | Injected by CMake |
| RAM_ORIGIN | 0x20000000 | Fixed for all STM32 |
| RAM_SIZE | 0x18000 (96 KB) | Injected by CMake |

---

## Peripherals used by HILglebone

### USART1 -- communication with BeagleBone

| Property | Value |
|----------|-------|
| Base address | 0x4001_1000 |
| Bus | APB2 (PCLK2 = 84 MHz) |
| TX pin | PA9 (AF7) |
| RX pin | PA10 (AF7) |

USART1 carries the binary protocol between the STM32 and the BeagleBone
orchestrator. The protocol is defined in `protocol/protocol-spec.md`.
USART2 (PA2/PA3) is reserved for the ST-Link virtual COM port (debug console).

### I2C -- sensor emulation

| Property | I2C1 | I2C2 | I2C3 |
|----------|------|------|------|
| Base address | 0x4000_5400 | 0x4000_5800 | 0x4000_5C00 |
| Bus | APB1 (42 MHz) | APB1 (42 MHz) | APB1 (42 MHz) |
| SCL pin | PB6 (AF4) or PB8 (AF4) | PB10 (AF4) | PA8 (AF4) |
| SDA pin | PB7 (AF4) or PB9 (AF4) | PB3 (AF9) | PB4 (AF9) |
| Speed modes | Standard (100 kHz), Fast (400 kHz) | Same | Same |

Used to emulate I2C sensor devices (e.g., temperature sensors, accelerometers,
EEPROMs). The STM32 can act as an I2C slave, responding to the DUT's master
requests with scenario-defined data.

### SPI -- sensor emulation

| Property | SPI1 | SPI2 | SPI3 |
|----------|------|------|------|
| Base address | 0x4001_3000 | 0x4000_3800 | 0x4000_3C00 |
| Bus | APB2 (84 MHz) | APB1 (42 MHz) | APB1 (42 MHz) |
| SCK pin | PA5 (AF5) or PB3 (AF5) | PB13 (AF5) | PB3 (AF6) or PC10 (AF6) |
| MISO pin | PA6 (AF5) or PB4 (AF5) | PB14 (AF5) | PB4 (AF6) or PC11 (AF6) |
| MOSI pin | PA7 (AF5) or PB5 (AF5) | PB15 (AF5) | PB5 (AF6) or PC12 (AF6) |
| Max clock | 42 MHz | 21 MHz | 21 MHz |

Used to emulate SPI sensor devices. The STM32 acts as an SPI slave,
providing scenario-defined register data in response to DUT read commands.

Note: SPI1 SCK shares PA5 with the onboard LED (LD2). If SPI1 is used,
the heartbeat LED must be moved to a different pin.

### GPIO -- digital signals, LED heartbeat, DUT control

| Pin | Function | Description |
|-----|----------|-------------|
| PA5 | GPIO output | On-board user LED (LD2). Heartbeat toggle |

Additional GPIO pins can be used for:
- DUT reset control
- Digital signal emulation (logic-level outputs simulating sensor signals)
- Interrupt inputs from the DUT
- Chip-select lines for SPI slave emulation

### Timers -- PWM signal generation and timing

| Timer | Bus | Resolution | Channels | Typical use |
|-------|-----|-----------|----------|-------------|
| TIM1 | APB2 (84 MHz) | 16-bit | 4 | Advanced: PWM with dead-time, complementary outputs |
| TIM2 | APB1 (84 MHz*) | 32-bit | 4 | General-purpose: long-period timing, input capture |
| TIM3 | APB1 (84 MHz*) | 16-bit | 4 | General-purpose: PWM signal generation |
| TIM4 | APB1 (84 MHz*) | 16-bit | 4 | General-purpose: encoder interface |
| TIM5 | APB1 (84 MHz*) | 32-bit | 4 | General-purpose: microsecond timestamps |
| TIM9 | APB2 (84 MHz) | 16-bit | 2 | Simple PWM |
| TIM10 | APB2 (84 MHz) | 16-bit | 1 | Simple timeout |
| TIM11 | APB2 (84 MHz) | 16-bit | 1 | Simple timeout |

*APB1 timer clock = PCLK1 x 2 when APB1 prescaler > 1 (which it is: /2),
so timers on APB1 still see 84 MHz.

Timers are used for PWM signal generation (e.g., emulating speed sensors,
frequency-output sensors) and for precise timing of signal transitions.

### ADC -- signal reading

| Property | Value |
|----------|-------|
| ADC1 | 12-bit, up to 16 channels, 2.4 MSPS (with DMA) |
| Channels | PA0--PA7 (ADC_IN0--IN7), PB0--PB1 (IN8--9), PC0--PC5 (IN10--15) |
| Temperature sensor | Internal, ADC_IN18 |
| VREFINT | Internal reference, ADC_IN17 |

Used to read analog responses from the DUT (e.g., feedback voltages,
analog sensor outputs).

---

## Compiler flags (from CMake toolchain)

These flags are set in `stm32/cmake/stm32_target.cmake` for the STM32F401RE:

| Flag | Purpose |
|------|---------|
| `-mcpu=cortex-m4` | Target Cortex-M4 instruction set |
| `-mthumb` | Use Thumb-2 encoding (required for Cortex-M) |
| `-mfpu=fpv4-sp-d16` | Enable hardware single-precision FPU |
| `-mfloat-abi=hard` | Pass floats in FPU registers (not softfp emulation) |
| `-DSTM32F401xE` | CMSIS device header: selects register definitions for this exact chip |
| `-DUSE_HSE` *(default)* | Select external HSE clock source (default configuration; uses crystal or ST-Link MCO instead of internal HSI) |

`USE_HSE` is enabled by default in CMake (`option(USE_HSE ... ON)`), so the firmware uses the external HSE clock unless explicitly disabled.

---

## Official documentation

### Primary references

| Document | Description | Link |
|----------|-------------|------|
| STM32F401xD/xE Datasheet | Pinout, electrical specs, package, peripheral summary | https://www.st.com/resource/en/datasheet/stm32f401re.pdf |
| STM32F401 Reference Manual (RM0368) | Register-level reference for all peripherals | https://www.st.com/resource/en/reference_manual/rm0368-stm32f401xbc-and-stm32f401xde-advanced-armbased-32bit-mcus-stmicroelectronics.pdf |
| Cortex-M4 Programming Manual (PM0214) | Core registers, NVIC, SysTick, FPU, instruction set | https://www.st.com/resource/en/programming_manual/pm0214-stm32-cortexm4-mcus-and-mpus-programming-manual-stmicroelectronics.pdf |
| STM32F4 HAL/LL Driver Manual (UM1725) | HAL and low-level driver API docs | https://www.st.com/resource/en/user_manual/um1725-description-of-stm32f4-hal-and-lowlayer-drivers-stmicroelectronics.pdf |

### Application notes

| Document | Description | Link |
|----------|-------------|------|
| AN4013 | STM32 timer cookbook (PWM, input capture, one-pulse) | https://www.st.com/resource/en/application_note/an4013-stm32-crossseries-timer-overview-stmicroelectronics.pdf |
| AN3155 | USART bootloader protocol (for serial firmware update) | https://www.st.com/resource/en/application_note/an3155-usart-protocol-used-in-the-stm32-bootloader-stmicroelectronics.pdf |
| AN4776 | General-purpose timer cookbook | https://www.st.com/resource/en/application_note/an4776-generalpurpose-timer-cookbook-for-stm32-microcontrollers-stmicroelectronics.pdf |

### CMSIS and firmware

| Resource | Description | Link |
|----------|-------------|------|
| CMSIS-Core | Cortex-M core abstraction headers | https://github.com/ARM-software/CMSIS_5 |
| cmsis_device_f4 | STM32F4 device headers + startup files | https://github.com/STMicroelectronics/cmsis_device_f4 |
| STM32CubeMX | GUI pin/clock configurator (generates init code) | https://www.st.com/en/development-tools/stm32cubemx.html |

### FreeRTOS

| Resource | Description | Link |
|----------|-------------|------|
| FreeRTOS Kernel | Source code (V11.1.0 used in HILglebone) | https://github.com/FreeRTOS/FreeRTOS-Kernel |
| FreeRTOS Reference Manual | API reference for all kernel objects | https://www.freertos.org/Documentation/02-Kernel/04-API-references/01-Task-creation/00-TaskCreate |
| FreeRTOS Developer Docs | Guides on tasks, queues, semaphores, memory management | https://www.freertos.org/Documentation/02-Kernel/02-Kernel-features/00-Tasks-and-co-routines |
| Cortex-M4 Port Notes | FreeRTOS-specific notes for ARM Cortex-M4F | https://www.freertos.org/Documentation/02-Kernel/03-Supported-devices/02-Customization |
| FreeRTOS Configuration | FreeRTOSConfig.h reference (all config options) | https://www.freertos.org/Documentation/02-Kernel/06-Configuring-a-project/01-Customization |

---

## How this relates to HILglebone

```
    STM32F401RE
    +-------------------------------------------+
    |  Cortex-M4F @ 84 MHz                      |
    |  FreeRTOS V11.1.0 firmware                 |
    |                                            |
    |  USART1 (PA9 TX / PA10 RX)                |----> BBB UART1 (protocol link)
    |     binary frames for commands/responses   |
    |                                            |
    |  I2C1/2/3 (slave mode)                     |----> emulated I2C sensors
    |     temperature, accelerometer, EEPROM     |      to DUT under test
    |                                            |
    |  SPI1/2/3 (slave mode)                     |----> emulated SPI sensors
    |     pressure, gyroscope, ADC               |      to DUT under test
    |                                            |
    |  TIMx + GPIOx                              |----> PWM / digital signals
    |     speed sensors, frequency outputs       |      to DUT under test
    |                                            |
    |  ADC1                                      |<---- DUT response signals
    |                                            |
    |  PA5 (LD2)                                 | heartbeat LED
    +-------------------------------------------+
```

The firmware receives commands from the orchestrator, configures peripherals
(I2C, SPI, timers, GPIO, ADC) to emulate various sensors and signal sources,
and reads back DUT responses. It runs under FreeRTOS for task scheduling
with deterministic timing.
