# I2C Sensor Emulation Spec

This document defines the payload formats used when the BeagleBone Black asks
the STM32 to emulate an I2C slave device (a.k.a. "sensor") and when it
subsequently drives that sensor's state. It plugs into the generic protocol
defined in [protocol-spec.md](protocol-spec.md) -- the framing, sequencing,
CRC, unified `RSP_ACK` semantics, and `sensor_id` lifecycle are inherited
from there.

## 1. What this emulation provides

The STM32 acts as an **I2C slave** on one of its hardware I2C peripherals
(I2C1, I2C2, I2C3). To the Device Under Test (DUT) it looks like a real
I2C device:

- Responds to START + its configured 7- or 10-bit address
- Exposes a register map (8-bit or 16-bit register addressing, 8-bit registers)
- Accepts writes from the DUT, services reads from the DUT
- Optionally clock-stretches, optionally auto-increments, optionally supports
  SMBus block ops with PEC

The BBB drives the register contents via `CMD_SET_OUTPUT`. When the DUT reads
a register, it sees whatever the BBB last wrote into that register slot.

This is enough to emulate most read-oriented sensors (temperature, humidity,
pressure, IMU, ADC, EEPROM snapshot, fuel gauge, etc.) faithfully. Sensors
that require handshaking logic inside themselves (e.g. FIFOs, interrupt
generation based on thresholds, internal auto-measurement state machines) are
not modeled at this layer -- the BBB is expected to precompute the values and
stream them via `CMD_SET_OUTPUT`.

## 2. Hardware constraints on STM32F401RE

All three I2C peripherals (`I2C1`, `I2C2`, `I2C3`) are wired to the same
physical bus that reaches the DUT. From the BBB's point of view they are
interchangeable: the STM32 auto-allocates the first free peripheral on each
`CMD_SETUP_SENSOR` and returns `ERR_I2C_NO_FREE_PERIPHERAL` once all three
are in use.

| Constraint                 | Value                                                                   |
|----------------------------|-------------------------------------------------------------------------|
| Max concurrent I2C sensors | 3 (one per peripheral)                                                  |
| Max clock                  | 400 kHz (Fast-mode). Fast-mode-plus / 1 MHz **not** supported on F401   |
| Addressing modes           | 7-bit and 10-bit                                                        |
| Dual addressing            | Supported in 7-bit mode only (STM32 `OAR2.ADD2`)                        |
| General call               | Optional                                                                |
| Logic voltage              | 3.3 V; DUT pull-ups must be sized to ~3.3 V                             |
| External pull-ups          | Required for reliable fast-mode operation (internal pulls are ~40 kΩ)   |

## 3. CMD_SETUP_SENSOR -- I2C payload (`protocol_id = 0x01`)

Outer wrapper is defined in [protocol-spec.md](protocol-spec.md). The bytes
starting at payload offset 0 (the `protocol_id` byte) and onward are:

| Offset | Size | Name                   | Description                                                                    |
|--------|------|------------------------|--------------------------------------------------------------------------------|
| 0      | 1    | `protocol_id`          | `0x01` = I2C                                                                   |
| 1      | 4    | `clock_hz`             | Bus clock in Hz, LE. Typical: `100000`, `400000`                               |
| 5      | 1    | `address_mode`         | `0` = 7-bit, `1` = 10-bit                                                      |
| 6      | 2    | `primary_addr`         | Device address, LE. 7-bit mode: bits 0..6. 10-bit mode: bits 0..9              |
| 8      | 2    | `secondary_addr`       | Second address (7-bit mode only); `0x0000` = disabled                          |
| 10     | 1    | `flags`                | Bitfield; see "Flags" below                                                    |
| 11     | 1    | `reg_addr_width`       | `0` = no reg addressing (stream sensor), `1` = 8-bit, `2` = 16-bit             |
| 12     | 1    | `reg_addr_endian`      | `0` = big-endian (most sensors), `1` = little-endian. Meaningful only when     |
|        |      |                        | `reg_addr_width = 2`                                                           |
| 13     | 1    | `auto_inc_mode`        | `0` = none, `1` = on read, `2` = on write, `3` = both                          |
| 14     | 2    | `register_count`       | Number of 8-bit registers in the map, LE. Max: see "Memory budget"             |
| 16     | 2    | `response_delay_us`    | Pre-data stretch after address match, µs, LE. `0` = no delay                   |
| 18     | 2    | `clock_stretch_max_us` | Upper bound on clock stretching this sensor may do, µs, LE. `0` = no stretch   |
| 20     | 1    | `has_preset`           | `0` = no preset block follows; `1` = preset block follows (see below)          |
| 21     | 2    | `preset_reg_start`     | (only if `has_preset = 1`) Register offset where preset data starts, LE        |
| 23     | 2    | `preset_value_len`     | (only if `has_preset = 1`) Number of bytes in the preset block, LE. Must `>= 1`|
| 25     | N    | `preset_values`        | (only if `has_preset = 1`) `preset_value_len` raw bytes, written sequentially  |

