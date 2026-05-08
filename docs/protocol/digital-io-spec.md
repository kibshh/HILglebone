# Digital I/O Emulation Spec

Payload formats used when the BeagleBone Black asks the STM32 to emulate
a digital output (or, eventually, a digital input). Plugs into the
generic protocol defined in [protocol-spec.md](protocol-spec.md) — the
framing, sequencing, CRC, unified `RSP_ACK` semantics, and `sensor_id`
lifecycle are inherited from there.

This spec covers `protocol_id = 0x03` (digital output) only. Digital
input (`protocol_id = 0x04`) is reserved and will be added later.

## 1. What this emulation provides

The STM32 drives a single GPIO pin as the emulated sensor's output.
From the DUT's perspective the pin behaves like a discrete signal line
on a real sensor (a `DRDY` flag, an interrupt line, an enable line, a
button, etc.). The BBB controls the line level — and optionally
schedules pulses — via `CMD_SET_OUTPUT`.

What's configurable at setup time:

- Pin assignment (port + pin)
- Initial level (idle high or idle low at the moment the peripheral
  comes up)
- Output type (push-pull vs open-drain)
- Slew rate / drive strength
- Internal pull-up / pull-down

What's controllable at run time:

- New level (instant)
- Pulse: drive `level` for `pulse_us` microseconds, then revert to the
  opposite level. `pulse_us = 0` means "hold the new level
  indefinitely".

### Out of scope (v1)

- DMA-driven waveforms / arbitrary timing patterns. Use PWM
  (`protocol_id = 0x06`) for periodic signals.
- Synchronisation with another sensor's edges (e.g. "pulse on next I2C
  read"). Will be revisited together with fault injection.
- Sub-microsecond pulses. Pulses are scheduled via FreeRTOS software
  timers, so the resolution is one tick (1 ms by default). Hardware-
  timer-backed pulses are a future improvement.

## 2. Hardware constraints (STM32F401RE)

