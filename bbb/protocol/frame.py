"""Immutable value objects for protocol frames, decoded responses, and parse errors."""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum, auto


@dataclass(frozen=True, slots=True)
class Frame:
    """A complete, CRC-validated frame received from the STM32.

    Attributes:
        type:    Raw FrameType byte. Kept as int so unknown future types
                 pass through without raising here.
        seq:     Sequence number echoed by the firmware (0..255).
        payload: Raw payload bytes; zero-length for payload-free frames.
    """

    type: int
    seq: int
    payload: bytes


@dataclass(frozen=True, slots=True)
class AckResponse:
    """Decoded RSP_ACK payload (3 bytes fixed).

    Attributes:
        cmd_type:   FrameType of the command being acknowledged.
        error_code: ErrorCode value; 0 == ERR_SUCCESS.
        sensor_id:  On SETUP success: newly assigned id.
                    On SETUP failure: 0.
                    On SET_OUT / STOP: echoed sensor_id from the command.
                    On SYNC: 0.
    """

    cmd_type: int
    error_code: int
    sensor_id: int

    @property
    def ok(self) -> bool:
        """True iff error_code is ERR_SUCCESS (0x00)."""
        return self.error_code == 0x00


@dataclass(frozen=True, slots=True)
class ErrorResponse:
    """Decoded RSP_ERROR payload (2 bytes fixed).

    Attributes:
        sensor_id:  Sensor context; 0 when not sensor-scoped.
        error_code: ErrorCode value.
    """

    sensor_id: int
    error_code: int


class ParseErrorReason(Enum):
    """Why the parser rejected a frame."""

    CRC_MISMATCH      = auto()  # Complete frame received but CRC check failed.
    PAYLOAD_TOO_LARGE = auto()  # Declared length field exceeds MAX_PAYLOAD_SIZE.


@dataclass(frozen=True, slots=True)
class ParseError:
    """A frame that was rejected during parsing.

    Returned by ProtocolParser.feed() in place of None when a frame
    boundary is detected but the frame is invalid. Allows callers to
    distinguish a genuine parse failure from an incomplete (mid-frame) byte.

    Attributes:
        reason:     Why the frame was rejected.
        frame_type: TYPE byte of the partial frame; 0 if not yet read.
        seq:        SEQ byte of the partial frame; 0 if not yet read
                    (PAYLOAD_TOO_LARGE is detected before SEQ is parsed).
    """

    reason: ParseErrorReason
    frame_type: int
    seq: int
