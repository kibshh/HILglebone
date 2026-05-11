"""HILglebone BBB-side wire protocol library.

Provides a complete encoder/decoder for the HILglebone serial protocol.
Import directly from this package — sub-module layout is an implementation detail.

Quick start
-----------
Sending a command::

    import serial
    from protocol import ProtocolParser, SeqCounter
    from protocol import FrameType, ProtocolId, ErrorCode
    from protocol import build_cmd_sync, build_cmd_setup_sensor, parse_ack
    from protocol import I2cSensorConfig, I2cAddressMode, pack_i2c_cfg

    port = serial.Serial("/dev/ttyUSB0", 115200, timeout=0.1)
    seq  = SeqCounter()
    parser = ProtocolParser()

    # Send SYNC
    port.write(build_cmd_sync(seq.next()))

    # Send I2C SETUP
    cfg = I2cSensorConfig(
        clock_hz=100_000,
        address_mode=I2cAddressMode.MODE_7BIT,
        primary_addr=0x48,
        register_count=2,
    )
    port.write(build_cmd_setup_sensor(seq.next(), ProtocolId.I2C, pack_i2c_cfg(cfg)))

    # Read response
    for event in parser.feed_bytes(port.read(64)):
        if isinstance(event, ParseError):
            print(f"parse error: {event.reason}")
            continue
        if event.type == FrameType.RSP_ACK:
            ack = parse_ack(event)
            if ack.ok:
                sensor_id = ack.sensor_id
"""

from .constants import (
    CRC_SIZE,
    FRAME_OVERHEAD,
    HEADER_SIZE,
    MAX_PAYLOAD_SIZE,
    START_BYTE,
    DigitalLevel,
    DigitalOutputType,
    DigitalPort,
    HwTimerId,
    DigitalPull,
    DigitalSpeed,
    DigitalTimerKind,
    PwmChannel,
    ErrorCode,
    FrameType,
    I2cAddressMode,
    I2cAutoIncMode,
    I2cFlag,
    I2cRegAddrEndian,
    I2cRegAddrWidth,
    ProtocolId,
)
from .crc import CRC16_INIT, crc16_ccitt, crc16_step
from .encoder import (
    SeqCounter,
    build_cmd_set_output,
    build_cmd_setup_sensor,
    build_cmd_stop_sensor,
    build_cmd_sync,
    encode_frame,
    parse_ack,
    parse_error_response,
)
from .digital_io import (
    DigitalOutConfig,
    pack_digital_out_cfg,
    pack_digital_out_set_output,
)
from .pwm import (
    PwmConfig,
    pack_pwm_cfg,
    pack_pwm_set_output,
    PWM_DUTY_MAX,
    PWM_FREQ_MAX_HZ,
    PWM_FREQ_MIN_HZ,
)
from .frame import AckResponse, ErrorResponse, Frame, ParseError, ParseErrorReason
from .i2c import I2cSensorConfig, pack_i2c_cfg, pack_i2c_set_output
from .parser import ProtocolParser

__all__ = [
    # Parser / decoder
    "ProtocolParser",
    # Frame types
    "Frame",
    "AckResponse",
    "ErrorResponse",
    "ParseError",
    "ParseErrorReason",
    # Encoder
    "SeqCounter",
    "encode_frame",
    "build_cmd_sync",
    "build_cmd_setup_sensor",
    "build_cmd_set_output",
    "build_cmd_stop_sensor",
    # Response decoders
    "parse_ack",
    "parse_error_response",
    # I2C backend
    "I2cSensorConfig",
    "pack_i2c_cfg",
    "pack_i2c_set_output",
    # Digital-output backend
    "DigitalOutConfig",
    "pack_digital_out_cfg",
    "pack_digital_out_set_output",
    # Constants
    "START_BYTE",
    "MAX_PAYLOAD_SIZE",
    "HEADER_SIZE",
    "CRC_SIZE",
    "FRAME_OVERHEAD",
    "FrameType",
    "ProtocolId",
    "ErrorCode",
    "I2cAddressMode",
    "I2cAutoIncMode",
    "I2cFlag",
    "I2cRegAddrEndian",
    "I2cRegAddrWidth",
    "DigitalPort",
    "DigitalOutputType",
    "DigitalSpeed",
    "DigitalPull",
    "DigitalLevel",
    "DigitalTimerKind",
    "HwTimerId",
    # PWM backend
    "PwmConfig",
    "pack_pwm_cfg",
    "pack_pwm_set_output",
    "PWM_DUTY_MAX",
    "PWM_FREQ_MIN_HZ",
    "PWM_FREQ_MAX_HZ",
    # PWM constants
    "PwmChannel",
    # CRC utilities
    "CRC16_INIT",
    "crc16_ccitt",
    "crc16_step",
]
