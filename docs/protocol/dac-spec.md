# DAC (Analog Output) Emulation Spec — DRAFT

Payload formats for the analog-output protocol (`protocol_id = 0x05`).
Plugs into the generic protocol in [protocol-spec.md](protocol-spec.md).

## 1. What this emulation provides

Continuous analog voltage output via an external **TI DAC8568** 8-channel
16-bit DAC connected to the STM32 over SPI.  The STM32 acts as the SPI
master; the SPI driver is ISR-driven (TXE/RXNE), so the protocol task
blocks on a semaphore for the ~3 µs the transfer takes rather than
busy-waiting.

From the DUT's perspective the DAC's `VOUT_x` pin behaves like a real
analog sensor output: 0 V .. V_REF, settled to the latest BBB-pushed
code within a few µs of the SPI transfer.

**Typical uses:** thermistor/RTD voltage emulation, battery-cell
voltage simulation, analog reference signals for control loops.

## 2. Hardware

### DAC8568 (external chip)

| Property | Value |
|----------|-------|
| Channels | 8 (`VOUT_A` .. `VOUT_H`) |
| Resolution | 16-bit (0..65 535 codes) |
| Reference | Internal 2.5 V (must be enabled) or external |
| Output range | 0 V .. V_REF (gain = 1) or 0 V .. 2 × V_REF (gain = 2) — fixed at chip variant |
| Settling time | ~10 µs typical |
| SPI mode | Mode 1 (CPOL=0, CPHA=1) or Mode 2 (CPOL=1, CPHA=0) |
| SPI clock max | 50 MHz |
| Frame size | 32 bits per write (4-bit prefix + 4-bit cmd + 4-bit addr + 16-bit data + 4-bit feature) |
| STM32 SPI pins | MOSI, SCK, `SYNC` (CS, software GPIO), optional `LDAC`, optional `CLR` |

The DAC8568 is **write-only** from the STM32's perspective. There is no
MISO traffic; we ignore the chip's `SDO` pin.

### STM32F401RE SPI peripherals

We support **SPI1** (APB2) or **SPI2** (APB1).  Per setup the BBB picks
which one.

| SPI | Bus  | Max clock           | Default pin candidates (AF5)         |
|-----|------|---------------------|--------------------------------------|
| 1   | APB2 | 84 MHz / 2 = 42 MHz | MOSI: PA7, PB5; SCK: PA5, PB3        |
| 2   | APB1 | 42 MHz / 2 = 21 MHz | MOSI: PB15, PC3; SCK: PB10, PB13     |

### CS / LDAC handling

- **CS (`SYNC`)** is driven as a software GPIO by the firmware: pulled
  low before the SPI transfer, raised at the end. We do **not** use the
  STM32 SPI peripheral's hardware NSS — software control gives clean
  per-frame timing without quirks.
- **LDAC** is optional. If wired to a STM32 GPIO and configured here,
  the firmware can pulse it after a write to atomically load the
  channel. If `LDAC` is not configured (BBB sends `0xFF` for the port),
  the firmware assumes LDAC is tied to GND on the board (transparent
  mode) and updates take effect at the end of the SPI transfer.

## 3. Resource model

The DAC8568 is treated as a **singleton chip**: at most one DAC8568
instance per SPI peripheral.  Each "DAC sensor" claims one of its 8
channels.  Multiple sensors share the same SPI bus, CS pin, LDAC pin
and reference configuration.

State machine:

```
       FREE ──── CMD_SETUP (first channel) ────► RUNNING(ref=1)
       ▲                                        │
       │                                        │ CMD_SETUP (next channel)
       │ CMD_STOP                               ▼
       │ (last channel released)            RUNNING(ref=N)
       │                                        │
       └─────── CMD_STOP (any channel) ─────────┘
```

The first channel's setup performs the full bring-up: enable RCC
clocks, configure GPIO AF for MOSI/SCK, configure CS/LDAC as plain
outputs, configure the SPI peripheral (ISR-driven, no DMA), and send
the DAC8568 internal-reference-enable command (if requested).  A
ref-counter tracks how many channels are active; on the last STOP the
firmware tears everything down and gates the clocks.

### Conflict detection