| Constraint                       | Value                                                                  |
|----------------------------------|------------------------------------------------------------------------|
| Max concurrent digital outputs   | 8 (shared with the global sensor-slot cap)                             |
| Available ports                  | A, B, C, D, E, H (encoded 0..5; F and G don't exist on F401)           |
| Pin range                        | 0..15                                                                  |
| Pulse timing — software timer    | ~1 ms resolution (FreeRTOS tick), unbounded duration                   |
| Pulse timing — hardware timer    | 1 µs resolution; per-slot cap (see below)                              |
| Hardware-timer pool size         | 5 slots: 2× 32-bit (TIM2, TIM5) + 3× 16-bit (TIM9, TIM10, TIM11)        |
| Hardware-timer max pulse (32-bit slot) | 4 294 967 295 µs (~71 minutes)                                    |
| Hardware-timer max pulse (16-bit slot) | 65 535 µs (~65 ms)                                                |
| Logic voltage                    | 3.3 V                                                                  |

A pin already claimed by a peripheral in firmware (UART1 PA9/PA10, the
heartbeat LED PA5, future I2C/SPI pins, etc.) will technically be
overridden by digital-output setup; the BBB is responsible for not
collision. Inter-digital-output collisions are detected by the firmware
and rejected with `ERR_PIN_CONFLICT`.

## 3. CMD_SETUP_SENSOR — digital output payload (`protocol_id = 0x03`)

Bytes starting *after* the generic `protocol_id` byte:

| Offset | Size | Name            | Description                                                         |
|--------|------|-----------------|---------------------------------------------------------------------|
| 0      | 1    | `port`          | `0`=A `1`=B `2`=C `3`=D `4`=E `5`=H                                 |
| 1      | 1    | `pin`           | `0..15`                                                             |
| 2      | 1    | `initial_level` | `0`=low, `1`=high. Applied **before** the pin leaves Hi-Z           |
| 3      | 1    | `output_type`   | `0`=push-pull, `1`=open-drain                                       |
| 4      | 1    | `speed`         | `0`=low, `1`=medium, `2`=high, `3`=very-high (slew rate / drive)    |
| 5      | 1    | `pull`          | `0`=none, `1`=pull-up, `2`=pull-down                                |
| 6      | 1    | `timer_kind`    | `0`=software (FreeRTOS, ~1 ms res, unbounded), `1`=hardware (1 µs res; cap depends on `pulse_range`) |
| 7      | 1    | `pulse_range`   | Only consulted when `timer_kind = 1`. `0`=short (16-bit slot, ≤ 65 535 µs), `1`=long (32-bit slot, ≤ ~71 min). Ignored for software. |

Total: **8 bytes** fixed.

### `timer_kind`

Pinned for the lifetime of the sensor. Affects only `pulse_us` in
`CMD_SET_OUTPUT`; an instantaneous level change (`pulse_us = 0`) is the
same regardless of kind.

- `0` (software): pulses are scheduled by a per-sensor FreeRTOS
  software timer. Tick-quantised (~1 ms), but unlimited duration and no
  hardware pool to share. `pulse_range` is ignored (must still be a
  valid value 0 or 1).
- `1` (hardware): the firmware reserves one of the dedicated timers
  at setup time and pins it to this sensor until stop. 1 µs
  resolution, deterministic jitter. The pool is split into two
  independent sub-pools, and the BBB picks which sub-pool to draw from
  via `pulse_range`:

  | `pulse_range` | Slot count | Timers       | Per-pulse cap                |
  |---------------|------------|--------------|------------------------------|
  | `0` (short)   | 3          | TIM9 / TIM10 / TIM11 | 65 535 µs (~65 ms)   |
  | `1` (long)    | 2          | TIM2 / TIM5  | 4 294 967 295 µs (~71 min)   |

  Allocation is **strict**: if the BBB asks for `short` and no 16-bit
  slot is free, the firmware does **not** fall back to a 32-bit slot
  -- it returns `ERR_OUT_OF_RESOURCES`. Same in the other direction.
  This makes the choice deterministic for the caller and keeps the
  scarcer 32-bit slots reserved for sensors that actually need long
  pulses.

### `pulse_range`

The BBB picks based on its needs:

- Pulses ≤ 65 ms (e.g. DRDY/INT lines, button presses) → `0` (short).
- Pulses > 65 ms (e.g. enable lines held for seconds, long debounce
  windows) → `1` (long).

If you don't know the maximum pulse length in advance, pick the
smallest range that comfortably covers the worst case so the larger
slots stay free for sensors that genuinely need them.

### Response

Standard `RSP_ACK`. On success: `error_code = SUCCESS`, `sensor_id` =
the newly allocated id. On failure: a non-zero error_code and
`sensor_id = 0`.

## 4. CMD_SET_OUTPUT — digital output payload

Bytes starting *after* the generic `sensor_id` byte:

| Offset | Size | Name        | Description                                                          |
|--------|------|-------------|----------------------------------------------------------------------|
| 0      | 1    | `level`     | Target level: `0`=low, `1`=high                                      |
| 1      | 4    | `pulse_us`  | u32 LE. `0` = hold `level` indefinitely. `>0` = drive `level` for    |
|        |      |             | this many µs, then automatically revert to `!level`                  |

Total: **5 bytes**.

`pulse_us` behaviour depends on `timer_kind` chosen at setup:

- **Software**: rounded up to the next FreeRTOS tick. With the default
  1 ms tick, requests under 1000 µs round up to 1 ms. No upper bound.
- **Hardware**: 1 µs resolution, capped by the `pulse_range` chosen at
  setup.
  - `pulse_range = 1` (long, 32-bit slot): up to `4 294 967 295 µs`
    (~71 min).
  - `pulse_range = 0` (short, 16-bit slot): up to `65 535 µs` (~65 ms).
  - Requests beyond the slot's cap are rejected with
    `ERR_INVALID_PARAMETER`.

If a previous pulse is still in flight when a new `CMD_SET_OUTPUT`
arrives, the old pulse is cancelled and the new request takes effect
immediately. There is no queue.

## 5. CMD_STOP_SENSOR — digital output

No protocol-specific additions. Generic stop applies:

| Offset | Size | Name        |
|--------|------|-------------|
| 0      | 1    | `sensor_id` |

On STOP the firmware:

1. Cancels any in-flight pulse.
2. Reverts the pin to floating input (Hi-Z).
3. Disables internal pull-ups/downs on that pin.
4. Frees the sensor slot and the pin reservation.

## 6. Errors

The minimal version uses common error codes only — no protocol-specific
slice yet:

| Code | Name                       | When it fires                                                  |
|------|----------------------------|----------------------------------------------------------------|
| 0x02 | `ERR_MALFORMED_PAYLOAD`    | Setup or set-output payload too short                          |
| 0x05 | `ERR_OUT_OF_RESOURCES`     | All 8 digital-output slots already in use, or `timer_kind=hardware` and the chosen `pulse_range` sub-pool (3 short slots, 2 long slots) is exhausted |
| 0x07 | `ERR_INVALID_PARAMETER`    | `port`, `pin`, `initial_level`, `output_type`, `speed`, `pull`, `timer_kind`, `pulse_range` or `level` out of range; or HW pulse exceeds the slot's per-pulse cap |
| 0x09 | `ERR_PIN_CONFLICT`         | The (port, pin) tuple is already owned by another digital-output sensor |

## 7. Worked example — DRDY pulse line

A sensor that pulses a `DRDY` line low for ~5 ms whenever a new sample
becomes available. Wire it to PB6 with an external pull-up:

### Setup

```
CMD_SETUP_SENSOR payload:
  protocol_id     = 0x03   (digital output)
  port            = 0x01   (B)
  pin             = 0x06
  initial_level   = 0x01   (idle high; sensor pulses low)
  output_type     = 0x01   (open-drain — DUT side has the pull-up)
  speed           = 0x02   (high)
  pull            = 0x00   (none)
  timer_kind      = 0x01   (hardware — DRDY pulse needs precise 5 ms)
  pulse_range     = 0x00   (short — 5 ms fits comfortably in a 16-bit slot)
```

STM32 responds with `RSP_ACK` carrying the new `sensor_id` (e.g. `0x01`).

### Generate one DRDY pulse (active-low, 5 ms)

```
CMD_SET_OUTPUT payload:
  sensor_id  = 0x01
  level      = 0x00       (drive low)
  pulse_us   = 5000       (then revert to high)
```

STM32 ACKs immediately. The pin goes low, stays low ≈ 5 ms, then snaps
back to high. The BBB can fire as many of these as the simulation needs.

### Teardown

```
CMD_STOP_SENSOR:
  sensor_id  = 0x01
```

PB6 returns to Hi-Z floating input.

## 8. Open questions / deferred

- **Digital input** (`protocol_id = 0x04`) — read DUT-driven pin state
  back over UART, with optional edge-triggered notifications.
- **Periodic pulse train** — N pulses with configurable spacing,
  scheduled in firmware so the BBB doesn't have to stream `CMD_SET_OUTPUT`
  at the pulse rate.
- **Pin reservation registry** that's aware of UART/I2C/SPI pins, so
  conflict detection covers cross-protocol clashes (today only digital-
  output ↔ digital-output is checked).
