# UART Wire Protocol Specification

This document defines the **generic** binary communication protocol between the
BeagleBone Black (control plane) and the STM32 (real-time engine).

It covers:

- The common frame format every message uses
- The sensor lifecycle (setup -> operate -> stop)
- The full message-type table
- The sensor_id model and acknowledgment semantics
- The common error-code space
- Synchronization and link error handling

Protocol-specific payloads (what bytes go inside `CMD_SETUP_SENSOR` and
`CMD_SET_OUTPUT` for each protocol the STM32 can emulate) live in per-protocol
spec files:

- [i2c-sensors-spec.md](i2c-sensors-spec.md) -- I2C slave emulation
- `spi-sensors-spec.md` -- (TBD) SPI slave emulation
- `digital-io-spec.md` -- (TBD) digital output / input simulation
- `analog-spec.md` -- (TBD) DAC / PWM / analog output simulation

## Design Principles

- Binary, compact framing (no JSON or text on the wire)
- Deterministic: fixed-size header, bounded message lengths
- Explicit message types with mandatory acknowledgment
- CRC for integrity checking
- Stateless framing, stateful lifecycle: the wire format is the same for every
  message, but the STM32 maintains a table of active sensors keyed by
  `sensor_id`
- Extensible: new protocols plug in by defining a new `protocol_id` and
  writing a payload spec -- the generic message types do not change

## Message Frame Format

```
+--------+--------+--------+--------+----------+--------+
| START  |  TYPE  |  LEN   |  SEQ   |  PAYLOAD |  CRC16 |
| 1 byte | 1 byte | 2 bytes| 1 byte | N bytes  | 2 bytes|
+--------+--------+--------+--------+----------+--------+
```

- **START**: Fixed sync byte `0xAA`
- **TYPE**: Message type identifier (see table below)
- **LEN**: Payload length in bytes (little-endian, max 256)
- **SEQ**: Sequence number for matching commands to ACKs (wraps at 255)
- **PAYLOAD**: Type-specific data -- format depends on TYPE and, for
  sensor-related commands, on the `protocol_id` inside the payload
- **CRC16**: CRC-16/CCITT over TYPE + LEN + SEQ + PAYLOAD

## Message Types

| Type ID | Name              | Direction     | Summary                                                   |
|---------|-------------------|---------------|-----------------------------------------------------------|
| 0x01    | CMD_SETUP_SENSOR  | BBB -> STM32  | Instantiate a sensor emulation on a chosen protocol       |
| 0x02    | CMD_SET_OUTPUT    | BBB -> STM32  | Update the value a running sensor emits                   |
| 0x03    | CMD_STOP_SENSOR   | BBB -> STM32  | Tear down a running sensor emulation                      |
| 0x04    | CMD_SCENARIO      | BBB -> STM32  | Load / start / stop a scenario segment (TBD)              |
| 0x05    | CMD_SYNC          | BBB <-> STM32 | Link synchronization / heartbeat                          |
| 0x10    | RSP_ACK           | STM32 -> BBB  | Unified response: carries status (success or error code)  |
| 0x11    | RSP_ERROR         | STM32 -> BBB  | Unsolicited runtime error report                          |
| 0x20    | STATUS_REPORT     | STM32 -> BBB  | Periodic status (uptime, active sensors, queue depth)     |

## Sensor Lifecycle

The core interaction model is a three-phase lifecycle per sensor:

```
       BBB                                          STM32
        |                                             |
        |---- CMD_SETUP_SENSOR (protocol + cfg) ----->|
        |                                             | validate, allocate
        |                                             | peripheral, initial
        |                                             | register state
        |<--- RSP_ACK (cmd=SETUP, err, sensor_id) ----|
        |          (err=0 -> sensor_id is live;       |
        |           err!=0 -> sensor_id is 0)         |
        |                                             |
        |---- CMD_SET_OUTPUT (sensor_id, value) ----->|  (repeatable)
        |<--- RSP_ACK (cmd=SET_OUTPUT, err, id) ------|
        |                                             |
        |---- CMD_STOP_SENSOR (sensor_id) ------------>|
        |<--- RSP_ACK (cmd=STOP, err, id) ------------|
```

Key points:

- A sensor exists on the STM32 from the moment it is successfully set up until
  it is stopped (or until the STM32 resets).
- Setup is **one-shot**: to change parameters of a running sensor, stop it
  and create a new one. This keeps setup-time checks (peripheral allocation,
  resource reservation) simple and deterministic.