A second SETUP that requests an inconsistent configuration is rejected:
- Different MOSI / SCK / CS / LDAC pins → `ERR_DAC_PIN_MISMATCH`
- Different SPI clock or SPI mode → `ERR_DAC_CLOCK_MISMATCH`
- Same `channel` already in use → `ERR_DAC_CHANNEL_IN_USE`

(There is no separate "different `spi_periph`" rejection — each SPI
peripheral has its own chip slot, so SETUPs on different peripherals
just initialise independent instances.)

## 4. CMD_SETUP_SENSOR — DAC payload (`protocol_id = 0x05`)

Bytes starting *after* the generic `protocol_id` byte:

| Offset | Size | Name              | Description |
|--------|------|-------------------|-------------|
| 0      | 1    | `spi_periph`      | `0`=SPI1, `1`=SPI2 |
| 1      | 4    | `spi_clock_hz`    | u32 LE. SPI baud rate. Max 21 MHz on SPI2, 42 MHz on SPI1. |
| 5      | 1    | `mosi_port`       | GPIO port for MOSI |
| 6      | 1    | `mosi_pin`        | 0..15 |
| 7      | 1    | `mosi_af`         | Alternate-function number (typically 5) |
| 8      | 1    | `sck_port`        | GPIO port for SCK |
| 9      | 1    | `sck_pin`         | 0..15 |
| 10     | 1    | `sck_af`          | Typically 5 |
| 11     | 1    | `cs_port`         | GPIO port for CS / SYNC (software-driven output) |
| 12     | 1    | `cs_pin`          | 0..15 |
| 13     | 1    | `ldac_port`       | LDAC port. `0xFF` = LDAC tied to GND on the board (transparent mode) |
| 14     | 1    | `ldac_pin`        | Ignored if `ldac_port = 0xFF` |
| 15     | 1    | `channel`         | DAC8568 channel: `0`=A, `1`=B, …, `7`=H |
| 16     | 1    | `reference`       | `0`=external, `1`=internal 2.5 V static, `2`=internal 2.5 V flexible |
| 17     | 1    | `spi_mode`        | `1` = CPOL 0, CPHA 1 (recommended); `2` = CPOL 1, CPHA 0 |
| 18     | 2    | `initial_value`   | u16 LE. Initial DAC code (0..65 535). Loaded as part of setup. |

Total: **20 bytes** fixed.

## 5. CMD_SET_OUTPUT — DAC payload

Bytes starting *after* the generic `sensor_id` byte:

| Offset | Size | Name    | Description |
|--------|------|---------|-------------|
| 0      | 2    | `value` | u16 LE. New DAC code (0..65 535). Mapped to 0 V .. V_REF (gain 1) on the channel. |

Total: **2 bytes**. One SET_OUTPUT → one analog level. No sample
buffering or streaming; the BBB issues a new command whenever the
level needs to change.

The firmware:
1. Builds the 32-bit DAC8568 command frame for "write-and-update channel X with value V".
2. Asserts CS (low).
3. Enables the SPI TX interrupt and starts shifting out the 4 bytes.
4. Returns; the protocol task suspends on a semaphore.
5. SPI TX-complete ISR deasserts CS (high), pulses LDAC if configured, and gives the semaphore.
6. Protocol task resumes and ACKs the command.

The protocol task is not polling — it blocks on the semaphore and yields to other tasks while the transfer runs (~3.2 µs at 10 MHz).

## 6. CMD_STOP_SENSOR — DAC

| Offset | Size | Name        |
|--------|------|-------------|
| 0      | 1    | `sensor_id` |

On STOP:

1. Free the channel slot (decrement ref-count).
2. Optionally (if firmware adds it later) send a "power-down channel" command to the chip, leaving the pin in a defined state.
3. If this was the last active channel, fully tear down: disable SPI, revert MOSI/SCK/CS/LDAC pins to floating input, gate RCC clocks.

## 7. Errors

