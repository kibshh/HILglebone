# SPI Slave Emulation Spec

Payload formats for the SPI slave protocol (`protocol_id = 0x02`).
Plugs into the generic protocol in [protocol-spec.md](protocol-spec.md).

## 1. What this emulation provides

The STM32 acts as an **SPI slave**: the DUT is the SPI master and
initiates transactions; the STM32 shifts out a pre-loaded response
buffer and optionally records what the DUT sent.

From the DUT's perspective the STM32 pin looks like a real SPI peripheral
(sensor, ADC, EEPROM, etc.): it drives MISO with whatever the BBB last
loaded via `CMD_SET_OUTPUT`, in full-duplex, at whatever clock the DUT
chooses.

**Typical uses:** SPI sensor emulation (barometer, IMU, ADC), SPI
EEPROM / flash stub, generic SPI responder for protocol testing.

## 2. Hardware

### STM32F401RE SPI peripherals (slave role)

The same SPI1 / SPI2 peripherals that can act as master can also act as
slave.  A peripheral may be claimed as master **or** slave — not both
simultaneously (enforced by the shared SPI resource layer, see §3).

| SPI | Bus  | Default pin candidates (AF5)                                    |
|-----|------|-----------------------------------------------------------------|
| 1   | APB2 | MISO: PA6, PB4; SCK: PA5, PB3; NSS: PA4, PA15; MOSI: PA7, PB5 |
| 2   | APB1 | MISO: PB14, PC2; SCK: PB10, PB13; NSS: PB9, PB12; MOSI: PB15  |

### Pin roles (slave mode)

| Pin   | Direction | Required | Description |
|-------|-----------|----------|-------------|
| SCK   | Input     | Yes      | DUT-driven clock |
| NSS   | Input     | Yes      | DUT-driven chip select (hardware NSS) |
| MISO  | Output    | Yes      | STM32 response data, driven while NSS is asserted |
| MOSI  | Input     | No       | Data from DUT; captured if pin is configured |

MOSI is optional — if `mosi_port = 0xFF` the pin is not configured and
received bytes are discarded.

## 3. Resource model

### Shared SPI peripheral layer

Both SPI master (`spi_master`) and SPI slave (`spi_slave`) drivers must
agree on which peripherals are already in use and in which role.  A new
thin shared module (`drivers/spi/spi.c` + `drivers/spi/inc/spi.h`)
tracks this:

- Exports the `spi_periph_t` enum (SPI1, SPI2) — used by both master
  and slave headers, replacing the current `spi_master_periph_t`.
- Exports `spi_claim(periph, role)` and `spi_release(periph)`.
- Returns `ERR_PERIPHERAL_BUSY` if the peripheral is already claimed
  in any role.

`spi_master` and `spi_slave` call `spi_claim` / `spi_release` internally;
sensor backends never call them directly.

### Per-sensor state machine

```
    FREE ──── CMD_SETUP ───► RUNNING
     ▲                           │
     └────── CMD_STOP ───────────┘
```

Each `CMD_SETUP` claims one SPI peripheral for slave use.  Only one
slave sensor may use a given peripheral at a time (there is no
multi-channel multiplexing like the DAC8568 — the peripheral itself is
the single resource).

### TX buffer persistence

The STM32 holds one TX buffer per slave instance.  The buffer is
**persistent**: it is served to every DUT transaction until the BBB
explicitly replaces it via `CMD_SET_OUTPUT`.  The BBB does not need to
reload the buffer after each DUT read — this mirrors the DAC model
where the output value holds until changed.

`CMD_SET_OUTPUT` replaces the entire buffer atomically; it takes effect
for the next NSS assertion (an in-flight transaction is never
interrupted).

If the DUT clocks more bytes in a single transaction than the buffer
holds, the STM32 outputs `0x00` for the excess bytes.  If the DUT
clocks fewer bytes than the buffer holds, the remaining buffer bytes are
simply not sent that transaction — they are still available for the next
one.

## 4. CMD_SETUP_SENSOR — SPI slave payload (`protocol_id = 0x02`)

Bytes starting *after* the generic `protocol_id` byte:

