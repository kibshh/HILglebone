"""Async serial bridge to the STM32 simulation engine.

Wraps pyserial with asyncio so the event loop is never blocked.
Reads are dispatched through a single-threaded executor; writes are
synchronous because individual frames are small (max ~263 bytes).

Architecture: a single long-lived reader task drains the port and
dispatches every parsed frame. RSP_ACKs land in a pending-ACK futures
map (matched by seq); everything else — unsolicited RSP_ERROR,
STATUS_REPORT, late ACKs, mismatched-seq frames — goes to
``on_unsolicited_frame``. This means STM32 messages are received even
when no command is in flight, so STATUS_REPORTs and RSP_ERRORs aren't
queued in the OS serial buffer waiting for the next ``send_command``.
"""
from __future__ import annotations

import asyncio
import logging
from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor

import serial

log = logging.getLogger(__name__)

# ── Configuration ──────────────────────────────────────────────────

# Serial port defaults.
DEFAULT_BAUDRATE: int   = 115_200
PORT_TIMEOUT_S: float   = 0.05   # read() blocks at most this long when idle

# CMD_SYNC handshake defaults.
SYNC_RETRIES: int       = 5
SYNC_TIMEOUT_S: float   = 1.0    # per-attempt deadline

# General command timeout.
CMD_TIMEOUT_S: float    = 3.0    # send_command() deadline

# ── Imports ────────────────────────────────────────────────────────

from protocol import (
    AckResponse,
    Frame,
    FrameType,
    ParseError,
    ProtocolParser,
    SeqCounter,
    encode_frame,
    parse_ack,
)


def _noop_unsolicited(_frame: Frame) -> None:
    """Default hook for unsolicited frames (RSP_ERROR, STATUS_REPORT).

    Drops them silently so a bare StmLink in tests / standalone use never
    crashes on them. Production wiring replaces this with the orchestrator's
    ResponseBridge, which forwards them to the cloud as telemetry.
    """

def _noop_parse_error(_error: ParseError) -> None:
    """Default hook for parse errors (bad CRC, malformed frame).

    Drops them silently. Production wiring should replace this with something
    that at least logs — or, eventually, surfaces parse errors as telemetry so
    operators can detect a flaky UART link.
    """


class SyncError(Exception):
    """CMD_SYNC handshake failed after all retries."""


class CommandError(Exception):
    """A command received an error ACK or timed out waiting for a response."""