- Between setup and stop, the STM32 holds the sensor's state (e.g. I2C
  register contents, DAC value, GPIO level). `CMD_SET_OUTPUT` updates that
  state; the emulated peripheral serves the updated state to the DUT using
  whatever physical timing the sensor protocol requires.
- The value the STM32 emits does not change until the next `CMD_SET_OUTPUT`
  arrives -- the BBB does not need to send periodic keep-alives.

## Sensor IDs

- Assigned by the STM32 on successful `CMD_SETUP_SENSOR`.
- 1 byte. Valid range: `0x01`..`0xFF`. `0x00` is reserved and never assigned.
- Unique across *all* active sensors on the STM32, regardless of protocol.
  An I2C sensor and a digital output cannot share an id.
- Not guaranteed to be stable across reboots or even across scenario runs --
  always re-discover via setup.
- Released when the sensor is stopped. IDs may be reused once freed.
- Maximum concurrent sensors: 255 in principle, in practice bounded by
  peripheral count and STM32 RAM.

## CMD_SETUP_SENSOR

Payload layout (generic framing -- inner bytes vary by protocol):

| Offset | Size | Name          | Description                                                          |
|--------|------|---------------|----------------------------------------------------------------------|
| 0      | 1    | `protocol_id` | Which emulation backend to use (see table below)                     |
| 1      | N    | `cfg`         | Protocol-specific configuration; see the corresponding spec document |

### Protocol IDs

| ID   | Protocol           | Spec                                        |
|------|--------------------|---------------------------------------------|
| 0x01 | I2C slave          | [i2c-sensors-spec.md](i2c-sensors-spec.md)  |
| 0x02 | SPI slave          | (TBD)                                       |
| 0x03 | Digital output     | (TBD)                                       |
| 0x04 | Digital input      | (TBD)                                       |
| 0x05 | Analog output (DAC)| (TBD)                                       |
| 0x06 | PWM output         | (TBD)                                       |
| 0x07 | Frequency output   | (TBD)                                       |
| 0x08 | 1-Wire slave       | (TBD)                                       |
| 0x09 | CAN frame source   | (TBD)                                       |

### Response

Unified `RSP_ACK` (see the "RSP_ACK" section below for the full layout).

- On success: `error_code = SUCCESS`, `sensor_id` = the newly allocated id.
  The sensor is live and addressable from the next message onward.
- On failure: `error_code` != `SUCCESS`, `sensor_id = 0x00`. No sensor is
  created; no id is allocated.

## CMD_SET_OUTPUT

Payload layout:

| Offset | Size | Name        | Description                                         |
|--------|------|-------------|-----------------------------------------------------|
| 0      | 1    | `sensor_id` | From the setup ACK                                  |
| 1      | N    | `value`     | Protocol-specific; see the corresponding spec file  |

The STM32 looks up the sensor, dispatches `value` to that sensor's protocol
backend, and replies with `RSP_ACK` once the state is committed. On success
the ACK echoes the command's `sensor_id`; on failure the ACK carries the
relevant `error_code` (e.g. `ERR_INVALID_SENSOR_ID` or a per-protocol code).

Semantics of "output" vary by protocol: for I2C it may be a block-write into
the emulated register map; for a DAC it is the new output code; for digital
output it is the new line level. Each protocol spec defines its own value
encoding.

## CMD_STOP_SENSOR

Payload layout:

| Offset | Size | Name        |
|--------|------|-------------|
| 0      | 1    | `sensor_id` |

On success the STM32 disables the emulation, releases the peripheral and
its pins, and frees the id for reuse. Response: `RSP_ACK` with
`error_code = SUCCESS` and the same `sensor_id` echoed back.

## RSP_ACK

Unified response: **every** command gets exactly one `RSP_ACK`, whether it
succeeded or failed. There is no separate `RSP_NACK` type -- the error code
field inside the ACK payload distinguishes success from failure.

### Payload layout

| Offset | Size | Name         | Description                                                            |
|--------|------|--------------|------------------------------------------------------------------------|
| 0      | 1    | `cmd_type`   | TYPE byte of the command being acknowledged (e.g. `0x01` for SETUP)    |
| 1      | 1    | `error_code` | `SUCCESS` = success; non-zero = failure (see error-code table below)   |
| 2      | 1    | `sensor_id`  | See "Sensor-id field semantics" below                                  |

Fixed payload size: **3 bytes**. `error_code` is expected to be
self-describing -- no per-message extra context is carried on the wire.

### Sensor-id field semantics

