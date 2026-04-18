"""I2C backend payload builders for CMD_SETUP_SENSOR and CMD_SET_OUTPUT.

Payload layouts mirror the offset macros in stm32/include/i2c_sensor.h.
All multi-byte fields are little-endian, matching the embedded side's
read_u16_le / read_u32_le helpers.

Usage:
    cfg = I2cSensorConfig(
        clock_hz=100_000,
        address_mode=I2cAddressMode.MODE_7BIT,
        primary_addr=0x48,
        register_count=2,
    )
    setup_payload = pack_i2c_cfg(cfg)   # passed as `cfg` to build_cmd_setup_sensor()
    set_payload   = pack_i2c_set_output(reg_start=0x00, values=b'\\x1A\\x2B')
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

from .constants import (
    I2cAddressMode,
    I2cAutoIncMode,
    I2cFlag,
    I2cRegAddrEndian,
    I2cRegAddrWidth,
)

# ── Pack format for the 20-byte fixed I2C config header ───────────
#
# Fields and their byte widths (all little-endian):
#   clock_hz             u32  4   offset  0
#   address_mode         u8   1   offset  4
#   primary_addr         u16  2   offset  5
#   secondary_addr       u16  2   offset  7
#   flags                u8   1   offset  9
#   reg_addr_width       u8   1   offset 10
#   reg_addr_endian      u8   1   offset 11
#   auto_inc_mode        u8   1   offset 12
#   register_count       u16  2   offset 13
#   response_delay_us    u16  2   offset 15
#   clock_stretch_max_us u16  2   offset 17
#   has_preset           u8   1   offset 19
#                       ────  ──
#                        20 bytes  (== I2C_CFG_SIZE_NO_PRESET)
#
# '<' disables alignment padding so the layout is byte-exact.
_CFG_FIXED_FMT: str = "<IBHHBBBBHHHB"
assert struct.calcsize(_CFG_FIXED_FMT) == 20


@dataclass(frozen=True, slots=True)
class I2cSensorConfig:
    """Configuration for CMD_SETUP_SENSOR with protocol_id = ProtocolId.I2C.

    Mandatory fields:
        clock_hz:      I2C bus clock in Hz (10_000..400_000).
        address_mode:  7-bit or 10-bit addressing.
        primary_addr:  Device address on the bus.

    Optional fields default to sensible zero/none values matching the
    embedded struct's zero-initialised state.

    Preset:
        If preset_values is set, preset_reg_start must also be set.
        A preset pre-loads the sensor's register map on SETUP so the DUT
        can read initial values before the first SET_OUTPUT.
    """

    clock_hz: int
    address_mode: I2cAddressMode
    primary_addr: int
    secondary_addr: int               = 0
    flags: I2cFlag                    = I2cFlag(0)
    reg_addr_width: I2cRegAddrWidth   = I2cRegAddrWidth.NONE
    reg_addr_endian: I2cRegAddrEndian = I2cRegAddrEndian.BIG
    auto_inc_mode: I2cAutoIncMode     = I2cAutoIncMode.NONE
    register_count: int               = 0
    response_delay_us: int            = 0
    clock_stretch_max_us: int         = 0
    preset_reg_start: int | None      = None
    preset_values: bytes | None       = None

    def __post_init__(self) -> None:
        has_values = self.preset_values is not None
        has_start  = self.preset_reg_start is not None
        if has_values != has_start:
            raise ValueError(
                "preset_reg_start and preset_values must both be provided or both be None"
            )


# ── Payload packers ────────────────────────────────────────────────

def pack_i2c_cfg(config: I2cSensorConfig) -> bytes:
    """Serialise an I2cSensorConfig to the on-wire byte layout.

    The returned bytes are the `cfg` argument for build_cmd_setup_sensor()
    (i.e. the bytes after the protocol_id byte inside the CMD_SETUP_SENSOR
    payload).
    """
    has_preset = config.preset_values is not None

    fixed = struct.pack(
        _CFG_FIXED_FMT,
        config.clock_hz,
        int(config.address_mode),
        config.primary_addr,
        config.secondary_addr,
        int(config.flags),
        int(config.reg_addr_width),
        int(config.reg_addr_endian),
        int(config.auto_inc_mode),
        config.register_count,
        config.response_delay_us,
        config.clock_stretch_max_us,
        1 if has_preset else 0,
    )

    if not has_preset:
        return fixed

    preset_values     = config.preset_values  # type: ignore[assignment]  # guarded above
    preset_reg_start  = config.preset_reg_start  # type: ignore[assignment]
    preset_header     = struct.pack("<HH", preset_reg_start, len(preset_values))
    return fixed + preset_header + preset_values


def pack_i2c_set_output(reg_start: int, values: bytes) -> bytes:
    """Build the I2C-specific payload for CMD_SET_OUTPUT.

    The returned bytes are the `values` argument for build_cmd_set_output()
    (i.e. the bytes after the sensor_id byte inside the CMD_SET_OUTPUT payload).

    Wire layout (mirrors I2C_SET_OUTPUT_OFFSET_* in i2c_sensor.h):
        reg_start  u16 LE   offset 0
        value_len  u16 LE   offset 2
        values     N bytes  offset 4

    Raises:
        ValueError: if values is empty.
    """
    if not values:
        raise ValueError("values must be non-empty")
    return struct.pack("<HH", reg_start, len(values)) + values
