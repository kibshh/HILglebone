"""DAC (analog output) backend payload builders.

Covers the DAC protocol (`ProtocolId.DAC`, protocol_id = 0x05).
Targets a TI DAC8568 8-channel 16-bit DAC connected to the STM32 over SPI.
Layouts mirror the offset macros in stm32/project/simulate/dac/inc/dac_sensor.h
and are spec'd in docs/protocol/dac-spec.md.

All multi-byte fields are little-endian.

Usage:
    cfg = DacConfig(
        spi_periph=SpiPeripheral.SPI1,
        spi_clock_hz=10_000_000,
        mosi_port=DigitalPort.A, mosi_pin=7, mosi_af=5,
        sck_port=DigitalPort.A,  sck_pin=5, sck_af=5,
        cs_port=DigitalPort.A,   cs_pin=4,
        channel=DacChannel.A,
        reference=DacReference.INTERNAL_STATIC,
        initial_value=0x8000,    # 1.25 V at 2.5 V Vref
    )
    setup_payload = pack_dac_cfg(cfg)
    value_payload = pack_dac_set_output(0x3333)   # ~0.5 V
"""
from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Optional

from .constants import (
    DacChannel,
    DacReference,
    DigitalPort,
    SpiMode,
    SpiPeripheral,
)

# u8 spi_periph + u32 spi_clock_hz + 13× u8 + u16 initial_value = 20 bytes
_CFG_FMT: str = "<BIBBBBBBBBBBBBBH"
assert struct.calcsize(_CFG_FMT) == 20

# u16 value
_SET_OUTPUT_FMT: str = "<H"
assert struct.calcsize(_SET_OUTPUT_FMT) == 2

# Sentinel port value meaning "LDAC is tied to GND on the board".
LDAC_DISABLED_PORT: int = 0xFF


@dataclass(frozen=True, slots=True)
class DacConfig:
    """Configuration for CMD_SETUP_SENSOR with protocol_id = DAC.

    Mandatory fields:
        spi_periph:    SPI peripheral (SPI1 or SPI2).
        spi_clock_hz:  Target SPI baud rate in Hz.
        mosi_port / mosi_pin / mosi_af:  MOSI GPIO and its AF number.
        sck_port  / sck_pin  / sck_af:   SCK  GPIO and its AF number.
        cs_port   / cs_pin:              CS (SYNC) GPIO — software-driven.
        channel:       DAC8568 output channel (A..H).

    Optional fields:
        ldac_port / ldac_pin:  LDAC GPIO. If not provided, LDAC is assumed
                               tied to GND on the board (transparent mode —
                               updates take effect at end of SPI transfer).
        reference:     Internal reference selection (default = INTERNAL_STATIC).
        spi_mode:      SPI clock phase/polarity (default = MODE_1).
        initial_value: DAC code loaded during setup (default = 0, i.e. 0 V).
    """

    spi_periph:    SpiPeripheral
    spi_clock_hz:  int
    mosi_port:     DigitalPort
    mosi_pin:      int
    mosi_af:       int
    sck_port:      DigitalPort
    sck_pin:       int
    sck_af:        int
    cs_port:       DigitalPort
    cs_pin:        int
    channel:       DacChannel
    ldac_port:     Optional[DigitalPort] = None
    ldac_pin:      int                   = 0
    reference:     DacReference          = DacReference.INTERNAL_STATIC
    spi_mode:      SpiMode               = SpiMode.MODE_1   # DAC8568 default
    initial_value: int                   = 0

    def __post_init__(self) -> None:
        for pin_name, pin_val in (("mosi_pin", self.mosi_pin),
                                  ("sck_pin",  self.sck_pin),
                                  ("cs_pin",   self.cs_pin)):
            if not 0 <= pin_val <= 15:
                raise ValueError(f"{pin_name} must be 0..15, got {pin_val}")
        for af_name, af_val in (("mosi_af", self.mosi_af),
                                ("sck_af",  self.sck_af)):
            if not 1 <= af_val <= 15:
                raise ValueError(f"{af_name} must be 1..15, got {af_val}")
        if self.ldac_port is not None and not 0 <= self.ldac_pin <= 15:
            raise ValueError(f"ldac_pin must be 0..15, got {self.ldac_pin}")
        if not 0 <= self.initial_value <= 0xFFFF:
            raise ValueError(
                f"initial_value must be 0..65535, got {self.initial_value}"
            )
        for value, enum_type, name in (
            (self.spi_periph, SpiPeripheral, "spi_periph"),
            (self.mosi_port,  DigitalPort,   "mosi_port"),
            (self.sck_port,   DigitalPort,   "sck_port"),
            (self.cs_port,    DigitalPort,   "cs_port"),
            (self.channel,    DacChannel,    "channel"),
            (self.reference,  DacReference,  "reference"),
            (self.spi_mode,   SpiMode,       "spi_mode"),
        ):
            if not isinstance(value, enum_type):
                raise ValueError(
                    f"{name} must be {enum_type.__name__}, got {value!r}"
                )
        if self.ldac_port is not None and not isinstance(self.ldac_port, DigitalPort):
            raise ValueError(
                f"ldac_port must be DigitalPort or None, got {self.ldac_port!r}"
            )


def pack_dac_cfg(config: DacConfig) -> bytes:
    """Serialise a DacConfig to the on-wire byte layout (20 bytes).

    The returned bytes are the `cfg` argument for
    build_cmd_setup_sensor() (bytes after the protocol_id byte).
    """
    ldac_port_byte = (
        int(config.ldac_port) if config.ldac_port is not None
        else LDAC_DISABLED_PORT
    )
    ldac_pin_byte = config.ldac_pin if config.ldac_port is not None else 0

    return struct.pack(
        _CFG_FMT,
        int(config.spi_periph),
        config.spi_clock_hz,
        int(config.mosi_port),
        config.mosi_pin,
        config.mosi_af,
        int(config.sck_port),
        config.sck_pin,
        config.sck_af,
        int(config.cs_port),
        config.cs_pin,
        ldac_port_byte,
        ldac_pin_byte,
        int(config.channel),
        int(config.reference),
        int(config.spi_mode),
        config.initial_value,
    )


def pack_dac_set_output(value: int) -> bytes:
    """Build the DAC payload for CMD_SET_OUTPUT.

    The returned bytes are the `values` argument for
    build_cmd_set_output() (bytes after the sensor_id byte).

    Args:
        value: DAC code 0..65535. Maps to 0 V .. V_REF on the channel.

    Raises:
        ValueError: if value is out of range.
    """
    if not 0 <= value <= 0xFFFF:
        raise ValueError(f"value must be 0..65535, got {value}")
    return struct.pack(_SET_OUTPUT_FMT, value)
