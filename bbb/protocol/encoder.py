"""Frame encoder, generic command builders, response decoders, and SeqCounter.

Encoding mirrors stm32/src/protocol/protocol_encoder.c:
  - CRC covers TYPE + LEN(2) + SEQ + PAYLOAD (everything between START and CRC).
  - Frame layout: START(1) + TYPE(1) + LEN_LO(1) + LEN_HI(1) + SEQ(1)
                  + PAYLOAD(N) + CRC_LO(1) + CRC_HI(1).

Callers are responsible for sequence number management; SeqCounter is provided
as a convenience but is deliberately not hidden inside these functions.
"""
from __future__ import annotations

from .constants import (
    FRAME_OVERHEAD,
    MAX_PAYLOAD_SIZE,
    START_BYTE,
    FrameType,
    ProtocolId,
)
from .crc import CRC16_INIT, crc16_ccitt
from .frame import AckResponse, ErrorResponse, Frame

# RSP_ACK and RSP_ERROR payload sizes (from protocol.h).
_RSP_ACK_SIZE: int   = 3
_RSP_ERROR_SIZE: int = 2


# ── Low-level frame builder ────────────────────────────────────────

def encode_frame(
    frame_type: int,
    seq: int,
    payload: bytes = b"",
) -> bytes:
    """Build a complete wire frame ready to write to the serial port.

    Args:
        frame_type: FrameType value.
        seq:        Sequence number, 0..255.
        payload:    Payload bytes; default empty.

    Returns:
        Framed bytes: START + TYPE + LEN(2) + SEQ + PAYLOAD + CRC(2).

    Raises:
        ValueError: if frame_type is not a known FrameType, seq is out of
                    range, or payload exceeds MAX_PAYLOAD_SIZE.
    """
    if frame_type not in FrameType.__members__.values():
        raise ValueError(
            f"unknown frame_type {frame_type:#04x} — add it to FrameType if intentional"
        )
    if not 0 <= seq <= 0xFF:
        raise ValueError(f"seq {seq} out of range 0..255")
    if len(payload) > MAX_PAYLOAD_SIZE:
        raise ValueError(
            f"payload length {len(payload)} exceeds maximum {MAX_PAYLOAD_SIZE}"
        )

    length = len(payload)
    header = bytes([
        START_BYTE,
        frame_type & 0xFF,
        length & 0xFF,
        (length >> 8) & 0xFF,
        seq & 0xFF,
    ])
    # CRC covers TYPE + LEN(2) + SEQ + PAYLOAD (header[1:] skips START).
    crc = crc16_ccitt(header[1:] + payload, CRC16_INIT)
    return header + payload + bytes([crc & 0xFF, crc >> 8])


# ── Generic command builders ───────────────────────────────────────

def build_cmd_sync(seq: int) -> bytes:
    """Build a CMD_SYNC frame. No payload."""
    return encode_frame(FrameType.CMD_SYNC, seq)


def build_cmd_setup_sensor(
    seq: int,
    protocol_id: int,
    cfg: bytes,
) -> bytes:
    """Build a CMD_SETUP_SENSOR frame.

    Args:
        seq:         Sequence number (0..255).
        protocol_id: ProtocolId value (e.g. ProtocolId.I2C).
        cfg:         Protocol-specific config payload, e.g. from pack_i2c_cfg().

    Raises:
        ValueError: if protocol_id is not a known ProtocolId.
    """
    if protocol_id not in ProtocolId.__members__.values():
        raise ValueError(
            f"unknown protocol_id {protocol_id:#04x} — add it to ProtocolId if intentional"
        )
    return encode_frame(
        FrameType.CMD_SETUP_SENSOR,
        seq,
        bytes([protocol_id & 0xFF]) + cfg,
    )


def build_cmd_set_output(
    seq: int,
    sensor_id: int,
    values: bytes,
) -> bytes:
    """Build a CMD_SET_OUTPUT frame.

    Args:
        seq:       Sequence number (0..255).
        sensor_id: Sensor ID returned by a successful SETUP (1..255).
        values:    Protocol-specific value payload, e.g. from pack_i2c_set_output().

    Raises:
        ValueError: if sensor_id is 0 or > 255.
    """
    if not 1 <= sensor_id <= 0xFF:
        raise ValueError(f"sensor_id {sensor_id:#04x} out of valid range 0x01..0xFF")
    return encode_frame(
        FrameType.CMD_SET_OUTPUT,
        seq,
        bytes([sensor_id & 0xFF]) + values,
    )


def build_cmd_stop_sensor(seq: int, sensor_id: int) -> bytes:
    """Build a CMD_STOP_SENSOR frame.

    Args:
        seq:       Sequence number (0..255).
        sensor_id: Sensor ID to stop (1..255).

    Raises:
        ValueError: if sensor_id is 0 or > 255.
    """
    if not 1 <= sensor_id <= 0xFF:
        raise ValueError(f"sensor_id {sensor_id:#04x} out of valid range 0x01..0xFF")
    return encode_frame(
        FrameType.CMD_STOP_SENSOR,
        seq,
        bytes([sensor_id & 0xFF]),
    )


# ── Response decoders ──────────────────────────────────────────────

def parse_ack(frame: Frame) -> AckResponse:
    """Decode an RSP_ACK frame payload into an AckResponse.

    Raises:
        ValueError: if the frame type is not RSP_ACK, or payload length is wrong.
    """
    if frame.type != FrameType.RSP_ACK:
        raise ValueError(
            f"expected RSP_ACK (0x{FrameType.RSP_ACK:02X}), got 0x{frame.type:02X}"
        )
    if len(frame.payload) != _RSP_ACK_SIZE:
        raise ValueError(
            f"RSP_ACK payload must be {_RSP_ACK_SIZE} bytes, got {len(frame.payload)}"
        )
    return AckResponse(
        cmd_type=frame.payload[0],
        error_code=frame.payload[1],
        sensor_id=frame.payload[2],
    )


def parse_error_response(frame: Frame) -> ErrorResponse:
    """Decode an RSP_ERROR frame payload into an ErrorResponse.

    Raises:
        ValueError: if the frame type is not RSP_ERROR, or payload length is wrong.
    """
    if frame.type != FrameType.RSP_ERROR:
        raise ValueError(
            f"expected RSP_ERROR (0x{FrameType.RSP_ERROR:02X}), got 0x{frame.type:02X}"
        )
    if len(frame.payload) != _RSP_ERROR_SIZE:
        raise ValueError(
            f"RSP_ERROR payload must be {_RSP_ERROR_SIZE} bytes, got {len(frame.payload)}"
        )
    return ErrorResponse(
        sensor_id=frame.payload[0],
        error_code=frame.payload[1],
    )


# ── Sequence counter ───────────────────────────────────────────────

class SeqCounter:
    """Monotonically incrementing sequence counter, wrapping 255 → 0.

    The sequence number is an 8-bit field shared between the BBB command and
    the firmware ACK, so both sides can correlate responses to requests.

    Not thread-safe. If commands are issued from multiple threads, guard
    next() with a lock.

    Usage:
        seq = SeqCounter()
        frame = build_cmd_sync(seq.next())
    """

    __slots__ = ("_value",)

    def __init__(self, start: int = 0) -> None:
        if not 0 <= start <= 0xFF:
            raise ValueError(f"start {start} out of range 0..255")
        self._value = start

    def next(self) -> int:
        """Return the current value and advance the counter."""
        value = self._value
        self._value = (self._value + 1) & 0xFF
        return value

    @property
    def current(self) -> int:
        """The value that will be returned by the next call to next()."""
        return self._value