Total size:

- Without preset (`has_preset = 0`): **21 bytes**
- With preset (`has_preset = 1`): **25 + preset_value_len bytes**

### 3.1 Preset block

`has_preset = 1` bundles one `CMD_SET_OUTPUT`-equivalent block write into the
setup so the sensor is fully initialized in a single round-trip. This is
what you want when the DUT starts polling the bus immediately and the sensor
must already answer with valid identity / calibration bytes (e.g. the BME280
chip-ID register at `0xD0` must read `0x60` before the driver will probe any
further).

Mechanically, the effect is identical to sending `CMD_SETUP_SENSOR` (with
`has_preset = 0`) followed by `CMD_SET_OUTPUT(sensor_id, preset_reg_start,
preset_value_len, preset_values)`. The preset is applied **before** the STM32
enables the I2C peripheral, so the DUT never observes a window of all-zero
registers.

Range bounds are checked: `preset_reg_start + preset_value_len` must be
`<= register_count`, else the entire setup is rejected with
`ERR_I2C_REGISTER_OOB` (no sensor is created).

Only one contiguous block can be preset per setup. If you need to preload
non-contiguous registers, send the bulk of init via preset and follow up
with `CMD_SET_OUTPUT` for the rest.

### 3.2 Flags bitfield

| Bit | Name                     | Meaning                                                                              |
|-----|--------------------------|--------------------------------------------------------------------------------------|
| 0   | `general_call_enable`    | Respond to the general-call address (0x00)                                           |
| 1   | `smbus_mode`             | SMBus semantics (timeouts, ACK rules); `0` = plain I2C                               |
| 2   | `pec_required`           | Require PEC on SMBus block transactions. Only meaningful with `smbus_mode=1`         |
| 3   | `auto_inc_wrap`          | Auto-increment wraps at `register_count - 1`; else it saturates                      |
| 4   | `dut_writes_allowed`     | DUT writes to registers are accepted and update the map; else NACKed                 |
| 5   | `internal_pullups`       | Enable STM32 internal pull-ups on SDA/SCL. Not recommended -- too weak for reliable  |
|     |                          | fast-mode. Intended for lab sanity checks only.                                      |
| 6   | `clock_stretch_enable`   | Allow clock stretching at all. `0` forces instantaneous responses (overrides         |
|     |                          | `response_delay_us` / `clock_stretch_max_us`)                                        |
| 7   | reserved                 | Must be `0`                                                                          |

### 3.3 Register addressing modes

- `reg_addr_width = 0` **Streaming sensor** (rare). DUT issues raw
  read/write with no register offset. On read, DUT reads a single
  configurable "current value" register (index 0). On write, the byte goes
  to register 0. Used for dumb DAC-like or status-only devices.
- `reg_addr_width = 1` Classic 8-bit register offset. DUT sends `START +
  addr + write + regaddr [+ repeated START + addr + read + bytes...]` or
  `START + addr + write + regaddr + data`.
- `reg_addr_width = 2` 16-bit register offset. Byte order controlled by
  `reg_addr_endian`. Used by sensors like ADXL345 (high-capacity addr map),
  some EEPROMs.

### 3.4 Memory budget

Register storage is allocated from a shared pool on the STM32. Practical
limits on an F401RE:

| `register_count` | Approx RAM used   | Notes                                          |
|------------------|-------------------|------------------------------------------------|
| 256              | ~256 B + overhead | Typical for most sensors                       |
| 1024             | ~1 KiB            | EEPROM emulation territory                     |
| 4096             | ~4 KiB            | Hitting diminishing returns; use SPI flash sim |
| > 8192           | rejected          | Returns `ERR_I2C_REGMAP_TOO_LARGE`             |

### 3.5 Initial register state

All registers are initialized to `0x00` on successful setup. To populate
identity bytes (chip ID, revision, calibration) before the DUT can probe,
use the preset block (§3.1) in the same `CMD_SETUP_SENSOR`, or issue
`CMD_SET_OUTPUT` immediately after setup. The preset path is preferred
when the DUT starts polling before the BBB can guarantee a follow-up
message has landed.

### 3.6 Response delay vs clock stretching

Real sensors do not instantly deliver data. Two knobs control how the
emulated sensor stalls the bus:

- `response_delay_us` -- after ADDRESS + READ has been matched, the STM32
  stretches SCL for this many µs before shifting out the first data byte.
  Models the sensor's internal sample/compute latency.
- `clock_stretch_max_us` -- upper bound on all clock stretching this sensor
  performs (including `response_delay_us`). Safety rail: if the backend
  would ever stretch longer than this, it will forcibly release the clock
  and potentially serve a stale byte.

Both default to 0 (instant response) when set to zero. If the DUT is a
real-world master that does not tolerate stretching, set `flags bit 6 = 0`
to disable stretching entirely.

## 4. CMD_SET_OUTPUT -- I2C payload

Payload that follows the `sensor_id` byte (offset 1 of the generic payload):

| Offset | Size | Name        | Description                                           |
|--------|------|-------------|-------------------------------------------------------|
| 0      | 1    | `sensor_id` | From the setup ACK                                    |
| 1      | 2    | `reg_start` | Starting register offset, LE                          |
| 3      | 2    | `value_len` | Number of bytes to write into the map, LE             |
| 5      | N    | `values`    | `value_len` raw bytes, written sequentially           |

`reg_start + value_len` must be `<= register_count`, else the STM32 replies
`RSP_ACK` with `error_code = ERR_I2C_REGISTER_OOB` and `sensor_id` echoed.

This command **only updates the in-memory register map**. The next DUT read
from those registers will observe the new values. No signalling is sent to
the DUT (no interrupt, no DRDY toggle) -- that is a separate concern.

### 4.1 Multi-byte sensor values

The BBB is responsible for any unit conversion (°C to raw code, g-force to
LSB count, etc.) *and* for the byte order the emulated sensor specifies.
Example: the BME280 stores temperature as a 20-bit big-endian value split
across three registers (high, mid, low-nibble in the upper 4 bits of a byte).
The BBB crafts those three bytes and issues a single `CMD_SET_OUTPUT` with
`reg_start=0xFA, value_len=3, values=[MSB, MID, LSB]`.

## 5. CMD_STOP_SENSOR -- I2C

No I2C-specific additions. Generic stop applies:

| Offset | Size | Name        |
|--------|------|-------------|
| 0      | 1    | `sensor_id` |

On STOP the STM32 disables the I2C peripheral, reverts the associated
SDA/SCL pins to analog input (Hi-Z), and frees the sensor slot.

## 6. I2C-specific error codes

Protocol-scoped codes occupy range `0x40..0x5F` (the "I2C slice" of
`0x40..0xBF` reserved for protocol-specific errors in protocol-spec.md).

| Code | Name                             | Meaning                                                                   |
|------|----------------------------------|---------------------------------------------------------------------------|
| 0x40 | `ERR_I2C_NO_FREE_PERIPHERAL`     | All I2C peripherals are already bound to active sensors                   |
| 0x41 | `ERR_I2C_CLOCK_UNSUPPORTED`      | `clock_hz` not achievable on this peripheral                              |
| 0x42 | `ERR_I2C_ADDR_CONFLICT`          | `primary_addr` / `secondary_addr` collides with another sensor            |
| 0x43 | `ERR_I2C_ADDR_RESERVED`          | 7-bit: address in reserved range (0x00..0x07 or 0x78..0x7F) without       |
|      |                                  | the corresponding flag; 10-bit: address > 0x3FF                           |
| 0x44 | `ERR_I2C_REGMAP_TOO_LARGE`       | `register_count` exceeds the shared register-storage budget               |
| 0x45 | `ERR_I2C_BAD_ADDR_MODE`          | `address_mode` out of range or `secondary_addr != 0` in 10-bit mode       |
| 0x46 | `ERR_I2C_REGISTER_OOB`           | `CMD_SET_OUTPUT` or preset: `reg_start + value_len` exceeds               |
|      |                                  | `register_count`                                                          |
| 0x47 | `ERR_I2C_STRETCH_EXCEEDS_BUS`    | `response_delay_us` or `clock_stretch_max_us` violates `clock_hz` budget  |
| 0x48 | `ERR_I2C_SMBUS_REQUIRED`         | `pec_required` set but `smbus_mode` not set                               |
| 0x49 | `ERR_I2C_UNSUPPORTED_FEATURE`    | Field structurally valid but not implemented (e.g. 10-bit with dual addr) |

## 7. Worked example: emulating a BME280

A BME280 (Bosch humidity / temperature / pressure) on a Raspberry Pi-style
expander:

