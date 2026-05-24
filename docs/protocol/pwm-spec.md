# PWM Output Emulation Spec

Payload formats for the PWM output protocol (`protocol_id = 0x06`).
Plugs into the generic protocol in [protocol-spec.md](protocol-spec.md).

## 1. What this emulation provides

The STM32 drives a single GPIO pin with a hardware-PWM signal via an
on-chip general-purpose timer. From the DUT's perspective the pin looks
like a real PWM output: configurable frequency and duty cycle, driven
continuously until `CMD_STOP_SENSOR`.

**Typical uses:** fan tacho simulation, servo feedback, switching
regulator control signals, any PWM-encoded sensor output.

## 2. Hardware constraints (STM32F401RE)

| Constraint | Value |
|------------|-------|
| Supported timers | All 7 in the `hw_timer` pool (see table below) |
| Timer clock | 84 MHz for all timers |
| Max concurrent PWM sensors | 7 timers × 4 channels = 28 (some limited, see table) |
| Frequency range | 1 Hz .. 1 MHz |
| Duty cycle resolution | 0.01 % (0..10 000 = 0.00%..100.00%) |

### Available timers

| Wire value | Timer | Bus  | Counter width | Channels | Notes |
|-----------|-------|------|---------------|----------|-------|
| 0 | TIM2  | APB1 | **32-bit** | 4 | Best duty resolution at low freq |
| 1 | TIM3  | APB1 | 16-bit     | 4 | |
| 2 | TIM4  | APB1 | 16-bit     | 4 | |
| 3 | TIM5  | APB1 | **32-bit** | 4 | Best duty resolution at low freq |
| 4 | TIM9  | APB2 | 16-bit     | 2 | CH1 and CH2 only |
| 5 | TIM10 | APB2 | 16-bit     | 1 | CH1 only |
| 6 | TIM11 | APB2 | 16-bit     | 1 | CH1 only |

Timers TIM2/TIM5 and TIM3/TIM4 are shared with the digital-output pulse
timer pool. A timer allocated for PWM cannot simultaneously be used for
one-shot pulses, and vice-versa. The driver enforces this and returns
`ERR_PERIPHERAL_BUSY` if a conflict is attempted.

**TIM1** requires advanced-timer handling (MOE bit) and is not in the
pool.

### Timer sharing

Multiple sensors CAN share a timer (e.g. two sensors on TIM3 CH1 and
TIM3 CH2). Sensors on the same timer run at the **same frequency**
because the timer's auto-reload register (ARR) is shared. If a new
sensor requests a frequency that requires a different ARR than the one
already programmed on that timer, setup is rejected with
`ERR_PWM_FREQ_CONFLICT`.

### Alternate function (AF) pin mux

PWM channels reach GPIO pins via the alternate-function matrix. Common
mappings on the F401RE (AF2 for both TIM3 and TIM4):

| Timer | Channel | AF | Typical pins |
|-------|---------|----|--------------|
| TIM3  | CH1     | 2  | PA6, PC6     |
| TIM3  | CH2     | 2  | PA7, PC7     |
| TIM3  | CH3     | 2  | PB0, PC8     |
| TIM3  | CH4     | 2  | PB1, PC9     |
| TIM4  | CH1     | 2  | PB6, PD12    |
| TIM4  | CH2     | 2  | PB7, PD13    |
| TIM4  | CH3     | 2  | PB8, PD14    |
| TIM4  | CH4     | 2  | PB9, PD15    |

