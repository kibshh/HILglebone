# BeagleBone Black (BBB) -- Board Reference

The BeagleBone Black Rev C is the orchestrator platform for HILglebone. This
document covers the board-level hardware, the expansion headers, power,
storage, and boot, with links to the official BeagleBoard.org documentation.

---

## Board specifications

| Property | Value |
|----------|-------|
| SoC | TI AM3358BZCZ100 (1 GHz Cortex-A8) |
| RAM | 512 MB DDR3L (on-package, 400 MHz) |
| eMMC | 4 GB (on-board, connected to MMC1) |
| MicroSD slot | Yes (connected to MMC0; boot priority depends on S2 button) |
| Ethernet | 10/100 (Microchip LAN8710A PHY) |
| USB | 1x USB 2.0 Host (Type-A), 1x USB 2.0 Client (mini-B, also powers board) |
| Serial debug | 6-pin header (J1) -- UART0 @ 115200 baud (3.3V TTL) |
| Video | micro-HDMI (not used by HILglebone) |
| Power | 5V DC barrel jack (recommended) or USB client |
| Dimensions | 86.36 mm x 54.61 mm |
| Revision | Rev C (current production) |

---

## Expansion headers (P8 and P9)

The BBB exposes 92 pins across two 2x23 headers (P8 and P9). Each pin can be
multiplexed to different functions via the AM335x pinmux (see
`am335x-sitara-soc.md`).

### P9 header -- pins used by HILglebone

| Pin | Default function | HILglebone use | Notes |
|-----|-----------------|----------------|-------|
| P9.1 | GND | GND (shared with STM32) | **Must connect** |
| P9.2 | GND | GND (alternate) | |
| P9.3 | VDD_3V3 | -- | 3.3V output, 250 mA max |
| P9.4 | VDD_3V3 | -- | |
| P9.5 | VDD_5V | -- | System 5V (input or output) |
| P9.6 | VDD_5V | -- | |
| P9.7 | SYS_5V | -- | Regulated 5V from board |
| P9.8 | SYS_5V | -- | |
| P9.24 | uart1_txd | **UART1 TX -> STM32 RX** | Mode 0, output, pullup |
| P9.26 | uart1_rxd | **UART1 RX <- STM32 TX** | Mode 0, input, pullup |

### Full P9 pinout

```
         P9 Header (active side facing you)
     +------+------+
   1 | GND  | GND  | 2
   3 | 3V3  | 3V3  | 4
   5 | VDD5 | VDD5 | 6
   7 | SYS5 | SYS5 | 8
   9 | PWR  | RST  | 10
  11 | U4RX | GPIO | 12
  13 | U4TX | GPIO | 14
  15 | GPIO | GPIO | 16
  17 | I2C1 | I2C1 | 18
  19 | I2C2 | I2C2 | 20
  21 | GPIO | GPIO | 22
  23 | GPIO | U1TX | 24  <-- HILglebone TX
  25 | GPIO | U1RX | 26  <-- HILglebone RX
  27 | GPIO | SPI1 | 28
  29 | SPI1 | SPI1 | 30
  31 | SPI1 | ADC  | 32
  33 | ADC  | ADC  | 34
  35 | ADC  | ADC  | 36
  37 | ADC  | ADC  | 38
  39 | ADC  | ADC  | 40
  41 | CLKO | GPIO | 42
  43 | GND  | GND  | 44
  45 | GND  | GND  | 46
     +------+------+
```

### Full P8 pinout (summary)

P8 carries mostly GPIO and LCD/display signals. HILglebone does not currently
use any P8 pins, but they are available for future expansion (e.g., GPIO-based
DUT reset lines, SPI for high-speed data, or LCD status display).

### Voltage levels

All expansion header I/O is **3.3V**. The STM32F401RE runs at 3.3V and its
UART pins are 3.3V-compatible, so **no level shifting is needed** for the
HILglebone UART link. If you connect to 5V-logic devices, you need a level
shifter or voltage divider.

---

## Boot sequence

