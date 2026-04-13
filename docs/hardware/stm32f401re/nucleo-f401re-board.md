# Nucleo-F401RE -- Development Board Reference

The Nucleo-F401RE is the development board used in HILglebone for the
STM32F401RE real-time signal emulator. This document covers board-specific
details. For MCU-level information, see `stm32f401re-mcu.md`.

---

## Board features

| Feature | Detail |
|---------|--------|
| ST-Link/V2-1 | On-board debugger/programmer, USB-accessible |
| Virtual COM port | USART2 PA2/PA3 routed to ST-Link USB |
| Arduino headers | Pin-compatible with Arduino Uno shields (CN5, CN6, CN8, CN9) |
| Morpho headers | 2x 38-pin headers exposing all MCU pins |
| User button | B1, connected to PC13 (active low) |
| User LED | LD2, connected to PA5 |
| Power | 5V from USB, or 7--12V on VIN, or 3.3V direct |
| Crystal | 8 MHz HSE (X3, not mounted by default on some revisions) |

---

## Morpho connector pinout (HILglebone-relevant pins)

```
Nucleo-F401RE (Morpho headers)

Pin     | MCU Pin | HILglebone use
--------+---------+-----------------------------------------
CN5.1   | PA9     | USART1 TX -> BBB P9.26 (UART1 RX)
CN5.2   | PA10    | USART1 RX <- BBB P9.24 (UART1 TX)
CN10.20 | GND     | Common ground with BBB
```

USART2 (PA2/PA3) remains connected to the ST-Link virtual COM port via
solder bridges SB62/SB63, available as a debug console over USB.

Additional Morpho pins will be used as the peripheral allocation is finalized
(I2C SDA/SCL, SPI MISO/MOSI/SCK/CS, PWM outputs, ADC inputs).

---

## Programming and debugging

The on-board ST-Link is used for flashing and debugging via OpenOCD:

```bash
cd stm32
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake --build build
cmake --build build --target flash
```

The `flash` target runs OpenOCD with `interface/stlink.cfg` and
`target/stm32f4x.cfg` to program the ELF via ST-Link.

---

## Official documentation

| Document | Description | Link |
|----------|-------------|------|
| Nucleo-F401RE User Manual (UM1724) | Board layout, solder bridges, jumper config, Morpho/Arduino pinout | https://www.st.com/resource/en/user_manual/um1724-stm32-nucleo64-boards-mb1136-stmicroelectronics.pdf |