The `af` field in the setup payload must match the chosen pin's AF
entry in the STM32F401RE datasheet (Table 9, "Alternate function
mapping"). The firmware does not validate or auto-detect the AF number;
the BBB is responsible for providing the correct value.

## 3. CMD_SETUP_SENSOR — PWM payload (`protocol_id = 0x06`)

Bytes starting *after* the generic `protocol_id` byte:

| Offset | Size | Name              | Description |
|--------|------|-------------------|-------------|
| 0      | 1    | `port`            | GPIO port: `0`=A `1`=B `2`=C `3`=D `4`=E `5`=H |
| 1      | 1    | `pin`             | `0..15` |
| 2      | 1    | `af`              | Alternate function number `1..15` (pin-specific, see §2) |
| 3      | 1    | `timer`           | `0`=TIM3, `1`=TIM4 |
| 4      | 1    | `channel`         | `1..4` |
| 5      | 4    | `freq_hz`         | PWM frequency in Hz, u32 LE. Range: 1..1 000 000 |
| 9      | 2    | `duty_pct_x100`   | Initial duty cycle, u16 LE. `0..10000` (= 0.00%..100.00%) |

Total: **11 bytes** fixed.

### Response

Standard `RSP_ACK`. On success: `sensor_id` = newly allocated id. On
failure: non-zero `error_code`, `sensor_id = 0`.

## 4. CMD_SET_OUTPUT — PWM payload

Bytes starting *after* the generic `sensor_id` byte:

| Offset | Size | Name            | Description |
|--------|------|-----------------|-------------|
| 0      | 2    | `duty_pct_x100` | New duty cycle, u16 LE. `0..10000` |

Total: **2 bytes**.

The PWM frequency is fixed at setup and cannot be changed without
stopping and recreating the sensor. Only duty cycle is adjustable at
runtime. Changes take effect at the next timer update event (start of
the next PWM period) with no glitch.

## 5. CMD_STOP_SENSOR — PWM

No PWM-specific additions. Generic stop:

| Offset | Size | Name        |
|--------|------|-------------|
| 0      | 1    | `sensor_id` |

On STOP: the PWM channel is disabled, the pin reverts to Hi-Z floating
input, and the slot is freed. If this was the last active channel on
the timer, the timer itself is stopped and its clock gated.

## 6. Errors

| Code | Name | When |
|------|------|------|
| 0x02 | `ERR_MALFORMED_PAYLOAD`        | Payload shorter than 11 bytes (setup) or 2 bytes (set-output) |
| 0x07 | `ERR_INVALID_PARAMETER`    | `port`, `pin`, `af`, `timer`, `channel`, `freq_hz` or `duty_pct_x100` out of range |
| 0x09 | `ERR_PIN_CONFLICT`     | (port, pin) already owned by another sensor |
| 0x08 | `ERR_PERIPHERAL_BUSY`  | `timer` is currently allocated for one-shot pulses (digital-out sensor) |
| 0x60 | `ERR_PWM_FREQ_CONFLICT`| `timer` already running at a different frequency (sharing constraint) |
| 0x61 | `ERR_PWM_CHANNEL_IN_USE` | (timer, channel) pair already owned by another PWM sensor |

## 7. Worked example — 50 Hz servo signal

A standard RC servo expects a 50 Hz PWM signal; pulse width 1 ms (5%
duty) to 2 ms (10% duty) maps to the servo's range of motion.

### Setup

```
CMD_SETUP_SENSOR payload:
  protocol_id     = 0x06   (PWM)
  port            = 0x01   (B)
  pin             = 0x06   (PB6)
  af              = 0x02   (AF2 = TIM4_CH1 on PB6)
  timer           = 0x01   (TIM4)
  channel         = 0x01   (CH1)
  freq_hz         = 50     (LE: 32 00 00 00)
  duty_pct_x100   = 750    (7.50% = mid-travel, LE: EE 02)
```

### Drive to one end of travel (1 ms pulse = 5.00% at 50 Hz)

```
CMD_SET_OUTPUT:
  sensor_id       = <from setup ACK>
  duty_pct_x100   = 500    (5.00%, LE: F4 01)
```

### Drive to other end (2 ms pulse = 10.00%)

```
CMD_SET_OUTPUT:
  sensor_id       = <from setup ACK>
  duty_pct_x100   = 1000   (10.00%, LE: E8 03)
```

### Stop

```
CMD_STOP_SENSOR:
  sensor_id  = <from setup ACK>
```

PB6 reverts to Hi-Z.

## 8. Open questions / deferred

- **TIM1 support** — advanced timer, requires MOE (BDTR register).
- **Frequency change at runtime** — stopping and recreating the sensor
  is the current mechanism. A future extension could allow changing
  `freq_hz` via a new command or an extended `CMD_SET_OUTPUT` payload.
- **Phase alignment** — two channels on different timers cannot be
  phase-synchronised; channels on the same timer are inherently aligned.
