"""Incremental byte-stream parser mirroring the STM32 C state machine.

Design mirrors stm32/src/protocol/protocol_parser.c:
  - Same eight states, same transitions, same CRC coverage.
  - Returns ParseError (not None) on CRC mismatch or over-length payload so
    callers can distinguish a genuine failure from an incomplete frame.
  - All state is instance variables; no module-level mutable state.
"""
from __future__ import annotations

from enum import Enum, auto

from .constants import MAX_PAYLOAD_SIZE, START_BYTE
from .crc import CRC16_INIT, crc16_step
from .frame import Frame, ParseError, ParseErrorReason


class _State(Enum):
    WAIT_START   = auto()
    READ_TYPE    = auto()
    READ_LEN_LO  = auto()
    READ_LEN_HI  = auto()
    READ_SEQ     = auto()
    READ_PAYLOAD = auto()
    READ_CRC_LO  = auto()
    READ_CRC_HI  = auto()


class ProtocolParser:
    """Incremental byte-by-byte frame parser.

    Feed bytes one at a time via feed(), or pass a chunk via feed_bytes().

    Return values of feed():
        Frame       — complete, CRC-valid frame assembled.
        ParseError  — frame boundary reached but rejected (CRC mismatch or
                      over-length payload). Distinct from None so callers can
                      log or count parse failures.
        None        — byte consumed, frame not yet complete.

    Error recovery:
        On any error the parser resets to WAIT_START. The next 0xAA in the
        stream starts a fresh frame, matching the embedded side's behaviour.

    Thread safety:
        Not thread-safe. Use one instance per reader thread.
    """

    __slots__ = (
        "_state",
        "_frame_type",
        "_expected_len",
        "_seq",
        "_payload",
        "_payload_idx",
        "_crc_running",
        "_crc_recv",
    )

    def __init__(self) -> None:
        self.reset()

    # ── Public API ─────────────────────────────────────────────────

    def reset(self) -> None:
        """Discard any partial frame and return to WAIT_START."""
        self._state: _State        = _State.WAIT_START
        self._frame_type: int      = 0
        self._expected_len: int    = 0
        self._seq: int             = 0
        self._payload: bytearray   = bytearray()
        self._payload_idx: int     = 0
        self._crc_running: int     = CRC16_INIT
        self._crc_recv: int        = 0

    def feed(self, byte: int) -> Frame | ParseError | None:
        """Feed one byte.

        Returns:
            Frame       — complete, CRC-valid frame.
            ParseError  — frame rejected (CRC_MISMATCH or PAYLOAD_TOO_LARGE).
            None        — byte consumed, no frame boundary yet.
        """
        match self._state:

            case _State.WAIT_START:
                if byte == START_BYTE:
                    self._crc_running = CRC16_INIT
                    self._state = _State.READ_TYPE
                # Non-START bytes here are line noise; discard and stay.
                return None

            case _State.READ_TYPE:
                self._frame_type = byte
                self._crc_running = crc16_step(self._crc_running, byte)
                self._state = _State.READ_LEN_LO
                return None

            case _State.READ_LEN_LO:
                self._expected_len = byte
                self._crc_running = crc16_step(self._crc_running, byte)
                self._state = _State.READ_LEN_HI
                return None

            case _State.READ_LEN_HI:
                self._expected_len |= byte << 8
                self._crc_running = crc16_step(self._crc_running, byte)

                if self._expected_len > MAX_PAYLOAD_SIZE:
                    err = ParseError(
                        reason=ParseErrorReason.PAYLOAD_TOO_LARGE,
                        frame_type=self._frame_type,
                        seq=0,  # SEQ not yet parsed at this state.
                    )
                    self.reset()
                    return err

                self._state = _State.READ_SEQ
                return None

            case _State.READ_SEQ:
                self._seq = byte
                self._crc_running = crc16_step(self._crc_running, byte)
                self._payload = bytearray(self._expected_len)
                self._payload_idx = 0
                self._state = (
                    _State.READ_CRC_LO
                    if self._expected_len == 0
                    else _State.READ_PAYLOAD
                )
                return None

            case _State.READ_PAYLOAD:
                if self._payload_idx >= MAX_PAYLOAD_SIZE:
                    # Defensive guard — shouldn't fire after the LEN_HI check.
                    err = ParseError(
                        reason=ParseErrorReason.PAYLOAD_TOO_LARGE,
                        frame_type=self._frame_type,
                        seq=self._seq,
                    )
                    self.reset()
                    return err
                self._payload[self._payload_idx] = byte
                self._payload_idx += 1
                self._crc_running = crc16_step(self._crc_running, byte)
                if self._payload_idx >= self._expected_len:
                    self._state = _State.READ_CRC_LO
                return None

            case _State.READ_CRC_LO:
                self._crc_recv = byte
                self._state = _State.READ_CRC_HI
                return None

            case _State.READ_CRC_HI:
                self._crc_recv |= byte << 8
                if self._crc_recv == self._crc_running:
                    result: Frame | ParseError = Frame(
                        type=self._frame_type,
                        seq=self._seq,
                        payload=bytes(self._payload),
                    )
                else:
                    result = ParseError(
                        reason=ParseErrorReason.CRC_MISMATCH,
                        frame_type=self._frame_type,
                        seq=self._seq,
                    )
                self.reset()
                return result

        # Unreachable under normal operation.
        self.reset()
        return None

    def feed_bytes(self, data: bytes | bytearray) -> list[Frame | ParseError]:
        """Feed a chunk of bytes and return all frames and parse errors found within it.

        Both complete valid frames and rejected frames (ParseError) are included
        so callers can log failures without a separate pass.

        Suitable for passing serial.read() results directly:

            for event in parser.feed_bytes(port.read(port.in_waiting or 1)):
                if isinstance(event, Frame):
                    ...
                elif isinstance(event, ParseError):
                    log.warning("parse error: %s", event.reason)
        """
        events: list[Frame | ParseError] = []
        for byte in data:
            result = self.feed(byte)
            if result is not None:
                events.append(result)
        return events