class StmLink:
    """Async serial bridge to the STM32.

    Manages a pyserial port and a ProtocolParser instance.
    Provides a startup sync handshake via CMD_SYNC; higher-level command
    methods will be added in the comms layer as needed.

    Usage::

        async with StmLink("/dev/ttyS1") as link:
            await link.sync()
            # issue commands ...

    Or manually::

        link = StmLink("/dev/ttyS1")
        await link.open()
        try:
            await link.sync()
        finally:
            await link.close()
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

        # Reader-task plumbing (filled in by open()).
        self._reader_task: asyncio.Task[None] | None = None
        # seq → future awaiting an RSP_ACK for that command.
        self._pending_acks: dict[int, asyncio.Future[Frame]] = {}

        # Event hooks — assign a callable to receive frames/errors that
        # aren't consumed by a pending command (RSP_ERROR, STATUS_REPORT,
        # late or duplicate RSP_ACKs).
        self.on_unsolicited_frame: Callable[[Frame], None] = _noop_unsolicited
        self.on_parse_error: Callable[[ParseError], None] = _noop_parse_error

    # ── Lifecycle ──────────────────────────────────────────────────

    async def open(self) -> None:
        """Open the serial port and start the background reader task.

        Idempotent: a second call is a no-op.

        Raises:
            serial.SerialException: if the port cannot be opened.
        """
        if self._port is not None:
            return
        self._port = serial.Serial(
            self._port_path,
            self._baudrate,
            timeout=PORT_TIMEOUT_S,
        )
        self._reader_task = asyncio.create_task(self._reader_loop(), name="stm-reader")

    async def close(self) -> None:
        """Stop the reader, fail any pending ACKs, close the port."""
        if self._reader_task is not None:
            self._reader_task.cancel()
            try:
                await self._reader_task
            except asyncio.CancelledError:
                pass
            self._reader_task = None
        # Fail any commands still waiting on an ACK so callers unblock
        # instead of hanging until their timeout fires.
        for seq, future in self._pending_acks.items():
            if not future.done():
                future.set_exception(
                    CommandError(f"stm link closed while awaiting ack for seq={seq}")
                )
        self._pending_acks.clear()
        if self._port is not None and self._port.is_open:
            self._port.close()
        self._port = None
        self._executor.shutdown(wait=True)

    async def __aenter__(self) -> StmLink:
        await self.open()
        return self

    async def __aexit__(self, *_: object) -> None:
        await self.close()

    # ── I/O helpers ────────────────────────────────────────────────

    async def _read(self) -> bytes:
        """Read available bytes without blocking the event loop.

        Reads all bytes waiting in the OS buffer; if the buffer is empty,
        blocks in the executor for up to _PORT_TIMEOUT_S seconds waiting
        for at least one byte. This avoids a busy-wait spin.
        """
        if self._port is None:
            raise RuntimeError("port not open")
        port = self._port
        loop = asyncio.get_running_loop()
        return await loop.run_in_executor(
            self._executor,
            lambda: port.read(port.in_waiting or 1),
        )

    def _write(self, data: bytes) -> None:
        if self._port is None:
            raise RuntimeError("port not open")
        self._port.write(data)

    # ── Reader loop ────────────────────────────────────────────────

    async def _reader_loop(self) -> None:
        """Continuously read + dispatch until the task is cancelled."""
        while True:
            try:
                chunk = await self._read()
            except asyncio.CancelledError:
                return
            except Exception:
                # Don't kill the loop on a transient read error; back off
                # briefly and try again.
                log.exception("stm reader: read failed")
                await asyncio.sleep(0.1)
                continue
            if not chunk:
                continue
            for event in self._parser.feed_bytes(chunk):
                self._dispatch(event)

    def _dispatch(self, event: Frame | ParseError) -> None:
        if isinstance(event, ParseError):
            try:
                self.on_parse_error(event)
            except Exception:
                log.exception("on_parse_error handler raised")
            return
        if not isinstance(event, Frame):
            return  # unknown event type — defensive
        if event.type == FrameType.RSP_ACK:
            future = self._pending_acks.get(event.seq)
            if future is not None and not future.done():
                future.set_result(event)
                return
            # ACK with no waiter — late (post-timeout) or duplicate. Falls
            # through to the unsolicited hook so the response bridge can
            # still surface it as telemetry.
        try:
            self.on_unsolicited_frame(event)
        except Exception:
            log.exception("on_unsolicited_frame handler raised")

    # ── Synchronization ────────────────────────────────────────────

    async def sync(
        self,
        retries: int = SYNC_RETRIES,
        timeout_s: float = SYNC_TIMEOUT_S,
    ) -> None:
        """Establish communication with the STM32 via CMD_SYNC handshake.

        Retries on timeout, propagates any other CommandError as SyncError.

        Args:
            retries:   Maximum number of CMD_SYNC attempts.
            timeout_s: Per-attempt deadline in seconds.

        Raises:
            SyncError:              if no valid ACK received after all retries.
            serial.SerialException: if the port fails mid-handshake.
        """
        for attempt in range(1, retries + 1):
            try:
                await self.send_command(FrameType.CMD_SYNC, timeout_s=timeout_s)
                return
            except TimeoutError:
                continue  # retry
            except CommandError as exc:
                raise SyncError(f"CMD_SYNC attempt {attempt}: {exc}") from exc

        raise SyncError(
            f"CMD_SYNC failed: no valid RSP_ACK after {retries} "
            f"attempt(s) ({timeout_s}s timeout each)"
        )

    # ── Generic command ────────────────────────────────────────────

    async def send_command(
        self,
        frame_type: int,
        payload: bytes = b"",
        timeout_s: float = CMD_TIMEOUT_S,
    ) -> AckResponse:
        """Send a command and wait for its matching RSP_ACK.

        Allocates a fresh seq, registers a future in the pending-ACKs map,
        writes the frame, then awaits the future. The reader task resolves
        the future when an ACK with the same seq arrives.

        Args:
            frame_type: One of the ``FrameType.CMD_*`` constants.
            payload:    Command-specific payload bytes (default empty).
            timeout_s:  Deadline in seconds (default :data:`CMD_TIMEOUT_S`).

        Returns:
            :class:`~protocol.AckResponse` for the sent frame.

        Raises:
            TimeoutError:               if no matching ACK arrives in time.
            CommandError:               if the ACK carries a non-zero error_code
                                        or its cmd_type doesn't match.
            serial.SerialException:     if the port fails mid-command.
            ValueError:                 if ``frame_type`` or ``payload`` are invalid.
        """
        seq = self._seq.next()
        loop = asyncio.get_running_loop()
        future: asyncio.Future[Frame] = loop.create_future()
        # Register the waiter BEFORE writing so a (theoretical) instant ACK
        # can't race past us.
        self._pending_acks[seq] = future
        try:
            self._write(encode_frame(frame_type, seq=seq, payload=payload))
            try:
                ack_frame = await asyncio.wait_for(future, timeout_s)
            except asyncio.TimeoutError:
                raise TimeoutError(
                    f"command 0x{frame_type:02X} seq={seq}: "
                    f"no RSP_ACK within {timeout_s}s"
                ) from None
            ack = parse_ack(ack_frame)
            if not ack.ok:
                raise CommandError(
                    f"frame_type=0x{ack.cmd_type:02X} seq={seq}: "
                    f"RSP_ACK error_code=0x{ack.error_code:02X}"
                )
            if ack.cmd_type != frame_type:
                raise CommandError(
                    f"expected cmd_type=0x{frame_type:02X} seq={seq}: "
                    f"ACK cmd_type mismatch 0x{ack.cmd_type:02X}"
                )
            return ack
        finally:
            self._pending_acks.pop(seq, None)
