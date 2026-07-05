"""Receives cloud commands over NATS and routes them.

CommandEnvelope.payload variants split into two layers:

* ``session_start`` / ``session_stop`` — cloud-only lifecycle messages. They
  update the BBB's self-reported state (session_id, online_status) via the
  Heartbeat. They are NOT relayed to the STM32.

* ``runtime`` — wraps a RuntimeCommand (setup_sensor, set_output, stop_sensor,
  scenario, sync). We unwrap, build the on-wire payload that bbb/protocol/
  expects, and pass it to StmLink.send_command. ACK forwarding back to the
  cloud is a separate concern owned by the response bridge.

Subscription model
------------------
The subject is ``device.{device_id}.command`` — each BBB owns its own subject,
so the workqueue stream's "delivered to exactly one consumer" semantics map
cleanly onto "delivered to this BBB." No wildcard, no in-handler filter; if a
message arrives here it's for us by construction.
"""
from __future__ import annotations

import logging

from nats.js.api import DeliverPolicy

from comms import NatsClient, StmLink
from hilglebone.v1 import command_pb2, common_pb2, status_pb2
from orchestrator.response_bridge import ResponseBridge
from orchestrator.state import DeviceState
from protocol import FrameType

log = logging.getLogger(__name__)

COMMAND_SUBJECT_FMT = "device.{device_id}.command"

# Valid protocol_id values, derived from the proto enum so adding a new
# protocol there (e.g. MODBUS) automatically widens this set without code
# changes here. UNSPECIFIED is explicitly excluded.
_VALID_PROTOCOL_IDS: frozenset[int] = frozenset(
    v for v in common_pb2.ProtocolId.values()
    if v != common_pb2.ProtocolId.PROTOCOL_ID_UNSPECIFIED
)


