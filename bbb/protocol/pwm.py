"""PWM backend payload builders for CMD_SETUP_SENSOR and CMD_SET_OUTPUT.

Layouts mirror the offset macros in stm32/project/simulate/pwm/inc/pwm_sensor.h
and are spec'd in docs/protocol/pwm-spec.md.

All multi-byte fields are little-endian.

Usage:
    cfg = PwmConfig(
        port=DigitalPort.B,
        pin=6,
        af=2,                       # AF2 = TIM4_CH1 on PB6
        timer=HwTimerId.TIM4,
        channel=PwmChannel.CH1,
        freq_hz=50,                 # 50 Hz servo
        duty_pct_x100=750,          # 7.50% = mid-travel
    )
    setup_payload = pack_pwm_cfg(cfg)
    duty_payload  = pack_pwm_set_output(duty_pct_x100=500)  # 5.00%
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

from .constants import DigitalPort, HwTimerId, PwmChannel

# u8 port + u8 pin + u8 af + u8 timer + u8 channel + u32 freq_hz + u16 duty
_CFG_FMT: str = "<BBBBBIH"
assert struct.calcsize(_CFG_FMT) == 11

# u16 duty_pct_x100
_SET_OUTPUT_FMT: str = "<H"
assert struct.calcsize(_SET_OUTPUT_FMT) == 2

PWM_FREQ_MIN_HZ: int = 1
PWM_FREQ_MAX_HZ: int = 1_000_000
PWM_DUTY_MAX:    int = 10_000      # 100.00 %


@dataclass(frozen=True, slots=True)
class PwmConfig:
    """Configuration for CMD_SETUP_SENSOR with protocol_id = DIGITAL_OUT.

    Mandatory fields:
        port:    GPIO port.
        pin:     Pin number 0..15.
        af:      Alternate-function number (1..15, pin-specific).
                 TIM3 and TIM4 channels typically use AF2 on F401RE.
        timer:   Which timer to use (TIM3 or TIM4).
        channel: Timer channel (CH1..CH4).
        freq_hz: PWM frequency in Hz (1..1 000 000).

    Optional:
        duty_pct_x100: Initial duty cycle in 0.01 % steps (default 0).
                       0 = 0.00 %, 10 000 = 100.00 %.
    """

    port: DigitalPort
    pin: int
    af: int
    timer: HwTimerId
    channel: PwmChannel
    freq_hz: int
    duty_pct_x100: int = 0

    def __post_init__(self) -> None:
        if not 0 <= self.pin <= 15:
            raise ValueError(f"pin must be 0..15, got {self.pin}")
        if not 1 <= self.af <= 15:
            raise ValueError(f"af must be 1..15, got {self.af}")
        if not isinstance(self.port, DigitalPort):
            raise ValueError(f"port must be DigitalPort, got {self.port!r}")
        if not isinstance(self.timer, HwTimerId):
            raise ValueError(f"timer must be HwTimerId, got {self.timer!r}")
        if not isinstance(self.channel, PwmChannel):
            raise ValueError(f"channel must be PwmChannel, got {self.channel!r}")
        if not PWM_FREQ_MIN_HZ <= self.freq_hz <= PWM_FREQ_MAX_HZ:
            raise ValueError(
                f"freq_hz must be {PWM_FREQ_MIN_HZ}..{PWM_FREQ_MAX_HZ}, "
                f"got {self.freq_hz}"
            )
        if not 0 <= self.duty_pct_x100 <= PWM_DUTY_MAX:
            raise ValueError(
                f"duty_pct_x100 must be 0..{PWM_DUTY_MAX}, "
                f"got {self.duty_pct_x100}"
            )


def pack_pwm_cfg(config: PwmConfig) -> bytes:
    """Serialise a PwmConfig to the on-wire byte layout.

    The returned bytes are the `cfg` argument for
    build_cmd_setup_sensor() (bytes after the protocol_id byte).
    """
    return struct.pack(
        _CFG_FMT,
        int(config.port),
        config.pin,
        config.af,
        int(config.timer),
        int(config.channel),
        config.freq_hz,
        config.duty_pct_x100,
    )


def pack_pwm_set_output(duty_pct_x100: int) -> bytes:
    """Build the PWM payload for CMD_SET_OUTPUT.

    The returned bytes are the `values` argument for
    build_cmd_set_output() (bytes after the sensor_id byte).

    Args:
        duty_pct_x100: New duty cycle in 0.01 % steps (0..10 000).

    Raises:
        ValueError: if duty_pct_x100 is out of range.
    """
    if not 0 <= duty_pct_x100 <= PWM_DUTY_MAX:
        raise ValueError(
            f"duty_pct_x100 must be 0..{PWM_DUTY_MAX}, got {duty_pct_x100}"
        )
    return struct.pack(_SET_OUTPUT_FMT, duty_pct_x100)
