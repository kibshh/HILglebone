"""Digital-I/O backend payload builders.

Currently covers the digital-output protocol (`ProtocolId.DIGITAL_OUT`).
Layouts mirror the offset macros in stm32/include/digital_out_sensor.h
and are spec'd in docs/protocol/digital-io-spec.md.

All multi-byte fields are little-endian, matching the embedded side.

Usage:
    cfg = DigitalOutConfig(
        port=DigitalPort.B,
        pin=6,
        initial_level=DigitalLevel.HIGH,
        output_type=DigitalOutputType.OPEN_DRAIN,
        speed=DigitalSpeed.HIGH,
    )
    setup_payload = pack_digital_out_cfg(cfg)
    pulse_payload = pack_digital_out_set_output(level=DigitalLevel.LOW,
                                                pulse_us=5000)
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

from .constants import (
    DigitalLevel,
    DigitalOutputType,
    DigitalPort,
    DigitalPulseRange,
    DigitalPull,
    DigitalSpeed,
    DigitalTimerKind,
)

# Eight u8 fields, no padding.
_CFG_FMT: str = "<BBBBBBBB"
assert struct.calcsize(_CFG_FMT) == 8

# Per-sub-pool pulse caps. Mirrors PULSE_TIMER_HW_MAX_PULSE_US_{16,32} in
# stm32/include/digital_out_pulse_timer.h. Use these to size the
# `pulse_us` requests you'll send via CMD_SET_OUTPUT, depending on which
# DigitalPulseRange you chose at setup.
HW_PULSE_MAX_US_SHORT: int = 0xFFFF        # 16-bit slot cap
HW_PULSE_MAX_US_LONG:  int = 0xFFFF_FFFF   # 32-bit slot cap

# u8 level + u32 LE pulse_us.
_SET_OUTPUT_FMT: str = "<BI"
assert struct.calcsize(_SET_OUTPUT_FMT) == 5


@dataclass(frozen=True, slots=True)
class DigitalOutConfig:
    """Configuration for CMD_SETUP_SENSOR with protocol_id = DIGITAL_OUT.

    Mandatory fields:
        port:           GPIO port (A..E or H on the STM32F401RE).
        pin:            Pin number 0..15.

    Optional fields default to the safest cold-start state (push-pull,
    low-speed, no pull, idle-low).
    """

    port: DigitalPort
    pin: int
    initial_level: DigitalLevel       = DigitalLevel.LOW
    output_type: DigitalOutputType    = DigitalOutputType.PUSH_PULL
    speed: DigitalSpeed               = DigitalSpeed.LOW
    pull: DigitalPull                 = DigitalPull.NONE
    timer_kind: DigitalTimerKind      = DigitalTimerKind.SOFTWARE
    pulse_range: DigitalPulseRange    = DigitalPulseRange.SHORT

    def __post_init__(self) -> None:
        if not 0 <= self.pin <= 15:
            raise ValueError(f"pin must be 0..15, got {self.pin}")

        _enum_fields: list[tuple[object, type, str]] = [
            (self.port,          DigitalPort,        "port"),
            (self.initial_level, DigitalLevel,       "initial_level"),
            (self.output_type,   DigitalOutputType,  "output_type"),
            (self.speed,         DigitalSpeed,       "speed"),
            (self.pull,          DigitalPull,        "pull"),
            (self.timer_kind,    DigitalTimerKind,   "timer_kind"),
            (self.pulse_range,   DigitalPulseRange,  "pulse_range"),
        ]
        for value, enum_type, name in _enum_fields:
            if not isinstance(value, enum_type):
                raise ValueError(
                    f"{name} must be {enum_type.__name__}, got {value!r}"
                )


def pack_digital_out_cfg(config: DigitalOutConfig) -> bytes:
    """Serialise a DigitalOutConfig to the on-wire byte layout.

    The returned bytes are the `cfg` argument for
    build_cmd_setup_sensor() (i.e. the bytes after the protocol_id byte
    inside the CMD_SETUP_SENSOR payload).
    """
    return struct.pack(
        _CFG_FMT,
        int(config.port),
        config.pin,
        int(config.initial_level),
        int(config.output_type),
        int(config.speed),
        int(config.pull),
        int(config.timer_kind),
        int(config.pulse_range),
    )


def pack_digital_out_set_output(level: DigitalLevel, pulse_us: int = 0) -> bytes:
    """Build the digital-output payload for CMD_SET_OUTPUT.

    The returned bytes are the `values` argument for
    build_cmd_set_output() (i.e. the bytes after the sensor_id byte
    inside the CMD_SET_OUTPUT payload).

    Args:
        level:    Target level (0=low, 1=high).
        pulse_us: 0 = hold `level` indefinitely. Non-zero = drive
                  `level` for this many microseconds, then automatically
                  revert to !level. Rounded up to the next FreeRTOS tick
                  (typically 1 ms) by the firmware.

    Raises:
        ValueError: if pulse_us is negative or doesn't fit in u32.
    """
    if not isinstance(level, DigitalLevel):
        raise ValueError(f"level must be DigitalLevel, got {level!r}")
    if pulse_us < 0 or pulse_us > 0xFFFF_FFFF:
        raise ValueError(f"pulse_us must fit in u32, got {pulse_us}")
    return struct.pack(_SET_OUTPUT_FMT, int(level), pulse_us)