| Code | Name | When |
|------|------|------|
| 0x02 | `ERR_MALFORMED_PAYLOAD`         | Setup < 20 bytes; set-output < 2 bytes |
| 0x05 | `ERR_OUT_OF_RESOURCES`      | Semaphore allocation failed; sensor manager full |
| 0x06 | `ERR_INVALID_SENSOR_ID`    | `sensor_id` not registered (set-output/stop) |
| 0x07 | `ERR_INVALID_PARAMETER`     | Any field out of range; `spi_master_init` rejected the clock/mode |
| 0x0B | `ERR_INTERNAL`          | `spi_master_write` returned non-OK, or TX semaphore timed out |
| 0x80 | `ERR_DAC_PIN_MISMATCH`  | A second-channel setup specifies different MOSI/SCK/CS/LDAC than the running instance |
| 0x81 | `ERR_DAC_CLOCK_MISMATCH`| A second-channel setup specifies a different `spi_clock_hz` or `spi_mode` |
| 0x82 | `ERR_DAC_CHANNEL_IN_USE`| The requested DAC8568 channel is already claimed by another sensor |

(Slice 0x80..0x9F is the DAC slice of the protocol error space, in
keeping with one-protocol-per-16-codes from
[protocol-spec.md](protocol-spec.md).)

## 8. DAC8568 command frame

A single 32-bit big-endian frame sent over SPI MSB-first:

| Bits  | Width | Field     | Description |
|-------|-------|-----------|-------------|
| 31:28 | 4     | `prefix`  | Always `0x0` |
| 27:24 | 4     | `command` | Operation to perform (see table below) |
| 23:20 | 4     | `address` | Target channel: `0x0`=A .. `0x7`=H |
| 19:4  | 16    | `data`    | DAC code (0..65 535) |
| 3:0   | 4     | `feature` | `0x0` for normal operation |

Common commands we care about:

| Command nibble | Meaning |
|----------------|---------|
| `0x0`          | Write to input register N (no update) |
| `0x1`          | Update DAC register N (load) |
| `0x3`          | Write input register N and update channel N (write-and-load) — **most common** |
| `0x8`          | Setup internal reference: data field selects static / flexible mode |

For a single-value update we use command `0x3` with the target channel
in `address` (0..7 = A..H), the value in `data`, and `feature = 0`.

## 9. Open questions / deferred

- **Synchronous multi-channel updates** — write multiple channels with
  LDAC held high, then pulse LDAC once to load all atomically. Removes
  inter-channel skew.
- **Power-down on STOP** — explicit DAC8568 power-down command per
  channel so the released `VOUT_x` returns to a defined state.
- **Filtered-PWM fallback** — if no DAC8568 board is available, the
  same `protocol_id = 0x05` could route to an internal STM32 PWM with
  an external RC filter. Different sub-protocol, deferred until needed.
- **Concurrent SPI users** — if a future sensor also wants the same
  SPI peripheral an arbiter (mutex) will be needed. For now, the DAC
  owns the SPI bus exclusively while initialised.

## 10. Worked example — drive 1.25 V on VOUT_A

Hardware: DAC8568 on SPI1, MOSI=PA7, SCK=PA5, CS=PA4, LDAC tied to GND,
internal 2.5 V reference.  1.25 V = code 32768 (half-scale).

### Setup

```
CMD_SETUP_SENSOR payload:
  protocol_id    = 0x05
  spi_periph     = 0x00       (SPI1)
  spi_clock_hz   = 10_000_000 (10 MHz, LE: 80 96 98 00)
  mosi_port      = 0x00       (A)
  mosi_pin       = 0x07
  mosi_af        = 0x05
  sck_port       = 0x00       (A)
  sck_pin        = 0x05
  sck_af         = 0x05
  cs_port        = 0x00       (A)
  cs_pin         = 0x04
  ldac_port      = 0xFF       (LDAC tied to GND)
  ldac_pin       = 0x00       (ignored)
  channel        = 0x00       (A)
  reference      = 0x01       (internal 2.5 V static)
  spi_mode       = 0x01       (CPOL=0, CPHA=1)
  initial_value  = 0x8000     (32768, LE: 00 80)
```

STM32 brings up SPI1, enables the DAC's internal reference
(command `0x08`), then writes `0x0300_8000` (write-and-load CH A with
value 0x8000). VOUT_A settles at ~1.25 V. ACKs with `sensor_id = 0x01`.

### Drive to 0.5 V

```
CMD_SET_OUTPUT payload:
  sensor_id  = 0x01
  value      = 0x3333          (13107 → ~0.5 V at 2.5 V Vref, LE: 33 33)
```

### Teardown

```
CMD_STOP_SENSOR:
  sensor_id  = 0x01
```

(If this is the only active channel, SPI and pins are released.)