1. **ROM bootloader** (on-chip, AM335x Boot ROM)
   - Reads S2 button and SYSBOOT pins to determine boot order
   - Default: eMMC first, then SD card
   - Hold S2 during power-on to force SD-card-first boot
2. **MLO** (first-stage bootloader, SPL)
   - Loaded from FAT partition on eMMC or SD card
   - Initializes DDR3, clocks, UART0
   - Loads U-Boot
3. **U-Boot** (second-stage bootloader)
   - Reads `uEnv.txt` for environment overrides (kernel, DTB, overlays)
   - Loads kernel + DTB + optional overlays into RAM
   - Jumps to kernel
4. **Linux kernel**
   - Applies device-tree overlays (if configured in U-Boot)
   - Mounts rootfs from eMMC partition or SD card
   - Launches systemd (or sysvinit)

### SD card layout (Yocto wic image)

The `wic.xz` image produced by Yocto contains:

| Partition | Type | Content |
|-----------|------|---------|
| 1 | FAT16 (boot) | MLO, u-boot.img, uEnv.txt, zImage, DTBs, overlays |
| 2 | ext4 (rootfs) | Full Linux root filesystem |

Flashing to SD:
```bash
xzcat hilglebone-image-beaglebone.wic.xz | sudo dd of=/dev/sdX bs=4M status=progress
sync
```

Or with bmaptool (faster, skips empty regions):
```bash
bmaptool copy hilglebone-image-beaglebone.wic.xz /dev/sdX
```

---

## Serial debug console

The 6-pin header **J1** exposes UART0:

```
J1 pinout (left to right, component side up):
  Pin 1: GND
  Pin 4: RX (input to BBB)
  Pin 5: TX (output from BBB)
```

Connect a 3.3V FTDI cable and open at **115200 8N1**. This is invaluable for
debugging U-Boot, kernel boot, and early systemd startup before the network
is up.

---

## Power considerations

| Source | Voltage | Max current | Notes |
|--------|---------|-------------|-------|
| Barrel jack (J5) | 5V DC | 2A recommended | Preferred for reliability |
| USB client (J6) | 5V | 500 mA (USB 2.0 spec) | May brown-out under load (USB peripherals, capes) |
| VDD_5V (P9.5/6) | 5V | Shared with system | Can be used as power input, but no reverse-polarity protection |

For HILglebone, power via the **barrel jack** to avoid USB current limits
affecting UART timing or causing random resets under load.

---

## Official documentation

### Primary references

| Document | Description | Link |
|----------|-------------|------|
| BeagleBone Black System Reference Manual (SRM) | Schematics, BOM, connector pinout, boot sequence | https://docs.beagleboard.org/latest/boards/beaglebone/black/index.html |
| Detailed Hardware Design | Hardware design with schematics | https://docs.beagleboard.org/boards/beaglebone/black/ch06.html |
| BeagleBone Cape Expansion Headers | Definitive header pinout table with all 8 mux modes | https://docs.beagleboard.org/latest/boards/beaglebone/black/ch07.html |
| U-Boot for BeagleBone | U-Boot configuration, uEnv.txt, overlay loading | https://docs.u-boot.org/en/latest/board/ti/am335x_evm.html |

### Community and support

| Resource | Link |
|----------|------|
| BeagleBoard.org forum | https://forum.beagleboard.org/ |
| BeagleBone cookbook (examples) | https://docs.beagleboard.org/latest/books/beaglebone-cookbook/index.html |
| eLinux BeagleBone wiki | https://elinux.org/Beagleboard:BeagleBoneBlack |

---

## HILglebone wiring summary

```
BeagleBone Black                     STM32F401RE (Nucleo)
+--------------+                     +------------------+
|              |                     |                  |
| P9.24 (TX) -|------ wire -------->|- PA10 (USART2 RX)|
|              |                     |                  |
| P9.26 (RX) -|<----- wire ---------|-- PA2 (USART2 TX)|
|              |                     |                  |
| P9.1  (GND)-|------ wire ---------|-- GND            |
|              |                     |                  |
+--------------+                     +------------------+

Both sides are 3.3V logic -- no level shifter required.
```