| Scenario                                                   | `sensor_id` value                     |
|------------------------------------------------------------|---------------------------------------|
| `CMD_SETUP_SENSOR`, success                                | Newly allocated id (`0x01`..`0xFF`)   |
| `CMD_SETUP_SENSOR`, failure                                | `0x00` (no sensor was created)        |
| `CMD_SET_OUTPUT` / `CMD_STOP_SENSOR`                       | Echo of the command's `sensor_id`     |
| `CMD_SYNC`                                                 | `0x00` (not sensor-scoped)            |
| `CMD_SCENARIO`                                             | `0x00` (not sensor-scoped)            |
| Any command that was rejected before its payload was read  | `0x00`                                |

A `sensor_id` of `0x00` in an ACK therefore always means either "the command
was not sensor-scoped" or "no sensor context applies".

### Why `cmd_type` is in the payload even though SEQ already pairs ACKs

The `SEQ` byte in the frame header is what the BBB uses to match an ACK to
its originating command (SEQ numbers are unique per outstanding command).
`cmd_type` in the ACK payload is redundant with that matching -- deliberately.
It lets the BBB sanity-check that the firmware interpreted the frame the way
it intended, catches byte-shift corruption that slipped past CRC, and makes
on-wire logs self-describing without needing to correlate to the command
stream. Cheap defense in depth (1 byte).

### Common error codes (0x00 - 0x3F)

| Code | Name                      | Meaning                                                            |
|------|---------------------------|--------------------------------------------------------------------|
| 0x00 | `SUCCESS`                 | Command executed successfully                                      |
| 0x01 | `ERR_UNKNOWN_COMMAND`     | TYPE byte not recognized                                           |
| 0x02 | `ERR_MALFORMED_PAYLOAD`   | Payload too short / bad layout for the TYPE                        |
| 0x03 | `ERR_BAD_CRC`             | CRC16 did not match (rarely returned -- usually dropped silently)  |
| 0x04 | `ERR_PROTOCOL_UNSUPPORTED`| `protocol_id` not implemented on this firmware                     |
| 0x05 | `ERR_OUT_OF_RESOURCES`    | No free sensor slots / RAM                                         |
| 0x06 | `ERR_INVALID_SENSOR_ID`   | Sensor id does not refer to an active sensor                       |
| 0x07 | `ERR_INVALID_PARAMETER`   | A generic field (not protocol-specific) had an illegal value       |
| 0x08 | `ERR_PERIPHERAL_BUSY`     | Requested peripheral (I2C1, SPI2, ...) already bound to another id |
| 0x09 | `ERR_PIN_CONFLICT`        | A requested pin is already in use by another active sensor         |
| 0x0A | `ERR_UNSUPPORTED_FEATURE` | Parameter is structurally valid but not implemented yet            |
| 0x0B | `ERR_INTERNAL`            | STM32 reached an unexpected state (firmware bug or hardware fault) |

Each protocol spec extends this table with its own error codes in the range
`0x40`..`0xBF` (e.g. I2C uses `0x40`..`0x5F`). The range `0xC0`..`0xFF` is
reserved for user/scenario-level errors.

The same code space is shared between `RSP_ACK.error_code` and
`RSP_ERROR.error_code` -- an unsolicited `RSP_ERROR` reporting "I2C bus
stuck" uses the same code the BBB would have seen in an ACK.

## RSP_ERROR

**Unsolicited** error report generated by the STM32 when something goes wrong
on an already-running sensor (internal bus collision, peripheral fault flagged
by hardware, etc.). It is **not** a response to a specific command and
therefore carries no `cmd_type`.

Payload:

| Offset | Size | Name         | Description                                        |
|--------|------|--------------|----------------------------------------------------|
| 0      | 1    | `sensor_id`  | `0x00` if the error is not sensor-scoped           |
| 1      | 1    | `error_code` | Same code space as `RSP_ACK.error_code`            |

## Synchronization

On startup, BBB sends `CMD_SYNC` every 100 ms until STM32 responds with
`RSP_ACK`. This establishes the communication link and resets sequence
counters on both sides.

## Error Handling

- If no ACK is received within a configurable timeout (default 50 ms), BBB
  retransmits.
- After 3 retransmits with no ACK, BBB logs an error and resets the link via
  `CMD_SYNC`.
- STM32 discards duplicate sequence numbers within a sliding window.
- STM32 silently drops frames with bad CRC (no NACK) -- retransmission is the
  BBB's responsibility once its timeout fires.
- STM32 silently drops frames with unknown START byte until resync.

## Notes

- All multi-byte fields are little-endian unless a per-protocol spec states
  otherwise.
- Maximum message size (header + payload + CRC): 263 bytes.
- Baud rate: 115200 default, configurable up to 921600.
- This spec will evolve -- version it alongside firmware releases.
