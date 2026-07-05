"""Forwards STM32-side responses to the cloud as TelemetryEnvelope messages.

Three response shapes flow through here:

* **Solicited ACK** — returned by ``StmLink.send_command``. The CommandBridge
  hands it to us via :meth:`publish_ack` after each runtime command it relays.

* **Unsolicited RSP_ERROR** — STM32 detected a fault on an already-running
  sensor (bus collision, hardware peripheral error). Delivered through
  ``StmLink.on_unsolicited_frame`` (sync callback). We re-parse and publish.

* **Unsolicited STATUS_REPORT** — periodic uptime / active-sensor /
  queue-depth heartbeat from the STM32. Same delivery path.

The unsolicited callback fires synchronously from inside an async coroutine
on the event loop, so we use ``asyncio.create_task`` to fire-and-forget the
publish. A publish failure is logged, not retried — the cloud's view of
device health is heartbeat-driven, so missing one telemetry frame is not
catastrophic.
"""
from __future__ import annotations

import asyncio
import logging
import time

from comms import NatsClient, StmLink
from hilglebone.v1 import telemetry_pb2
from orchestrator.state import DeviceState
from protocol import Frame, FrameType, parse_ack, parse_error_response, parse_status_report

log = logging.getLogger(__name__)

TELEMETRY_SUBJECT_FMT = "device.{device_id}.telemetry"

class ResponseBridge:
    """Re-encodes STM32 responses as TelemetryEnvelope and publishes them."""

    def __init__(
        self,
        nats: NatsClient,
        state: DeviceState,
    ) -> None:
        if not state.device_id:
            raise ValueError("state.device_id is empty")
        self._nats = nats
        self._state = state
        self._subject = TELEMETRY_SUBJECT_FMT.format(device_id=state.device_id)

    # ── Hookup ─────────────────────────────────────────────────────

    def install_on(self, stm: StmLink) -> None:
        """Register our sync callback as the StmLink unsolicited-frame hook."""
        stm.on_unsolicited_frame = self._on_unsolicited
        log.info("response bridge installed on stm link")

    # ── Sync hook for unsolicited frames ───────────────────────────

    def _on_unsolicited(self, frame: Frame) -> None:
        # Called from inside StmLink's reader coroutine — we are on the
        # event loop. create_task fire-and-forgets the async publish.
        if frame.type == FrameType.RSP_ACK:
            # A late ACK (post-timeout) or duplicate. The reader's dispatcher
            # already tried the pending-ACKs map; reaching here means no
            # waiter was registered. Still worth forwarding so the cloud
            # sees the outcome — frame.seq is the real wire sequence number.
            try:
                ack = parse_ack(frame)
            except Exception:
                log.exception("late RSP_ACK: parse failed")
                return
            asyncio.create_task(
                self.publish_ack(
                    seq=frame.seq,
                    cmd_type=ack.cmd_type,
                    error_code=ack.error_code,
                    sensor_id=ack.sensor_id,
                ),
                name="resp-bridge-late-ack",
            )

        elif frame.type == FrameType.RSP_ERROR:
            try:
                err = parse_error_response(frame)
            except Exception:
                log.exception("unsolicited RSP_ERROR: parse failed")
                return
            asyncio.create_task(
                self._publish_error(err.sensor_id, err.error_code),
                name="resp-bridge-error",
            )

        elif frame.type == FrameType.STATUS_REPORT:
            try:
                rep = parse_status_report(frame)
            except ValueError:
                log.exception(
                    "STATUS_REPORT: parse failed",
                    extra={"length": len(frame.payload)},
                )
                return
            asyncio.create_task(
                self._publish_status_report(
                    rep.uptime_s, rep.active_sensor_count, rep.command_queue_depth
                ),
                name="resp-bridge-status",
            )

        else:
            log.warning(
                "ignoring unsolicited frame of unexpected type",
                extra={"frame_type": frame.type},
            )

    # ── Envelope builders ──────────────────────────────────────────

    async def publish_ack(self, seq: int, cmd_type: int, error_code: int, sensor_id: int) -> None:
        """Public — called by CommandBridge after each solicited ACK, and by
        our own unsolicited hook when a late ACK arrives."""
        env = telemetry_pb2.TelemetryEnvelope(
            device_id=self._state.device_id,
            session_id=self._state.session_id,
            timestamp_us=int(time.time() * 1_000_000),
            ack=telemetry_pb2.AckResponse(
                sequence_num=seq,
                cmd_type=cmd_type,
                error_code=error_code,
                sensor_id=sensor_id,
            ),
        )
        await self._publish(env)

    async def _publish_error(self, sensor_id: int, error_code: int) -> None:
        env = telemetry_pb2.TelemetryEnvelope(
            device_id=self._state.device_id,
            session_id=self._state.session_id,
            timestamp_us=int(time.time() * 1_000_000),
            error=telemetry_pb2.ErrorResponse(
                sensor_id=sensor_id,
                error_code=error_code,
            ),
        )
        await self._publish(env)

    async def _publish_status_report(self, uptime_s: int, active: int, queue: int) -> None:
        env = telemetry_pb2.TelemetryEnvelope(
            device_id=self._state.device_id,
            session_id=self._state.session_id,
            timestamp_us=int(time.time() * 1_000_000),
            status_report=telemetry_pb2.StatusReport(
                uptime_s=uptime_s,
                active_sensor_count=active,
                command_queue_depth=queue,
            ),
        )
        await self._publish(env)

    # ── Common publish path ────────────────────────────────────────

    async def _publish(self, env: telemetry_pb2.TelemetryEnvelope) -> None:
        try:
            await self._nats.publish(self._subject, env.SerializeToString())
        except Exception:
            log.exception("telemetry publish failed")