- 7-bit address `0x76`
- 8-bit register addressing
- Reads auto-increment
- Chip-ID register at `0xD0` must read `0x60`
- Temperature at `0xFA..0xFC` (20-bit big-endian across three bytes, MSB in
  0xFA, LSB nibble in upper 4 bits of 0xFC)

### 7.1 Setup

The BME280 is a plain I2C device (not SMBus), uses external board pull-ups,
saturates on register overflow (no wrap), and accepts DUT writes to its
control registers. That gives the following `flags` byte:

| Bit | Value | Reason                                                 |
|-----|-------|--------------------------------------------------------|
| 0   | 0     | No general call                                        |
| 1   | 0     | Not SMBus                                              |
| 2   | 0     | No PEC                                                 |
| 3   | 0     | No wrap on auto-inc (BME280 saturates at `0xFF`)       |
| 4   | 1     | DUT can write config registers (ctrl_hum, ctrl_meas)   |
| 5   | 0     | No internal pull-ups (external on the board)           |
| 6   | 0     | No clock stretching                                    |
| 7   | 0     | Reserved                                               |

So `flags = 0b00010000 = 0x10`.

The chip-ID byte (`0x60` at register `0xD0`) is loaded via the preset block
so the sensor answers correctly on the first DUT probe:

```
CMD_SETUP_SENSOR payload:
  protocol_id         = 0x01               (I2C)
  clock_hz            = 400000             (LE: 80 1A 06 00)
  address_mode        = 0                  (7-bit)
  primary_addr        = 0x0076             (LE: 76 00)
  secondary_addr      = 0x0000             (disabled)
  flags               = 0x10
  reg_addr_width      = 1                  (8-bit register offsets)
  reg_addr_endian     = 0                  (N/A when width=1)
  auto_inc_mode       = 1                  (on read)
  register_count      = 256                (LE: 00 01)
  response_delay_us   = 0
  clock_stretch_max_us= 0
  has_preset          = 1
  preset_reg_start    = 0x00D0             (LE: D0 00)
  preset_value_len    = 0x0001             (LE: 01 00)
  preset_values       = [0x60]
```

STM32 responds: `RSP_ACK` with payload
`[cmd_type=0x01, error_code=SUCCESS, sensor_id=0x01]` -- the id `0x01` is the
handle the BBB will use in all subsequent messages for this sensor. The
first time the DUT reads `0xD0`, it sees `0x60` and the chip is "detected".

### 7.2 Push a new temperature value

BBB computes that 20 °C maps to raw BME280 code `0x83000` (hypothetical,
actual conversion is sensor-specific). That is three bytes: `0x83 0x00 0x00`
stored across `0xFA, 0xFB, 0xFC`.

```
CMD_SET_OUTPUT:
  sensor_id  = 0x01
  reg_start  = 0x00FA   (LE: FA 00)
  value_len  = 0x0003   (LE: 03 00)
  values     = [0x83, 0x00, 0x00]
```

Subsequent DUT read of `0xFA..0xFC` returns `83 00 00`. The BBB will repeat
this at whatever cadence the simulated physical model produces new samples.

### 7.3 Teardown

```
CMD_STOP_SENSOR:
  sensor_id  = 0x01
```

The peripheral that was auto-allocated at setup is released, its SDA/SCL
pins go back to Hi-Z, and `sensor_id = 0x01` is available for the next
setup.

## 8. Open questions / deferred

Things explicitly **not** in v1; candidates for later:

- **Fault injection** (forced NACK, bit-flip, stuck bus, clock-stretch abuse,
  address drift, etc.). Was drafted in earlier revisions; deferred until the
  core setup/set-output/stop path is exercised end-to-end.
- **Variable register widths** (16-bit or 32-bit registers). Current spec is
  byte-addressable only.
- **Register-latched-on-write semantics** (sensor samples its ADC the moment
  the DUT writes a "trigger" register, then returns the result on next read).
  This requires a small scripting hook; for now the BBB can approximate it by
  subscribing to DUT writes via a future `DUT_WRITE_NOTIFY` event.
- **Interrupt / DRDY pin emulation** -- a GPIO line that the emulated sensor
  toggles when a new sample is "ready". Will live in the future
  `digital-io-spec.md` (BBB can wire the same STM32 GPIO as a separate
  digital-output sensor and toggle it explicitly).
- **FIFO-backed sensors** (many IMUs have a 32-sample FIFO that drains on
  read). Needs a queue primitive in the protocol backend.
- **SMBus block read / write** with per-block PEC.
- **Multi-master bus arbitration** -- currently the STM32 assumes it is the
  only slave and a single master drives the bus.
- **10-bit address + dual-address combinations** (may require separate
  peripheral instances).
