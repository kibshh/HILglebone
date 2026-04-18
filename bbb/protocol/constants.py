"""Wire-protocol constants mirroring stm32/include/protocol.h and i2c_sensor.h.

Every on-the-wire number lives here so the rest of the library never hard-codes
byte values. Add new protocol IDs or error codes here first; the encoders and
decoders pick them up automatically through IntEnum membership.
"""
from __future__ import annotations

from enum import IntEnum, IntFlag

# ── Frame framing ──────────────────────────────────────────────────

START_BYTE: int = 0xAA
MAX_PAYLOAD_SIZE: int = 256
HEADER_SIZE: int = 5        # START + TYPE + LEN(2) + SEQ
CRC_SIZE: int = 2
FRAME_OVERHEAD: int = HEADER_SIZE + CRC_SIZE  # 7

# ── Message types ──────────────────────────────────────────────────

class FrameType(IntEnum):
    CMD_SETUP_SENSOR = 0x01
    CMD_SET_OUTPUT   = 0x02
    CMD_STOP_SENSOR  = 0x03
    CMD_SCENARIO     = 0x04
    CMD_SYNC         = 0x05
    RSP_ACK          = 0x10
    RSP_ERROR        = 0x11
    STATUS_REPORT    = 0x20

# ── Protocol IDs (inside CMD_SETUP_SENSOR payload) ─────────────────

class ProtocolId(IntEnum):
    NONE        = 0x00
    I2C         = 0x01
    SPI         = 0x02
    DIGITAL_OUT = 0x03
    DIGITAL_IN  = 0x04
    DAC         = 0x05
    PWM         = 0x06
    FREQ        = 0x07
    ONEWIRE     = 0x08
    CAN         = 0x09

# ── Common error codes (0x00..0x3F) ───────────────────────────────

class ErrorCode(IntEnum):
    SUCCESS              = 0x00
    UNKNOWN_COMMAND      = 0x01
    MALFORMED_PAYLOAD    = 0x02
    BAD_CRC              = 0x03
    PROTOCOL_UNSUPPORTED = 0x04
    OUT_OF_RESOURCES     = 0x05
    INVALID_SENSOR_ID    = 0x06
    INVALID_PARAMETER    = 0x07
    PERIPHERAL_BUSY      = 0x08
    PIN_CONFLICT         = 0x09
    UNSUPPORTED_FEATURE  = 0x0A
    INTERNAL             = 0x0B
    # I2C-specific (0x40..0x5F)
    I2C_NO_FREE_PERIPHERAL  = 0x40
    I2C_CLOCK_UNSUPPORTED   = 0x41
    I2C_ADDR_CONFLICT       = 0x42
    I2C_ADDR_RESERVED       = 0x43
    I2C_REGMAP_TOO_LARGE    = 0x44
    I2C_BAD_ADDR_MODE       = 0x45
    I2C_REGISTER_OOB        = 0x46
    I2C_STRETCH_EXCEEDS_BUS = 0x47
    I2C_SMBUS_REQUIRED      = 0x48
    I2C_UNSUPPORTED_FEATURE = 0x49

# ── I2C field enumerants ───────────────────────────────────────────

class I2cAddressMode(IntEnum):
    MODE_7BIT  = 0
    MODE_10BIT = 1

class I2cRegAddrWidth(IntEnum):
    NONE = 0
    W8   = 1
    W16  = 2

class I2cRegAddrEndian(IntEnum):
    BIG    = 0
    LITTLE = 1

class I2cAutoIncMode(IntEnum):
    NONE  = 0
    READ  = 1
    WRITE = 2
    BOTH  = 3

class I2cFlag(IntFlag):
    GENERAL_CALL_ENABLE  = 1 << 0
    SMBUS_MODE           = 1 << 1
    PEC_REQUIRED         = 1 << 2
    AUTO_INC_WRAP        = 1 << 3
    DUT_WRITES_ALLOWED   = 1 << 4
    INTERNAL_PULLUPS     = 1 << 5
    CLOCK_STRETCH_ENABLE = 1 << 6
