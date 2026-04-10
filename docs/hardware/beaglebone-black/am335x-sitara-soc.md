# Texas Instruments AM335x Sitara SoC

The BeagleBone Black is built around the **AM3358BZCZ100** -- a member of TI's
AM335x Sitara family. This document covers the SoC architecture, the subsystems
HILglebone relies on, and links to the authoritative TI documentation.

---

## Architecture overview

| Property | Value |
|----------|-------|
| Core | ARM Cortex-A8 (ARMv7-A) |
| Max clock | 1 GHz (AM3358; 720 MHz for AM3352) |
| L1 cache | 32 KB I + 32 KB D |
| L2 cache | 256 KB unified |
| NEON / VFP | Yes (VFPv3, NEON SIMD) |
| On-chip RAM | 64 KB (OCMC SRAM at 0x4030_0000) |
| Boot ROM | 176 KB (at 0x4000_0000) |
| PRU-ICSS | 2x 200 MHz 32-bit RISC coprocessors |
| 3D graphics | PowerVR SGX530 |
| Process | 65 nm |

### Memory map (key regions)

Source: AM335x TRM (SPRUH73Q), Tables 2-1, 2-2, 2-3, 2-4.

**L3 interconnect (Table 2-1)**

| Start | End | Size | Description |
|-------|-----|------|-------------|
| 0x4000_0000 | 0x4002_BFFF | 176 KB | Boot ROM |
| 0x4030_0000 | 0x4030_FFFF | 64 KB | On-chip SRAM (OCMC) |
| 0x4740_0000 | 0x4740_7FFF | 32 KB | USB Subsystem |
| 0x4781_0000 | 0x4781_FFFF | 64 KB | MMCHS2 |
| 0x4900_0000 | 0x490F_FFFF | 1 MB | EDMA3 Channel Controller (TPCC) |
| 0x4C00_0000 | 0x4CFF_FFFF | 16 MB | EMIF0 Configuration |
| 0x5000_0000 | 0x50FF_FFFF | 16 MB | GPMC Configuration |
| 0x5600_0000 | 0x56FF_FFFF | 16 MB | SGX530 (GPU) |
| 0x8000_0000 | 0xBFFF_FFFF | 1 GB | EMIF0 SDRAM (512 MB populated on BBB) |

**L4_WKUP peripherals (Table 2-2)**

| Start | End | Size | Description |
|-------|-----|------|-------------|
| 0x44E0_0000 | 0x44E0_03FF | 1 KB | CM_PER (Clock Module Peripheral) |
| 0x44E0_0400 | 0x44E0_04FF | 256 B | CM_WKUP (Clock Module Wakeup) |
| 0x44E0_5000 | 0x44E0_5FFF | 4 KB | DMTimer0 |
| 0x44E0_7000 | 0x44E0_7FFF | 4 KB | GPIO0 |
| 0x44E0_9000 | 0x44E0_9FFF | 4 KB | UART0 (debug console) |
| 0x44E0_B000 | 0x44E0_BFFF | 4 KB | I2C0 |
| 0x44E0_D000 | 0x44E0_EFFF | 8 KB | ADC_TSC |
| 0x44E1_0000 | 0x44E1_1FFF | 8 KB | Control Module (pinmux, pad config) |
| 0x44E3_5000 | 0x44E3_5FFF | 4 KB | WDT1 (Watchdog Timer) |
| 0x44E3_E000 | 0x44E3_EFFF | 4 KB | RTCSS (RTC) |

**L4_PER peripherals (Table 2-3)**

| Start | End | Size | Description |
|-------|-----|------|-------------|
| 0x4802_2000 | 0x4802_2FFF | 4 KB | UART1 (HILglebone STM32 link) |
| 0x4802_4000 | 0x4802_4FFF | 4 KB | UART2 |
| 0x4802_A000 | 0x4802_AFFF | 4 KB | I2C1 |
| 0x4803_0000 | 0x4803_0FFF | 4 KB | McSPI0 |
| 0x4803_8000 | 0x4803_9FFF | 8 KB | McASP0 CFG |
| 0x4803_C000 | 0x4803_DFFF | 8 KB | McASP1 CFG |
| 0x4804_0000 | 0x4804_0FFF | 4 KB | DMTimer2 |
| 0x4804_C000 | 0x4804_CFFF | 4 KB | GPIO1 |
| 0x4806_0000 | 0x4806_0FFF | 4 KB | MMCHS0 (SD card on BBB) |
| 0x480C_8000 | 0x480C_8FFF | 4 KB | Mailbox 0 |
| 0x480C_A000 | 0x480C_AFFF | 4 KB | Spinlock |
| 0x4819_C000 | 0x4819_CFFF | 4 KB | I2C2 |
| 0x481A_0000 | 0x481A_0FFF | 4 KB | McSPI1 |
| 0x481A_6000 | 0x481A_6FFF | 4 KB | UART3 |
| 0x481A_8000 | 0x481A_8FFF | 4 KB | UART4 |
| 0x481A_A000 | 0x481A_AFFF | 4 KB | UART5 |
| 0x481A_C000 | 0x481A_CFFF | 4 KB | GPIO2 |
| 0x481A_E000 | 0x481A_EFFF | 4 KB | GPIO3 |
| 0x481C_C000 | 0x481C_DFFF | 8 KB | DCAN0 |
| 0x481D_0000 | 0x481D_1FFF | 8 KB | DCAN1 |
| 0x481D_8000 | 0x481D_8FFF | 4 KB | MMC1 (eMMC on BBB) |
| 0x4820_0000 | 0x4820_0FFF | 4 KB | Interrupt Controller (INTCPS) |
| 0x4830_0000 | 0x4830_025F | -- | PWMSS0 (eCAP0, eQEP0, ePWM0) |
| 0x4830_2000 | 0x4830_225F | -- | PWMSS1 (eCAP1, eQEP1, ePWM1) |
| 0x4830_4000 | 0x4830_425F | -- | PWMSS2 (eCAP2, eQEP2, ePWM2) |
| 0x4830_E000 | 0x4830_EFFF | 4 KB | LCD Controller |