class CommandBridge:
    """Bridges cloud-side CommandEnvelope messages to the STM32 + Heartbeat."""

    def __init__(
        self,
        nats: NatsClient,
        stm: StmLink,
        state: DeviceState,
        response_bridge: ResponseBridge | None = None,
    ) -> None:
        if not state.device_id:
            raise ValueError("state.device_id is empty")
        self._nats = nats
        self._stm = stm
        self._state = state
        self._response_bridge = response_bridge

    async def open(self) -> None:
        """Attach the NATS subscription.

        DeliverPolicy.ALL is mandatory: COMMANDS is a workqueue-retention
        stream, and JetStream refuses consumers with any other deliver
        policy on those streams (DeliverPolicy.NEW would silently skip
        already-queued work since each message has exactly one consumer).
        """
        subject = COMMAND_SUBJECT_FMT.format(device_id=self._state.device_id)
        # Durable name is per-(device, purpose) so future subscriptions on
        # this device (e.g. the OTA channel) get their own consumer state
        # instead of colliding on `bbb-{device_id}`.
        durable = f"bbb-{self._state.device_id}-cmd"
        await self._nats.subscribe(
            subject,
            self._on_message,
            durable=durable,
            deliver_policy=DeliverPolicy.ALL,
        )
        log.info(
            "command bridge open",
            extra={"subject": subject, "device_id": self._state.device_id},
        )

    # ── NATS handler ───────────────────────────────────────────────

    async def _on_message(self, msg) -> None:  # noqa: ANN001  (NATS Msg)
        try:
            env = command_pb2.CommandEnvelope.FromString(msg.data)
        except Exception:
            log.exception("CommandEnvelope decode failed; dropping")
            await msg.ack()
            return

        # device_id routing is enforced by the subject; this guard is a
        # cheap sanity check against misrouted publishes by the cloud.
        if env.device_id != self._state.device_id:
            log.warning(
                "command on our subject but wrong device_id; dropping",
                extra={"env_device": env.device_id, "self": self._state.device_id},
            )
            await msg.ack()
            return

        which = env.WhichOneof("payload")

        # Session-state gate: enforce a simple state machine so a stale or
        # duplicated lifecycle message can't corrupt our view of the world.
        #
        #   no session  → only session_start is allowed
        #   in session  → only stop / runtime, and they must match current_id
        if not self._session_check_passes(which, env.session_id):
            await msg.ack()
            return

        try:
            if which == "session_start":
                await self._handle_session_start(env)
            elif which == "session_stop":
                await self._handle_session_stop(env)
            elif which == "runtime":
                await self._handle_runtime(env.runtime)
            else:
                log.warning("unknown command variant", extra={"variant": which})
        except Exception:
            log.exception("command handler raised")
        finally:
            await msg.ack()

    def _session_check_passes(self, which: str | None, env_session_id: str) -> bool:
        """Returns True iff this command is appropriate for the current state."""
        in_session = self._state.session_id != ""

        if which == "session_start":
            if in_session:
                log.warning(
                    "session_start while already in a session; dropping",
                    extra={
                        "current": self._state.session_id,
                        "requested": env_session_id,
                    },
                )
                return False
            return True

        if which == "session_stop":
            if not in_session:
                log.warning(
                    "session_stop while no session active; dropping",
                    extra={"requested": env_session_id},
                )
                return False
            if env_session_id != self._state.session_id:
                log.warning(
                    "session_stop for a different session; dropping",
                    extra={
                        "current": self._state.session_id,
                        "requested": env_session_id,
                    },
                )
                return False
            return True

        if which == "runtime":
            if not in_session:
                log.warning(
                    "runtime command while no session active; dropping",
                    extra={"requested": env_session_id},
                )
                return False
            if env_session_id != self._state.session_id:
                log.warning(
                    "runtime command for a different session; dropping",
                    extra={
                        "current": self._state.session_id,
                        "requested": env_session_id,
                    },
                )
                return False
            return True

        # Unknown / unset variant: let the caller's `else` branch log it.
        return True

    # ── Lifecycle handlers (cloud-only, never reach STM32) ─────────

    async def _handle_session_start(self, env: command_pb2.CommandEnvelope) -> None:
        self._state.session_id = env.session_id
        self._state.online_status = status_pb2.OnlineStatus.ONLINE_STATUS_BUSY
        log.info("session started", extra={"session_id": env.session_id})

    async def _handle_session_stop(self, env: command_pb2.CommandEnvelope) -> None:
        log.info("session stopped", extra={"session_id": env.session_id})
        self._state.session_id = ""
        self._state.online_status = status_pb2.OnlineStatus.ONLINE_STATUS_ONLINE

    # ── Runtime handlers (forwarded over UART) ─────────────────────

    async def _handle_runtime(self, runtime: command_pb2.RuntimeCommand) -> None:
        which = runtime.WhichOneof("payload")
        if which == "setup_sensor":
            await self._send_setup_sensor(runtime.setup_sensor)
        elif which == "set_output":
            await self._send_set_output(runtime.set_output)
        elif which == "stop_sensor":
            await self._send_stop_sensor(runtime.stop_sensor)
        elif which == "scenario":
            log.warning("scenario runtime command not implemented yet")
        elif which == "sync":
            await self._send_sync()
        else:
            log.warning("unknown runtime variant", extra={"variant": which})

    async def _send_setup_sensor(self, cmd: command_pb2.SetupSensorCommand) -> None:
        # Reject anything that isn't a defined ProtocolId (UNSPECIFIED, an
        # unknown value the BBB doesn't recognise, or a number above the
        # u8 wire limit). The set is derived from the proto enum so adding
        # MODBUS / 1-Wire later "just works".
        if cmd.protocol_id not in _VALID_PROTOCOL_IDS:
            log.warning(
                "setup_sensor: protocol_id is not a known ProtocolId; dropping",
                extra={"value": cmd.protocol_id},
            )
            return
        # Wire payload: [protocol_id_u8 | cfg…]
        payload = bytes([cmd.protocol_id]) + bytes(cmd.config)
        await self._send_command(FrameType.CMD_SETUP_SENSOR, payload)

    async def _send_set_output(self, cmd: command_pb2.SetOutputCommand) -> None:
        if not (0 < cmd.sensor_id <= 0xFF):
            log.warning("set_output: sensor_id out of range", extra={"sensor_id": cmd.sensor_id})
            return
        # Wire payload: [sensor_id_u8 | values…]
        payload = bytes([cmd.sensor_id]) + bytes(cmd.values)
        await self._send_command(FrameType.CMD_SET_OUTPUT, payload)

    async def _send_stop_sensor(self, cmd: command_pb2.StopSensorCommand) -> None:
        if not (0 < cmd.sensor_id <= 0xFF):
            log.warning("stop_sensor: sensor_id out of range", extra={"sensor_id": cmd.sensor_id})
            return
        # Wire payload: [sensor_id_u8]
        await self._send_command(FrameType.CMD_STOP_SENSOR, bytes([cmd.sensor_id]))

    async def _send_sync(self) -> None:
        # CMD_SYNC has an empty payload. StmLink.sync() runs its own retry
        # loop, but for a single-shot sync request we just push one frame.
        await self._send_command(FrameType.CMD_SYNC, b"")

    async def _send_command(self, frame_type: int, payload: bytes) -> None:
        try:
            ack, seq = await self._stm.send_command(frame_type, payload)
        except Exception:
            # TimeoutError (no ACK in window) or CommandError (cmd_type
            # mismatch — protocol bug) lands here. In both cases there's no
            # AckResponse to forward; the cloud will notice via the command
            # never producing a telemetry-ACK plus the BBB's logs.
            log.exception("stm32 command failed", extra={"frame_type": frame_type})
            return

        # Every ACK gets logged and forwarded — including the error_code != 0
        # case. The cloud's TelemetryEnvelope.AckResponse carries error_code
        # specifically so the backend / dashboard see "STM32 rejected this".
        log.log(
            logging.WARNING if not ack.ok else logging.INFO,
            "stm32 ack",
            extra={
                "frame_type": frame_type,
                "seq": seq,
                "error_code": ack.error_code,
                "sensor_id": ack.sensor_id,
            },
        )

        if self._response_bridge is not None:
            try:
                await self._response_bridge.publish_ack(
                    seq=seq,
                    cmd_type=ack.cmd_type,
                    error_code=ack.error_code,
                    sensor_id=ack.sensor_id,
                )
            except Exception:
                log.exception("ack forwarding failed", extra={"frame_type": frame_type})
