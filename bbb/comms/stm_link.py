"""Async serial bridge to the STM32 simulation engine.

Wraps pyserial with asyncio so the event loop is never blocked.
Reads are dispatched through a single-threaded executor; writes are
synchronous because individual frames are small (max ~263 bytes).
"""
from __future__ import annotations

import asyncio
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor

import serial

# ── Configuration ──────────────────────────────────────────────────

# Serial port defaults.
DEFAULT_BAUDRATE: int   = 115_200
PORT_TIMEOUT_S: float   = 0.05   # read() blocks at most this long when idle

# CMD_SYNC handshake defaults.
SYNC_RETRIES: int       = 5
SYNC_TIMEOUT_S: float   = 1.0    # per-attempt deadline

# ── Imports ────────────────────────────────────────────────────────

from protocol import (
    Frame,
    FrameType,
    ParseError,
    ProtocolParser,
    SeqCounter,
    build_cmd_sync,
    parse_ack,
)


def _noop_unsolicited(_frame: Frame) -> None:
    """Placeholder — replaced when logging subsystem is wired up."""

def _noop_parse_error(_error: ParseError) -> None:
    """Placeholder — replaced when logging subsystem is wired up."""


class SyncError(Exception):
    """CMD_SYNC handshake failed after all retries."""


class StmLink:
    """Async serial bridge to the STM32.

    Manages a pyserial port and a ProtocolParser instance.
    Provides a startup sync handshake via CMD_SYNC; higher-level command
    methods will be added in the comms layer as needed.

    Usage::

        async with StmLink("/dev/ttyO1") as link:
            await link.sync()
            # issue commands ...

    Or manually::

        link = StmLink("/dev/ttyO1")
        link.open()
        try:
            await link.sync()
        finally:
            link.close()
    """

    def __init__(
        self,
        port_path: str,
        baudrate: int = DEFAULT_BAUDRATE,
    ) -> None:
        self._port_path = port_path
        self._baudrate = baudrate
        self._port: serial.Serial | None = None
        self._parser = ProtocolParser()
        self._seq = SeqCounter()
        # Single worker: serial reads must be serialised.
        self._executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="stm_rx")

        # Event hooks — assign a callable to receive frames/errors that
        # aren't consumed by the current operation (e.g. sync).  Wire
        # these to the logging subsystem once it exists.
        self.on_unsolicited_frame: Callable[[Frame], None] = _noop_unsolicited
        self.on_parse_error: Callable[[ParseError], None] = _noop_parse_error

    # ── Lifecycle ──────────────────────────────────────────────────

    def open(self) -> None:
        """Open the serial port.

        Raises:
            serial.SerialException: if the port cannot be opened.
        """
        self._port = serial.Serial(
            self._port_path,
            self._baudrate,
            timeout=PORT_TIMEOUT_S,
        )

    def close(self) -> None:
        """Close the serial port and shut down the reader thread pool."""
        if self._port is not None and self._port.is_open:
            self._port.close()
        self._executor.shutdown(wait=False)

    async def __aenter__(self) -> StmLink:
        self.open()
        return self

    async def __aexit__(self, *_: object) -> None:
        self.close()

    # ── I/O helpers ────────────────────────────────────────────────

    async def _read(self) -> bytes:
        """Read available bytes without blocking the event loop.

        Reads all bytes waiting in the OS buffer; if the buffer is empty,
        blocks in the executor for up to _PORT_TIMEOUT_S seconds waiting
        for at least one byte. This avoids a busy-wait spin.
        """
        assert self._port is not None, "port not open"
        port = self._port
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(
            self._executor,
            lambda: port.read(port.in_waiting or 1),
        )

    def _write(self, data: bytes) -> None:
        assert self._port is not None, "port not open"
        self._port.write(data)

    # ── Synchronization ────────────────────────────────────────────

    async def sync(
        self,
        retries: int = SYNC_RETRIES,
        timeout_s: float = SYNC_TIMEOUT_S,
    ) -> None:
        """Establish communication with the STM32 via CMD_SYNC handshake.

        Sends CMD_SYNC and waits for a matching RSP_ACK. Each attempt uses
        a fresh sequence number so stale ACKs from earlier attempts are
        discarded by seq matching, not by parser state.

        Args:
            retries:   Maximum number of CMD_SYNC attempts.
            timeout_s: Per-attempt deadline in seconds.

        Raises:
            SyncError:              if no valid ACK received after all retries.
            serial.SerialException: if the port fails mid-handshake.
        """
        for attempt in range(1, retries + 1):
            sent_seq = self._seq.next()
            self._write(build_cmd_sync(sent_seq))

            try:
                async with asyncio.timeout(timeout_s):
                    while True:
                        chunk = await self._read()
                        for event in self._parser.feed_bytes(chunk):
                            if isinstance(event, ParseError):
                                self.on_parse_error(event)
                                continue
                            assert isinstance(event, Frame)
                            if event.type != FrameType.RSP_ACK:
                                self.on_unsolicited_frame(event)
                                continue
                            if event.seq != sent_seq:
                                self.on_unsolicited_frame(event)
                                continue
                            ack = parse_ack(event)
                            if ack.ok:
                                return
                            # ACK with error — not expected for SYNC, treat as failure.
                            raise SyncError(
                                f"CMD_SYNC attempt {attempt}: RSP_ACK returned "
                                f"error_code=0x{ack.error_code:02X}"
                            )
            except TimeoutError:
                # No response within the window — retry.
                continue

        raise SyncError(
            f"CMD_SYNC failed: no valid RSP_ACK after {retries} "
            f"attempt(s) ({timeout_s}s timeout each)"
        )