| Offset | Size | Name            | Description |
|--------|------|-----------------|-------------|
| 0      | 1    | `spi_periph`    | `0`=SPI1, `1`=SPI2 |
| 1      | 1    | `spi_mode`      | `0`=Mode 0 .. `3`=Mode 3 (must match DUT) |
| 2      | 1    | `data_bits`     | Frame size: `8` or `16` |
| 3      | 1    | `miso_port`     | GPIO port for MISO |
| 4      | 1    | `miso_pin`      | 0..15 |
| 5      | 1    | `miso_af`       | Alternate-function number (typically 5) |
| 6      | 1    | `sck_port`      | GPIO port for SCK |
| 7      | 1    | `sck_pin`       | 0..15 |
| 8      | 1    | `sck_af`        | Typically 5 |
| 9      | 1    | `nss_port`      | GPIO port for NSS / CS |
| 10     | 1    | `nss_pin`       | 0..15 |
| 11     | 1    | `nss_af`        | Typically 5 (hardware NSS) |
| 12     | 1    | `mosi_port`     | GPIO port for MOSI. `0xFF` = MOSI not wired / not recorded |
| 13     | 1    | `mosi_pin`      | 0..15. Ignored if `mosi_port = 0xFF` |
| 14     | 1    | `mosi_af`       | Ignored if `mosi_port = 0xFF` |
| 15     | 2    | `tx_buf_len`    | u16 LE. Length of the initial TX buffer (1..`SPI_SLAVE_TX_BUF_MAX`) |
| 17     | N    | `tx_buf`        | Initial response bytes (N = `tx_buf_len`) |

Total: **17 + N bytes** (`tx_buf_len` is N).

The initial TX buffer is pre-loaded before the first NSS assertion.

### NSS mode

Hardware NSS is used (the SPI peripheral monitors the NSS pin and
automatically enables the slave shift register).  Software-driven NSS
is not supported in slave mode because the timing requirements are too
tight for a GPIO ISR.

## 5. CMD_SET_OUTPUT — SPI slave payload

Bytes starting *after* the generic `sensor_id` byte:

| Offset | Size | Name         | Description |
|--------|------|--------------|-------------|
| 0      | 2    | `tx_buf_len` | u16 LE. New buffer length (1..`SPI_SLAVE_TX_BUF_MAX`) |
| 2      | N    | `tx_buf`     | New response bytes |

Replaces the current TX buffer.  The new data is served to the DUT on
every subsequent transaction until replaced again.

## 6. CMD_STOP_SENSOR — SPI slave

| Offset | Size | Name        |
|--------|------|-------------|
| 0      | 1    | `sensor_id` |

On STOP: disable the SPI peripheral, revert all configured GPIO pins to
floating input, release the SPI peripheral slot in the shared resource
layer.

## 7. Errors

| Code | Name                    | When |
|------|-------------------------|------|
| 0x02 | `ERR_MALFORMED_PAYLOAD` | Setup shorter than 17 + `tx_buf_len` bytes; set-output shorter than 2 + `tx_buf_len` bytes |
| 0x05 | `ERR_OUT_OF_RESOURCES`  | `tx_buf_len` exceeds `SPI_SLAVE_TX_BUF_MAX`; sensor manager full |
| 0x06 | `ERR_INVALID_SENSOR_ID` | `sensor_id` not registered (set-output / stop) |
| 0x07 | `ERR_INVALID_PARAMETER` | Any field out of range; unsupported `data_bits` value |
| 0x08 | `ERR_PERIPHERAL_BUSY`   | The requested SPI peripheral is already claimed (as master or slave) |

## 8. Open questions / deferred

- **RX capture** — record what the DUT sends during each transaction
  and push it to the BBB via an unsolicited `RSP_DATA` or via a
  `CMD_READ_INPUT` command.  Deferred until a use-case requires it.
- **Transaction-level callbacks** — notify the BBB after each NSS
  de-assertion (DUT completed a transaction).  Useful for emulating
  sensors that expose a "data-ready" flag.
- **16-bit frame support** — `data_bits = 16` is listed but not
  prioritised for initial implementation.