**L4_FAST peripherals (Table 2-4)**

| Start | End | Size | Description |
|-------|-----|------|-------------|
| 0x4A10_0000 | 0x4A10_7FFF | 32 KB | CPSW (Ethernet Switch Subsystem) |

### UART subsystem (relevant to HILglebone)

The AM335x has **6 UART modules** (UART0--UART5). Each is a 16C750-compatible
controller with:

- 64-byte TX/RX FIFOs
- Hardware flow control (RTS/CTS) on UART0--3
- Baud rates up to 3.6864 Mbps (depending on UART clock source)
- DMA support via the EDMA3 controller

HILglebone uses **UART1** (`0x4802_2000`, L4_PER) for STM32 communication. The
pin multiplexer registers that control which function appears on each physical
pin live in the **Control Module** (`0x44E1_0000`, L4_WKUP). The pad config
registers start at offset `+0x800` within the Control Module, so the
`pinctrl-single` base is `0x44E1_0800`. Offsets used in our device-tree
overlay:

| Pin | Pad register offset | Config value | Meaning |
|-----|---------------------|-------------|---------|
| P9.24 (uart1_txd) | 0x0984 (pinctrl: 0x184) | 0x10 | OUTPUT, PULLUP, MODE0 |
| P9.26 (uart1_rxd) | 0x0980 (pinctrl: 0x180) | 0x30 | INPUT, PULLUP, MODE0 |

The pinctrl offset is calculated as `pad_register - 0x0800`. This is the value
used in `pinctrl-single,pins` inside device-tree overlays.

### Pin multiplexer

Every I/O pin on the AM335x can serve up to 8 functions (mode 0--7). The
Control Module's pad configuration registers control:

| Bit(s) | Name | Description |
|--------|------|-------------|
| 0--2 | MUXMODE | Function select (0 = primary, 7 = GPIO) |
| 3 | PUDEN | 0 = pull enabled, 1 = pull disabled |
| 4 | PUTYPESEL | 0 = pulldown, 1 = pullup |
| 5 | RXACTIVE | 0 = output only, 1 = input enabled |
| 6 | SLEWCTRL | 0 = fast, 1 = slow |

Common macros (from `am33xx.h` in the kernel DT includes):

| Macro | Value | Use for |
|-------|-------|---------|
| PIN_OUTPUT | 0x08 | TX pins (pull disabled) |
| PIN_OUTPUT_PULLUP | 0x10 | TX pins (pull to Vcc) |
| PIN_INPUT | 0x28 | RX pins (pull disabled) |
| PIN_INPUT_PULLUP | 0x30 | RX pins (pull to Vcc, idle-high) |
| PIN_INPUT_PULLDOWN | 0x20 | RX pins (pull to GND) |

### PRU-ICSS (future reference)

The two Programmable Realtime Units (PRU0, PRU1) are independent 32-bit RISC
cores running at 200 MHz with single-cycle instruction execution and
deterministic I/O. They have a 12 KB data RAM each plus an 8 KB shared RAM.

HILglebone does not currently use the PRUs, but they are worth noting because
they can bit-bang protocols or implement high-speed GPIO toggling at 5 ns
resolution.

---

## Official documentation

### Primary references

| Document | Description | Link |
|----------|-------------|------|
| AM335x Technical Reference Manual (TRM) | 5000+ page register-level reference for every subsystem | https://www.ti.com/lit/ug/spruh73q/spruh73q.pdf |
| AM335x Datasheet | Electrical specs, pinout, package info | https://www.ti.com/lit/ds/symlink/am3358.pdf |
| AM335x ARM Cortex-A8 Microprocessors (Sitara) Product Page | Downloads, errata, tools | https://www.ti.com/product/AM3358 |

### Subsystem-specific TRM chapters

| Topic | TRM Chapter | Key content |
|-------|-------------|-------------|
| UART | Chapter 19 | Register descriptions, FIFO config, baud rate calculation |
| Control Module (pinmux) | Chapter 9 | Pad configuration registers, mode selection |
| GPIO | Chapter 25 | GPIO registers, interrupt config |
| EDMA3 (DMA) | Chapter 11 | Channel mapping, transfer descriptors |
| PRU-ICSS | Chapter 4 | PRU architecture, shared memory, instruction set |
| Clocking / PRCM | Chapter 8 | Module clock enable, PLL configuration |
| Boot (ROM code) | Chapter 26 | Boot sequence, boot device selection, MLO loading |

### Errata

| Document | Link |
|----------|------|
| AM335x Silicon Errata | https://www.ti.com/lit/er/sprz360i/sprz360i.pdf |

---

## How this relates to HILglebone

```
                  AM335x SoC
    +--------------------------------------+
    |  Cortex-A8         UART1 (0x48022000) |---P9.24 TX---> STM32 RX
    |  Linux kernel           |            |
    |  /dev/ttyS1 <-----------+            |<--P9.26 RX---- STM32 TX
    |                                      |
    |  GPIO1 (0x4804C000)                  |--- LEDs, status signals
    |                                      |
    |  eMMC (MMC1)  DDR3 (512MB)           |
    +--------------------------------------+
```

The orchestrator Python process runs on the Cortex-A8 under Linux, opens
`/dev/ttyS1` (UART1), and exchanges binary frames with the STM32 emulator
using the protocol defined in `protocol/protocol-spec.md`.
