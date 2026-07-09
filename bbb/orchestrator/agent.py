"""Boot orchestrator: wires the BBB's components together in the right order.

Startup sequence (roadmap: "STM32 sync handshake first"):

    1. Open serial (StmLink).
    2. Run CMD_SYNC handshake. On failure, exit — the BBB must NOT register,
       otherwise we'd be reporting ourselves operational to the backend while
       the DUT-facing MCU is unreachable.
    3. Register with the backend. Registration returns our device_id, which is
       the identity every downstream component keys off of, so DeviceState is
       only constructable after this step.
    4. Open NATS.
    5. Start heartbeat (fires immediately, so the backend sees us come up
       with the correct stm32_state before any command traffic).
    6. Install response bridge on StmLink BEFORE the command bridge opens: the
       first ack/error a command triggers must find a listener already
       attached.
    7. Open command bridge.
    8. Open OTA bridge.
    9. Wait for SIGTERM / SIGINT, then shut down in reverse order.

Shutdown is best-effort: each teardown is guarded so one failing component
never strands the next open.
"""
from __future__ import annotations

import asyncio
import logging
import os
import signal
from dataclasses import dataclass

from comms import BackendClient, NatsClient, StmLink
from comms.stm_link import SyncError
from hilglebone.v1 import common_pb2
from orchestrator.command_bridge import CommandBridge
from orchestrator.heartbeat import DEFAULT_INTERVAL_S, Heartbeat
from orchestrator.ota_bridge import OtaBridge
from orchestrator.response_bridge import ResponseBridge
from orchestrator.state import DeviceState

log = logging.getLogger(__name__)


# ── Configuration ──────────────────────────────────────────────────

@dataclass
class AgentConfig:
    """All knobs the orchestrator needs. Loaded from env at startup.

    stm_port, stm_baudrate, heartbeat_interval_s have sensible defaults for
    the reference BBB image; the rest identify this specific deployment and
    must be provided (a missing value is fatal, not silently defaulted).
    """
    stm_port: str
    stm_baudrate: int
    backend_url: str
    registration_token: str
    nats_url: str
    heartbeat_interval_s: float

    @classmethod
    def from_env(cls) -> "AgentConfig":
        return cls(
            stm_port=os.environ.get("BBB_STM_PORT", "/dev/ttyS1"),
            stm_baudrate=int(os.environ.get("BBB_STM_BAUDRATE", "115200")),
            backend_url=_required_env("BBB_BACKEND_URL"),
            registration_token=_required_env("BBB_REGISTRATION_TOKEN"),
            nats_url=_required_env("BBB_NATS_URL"),
            heartbeat_interval_s=float(
                os.environ.get("BBB_HEARTBEAT_INTERVAL_S", str(DEFAULT_INTERVAL_S))
            ),
        )


def _required_env(name: str) -> str:
    val = os.environ.get(name)
    if not val:
        raise SystemExit(f"missing required env var: {name}")
    return val


# ── Agent ──────────────────────────────────────────────────────────

class Agent:
    """Boot + shutdown lifecycle for one BeagleBone."""

    def __init__(self, config: AgentConfig) -> None:
        self._cfg = config
        # Populated during _startup(). Kept as Optional so _shutdown can run
        # a partial teardown if startup aborted mid-sequence.
        self._stm: StmLink | None = None
        self._backend: BackendClient | None = None
        self._nats: NatsClient | None = None
        self._state: DeviceState | None = None
        self._heartbeat: Heartbeat | None = None
        self._response_bridge: ResponseBridge | None = None
        self._command_bridge: CommandBridge | None = None
        self._ota_bridge: OtaBridge | None = None

    async def run(self) -> None:
        """Start everything and block until a shutdown signal fires."""
        try:
            await self._startup()
        except BaseException:
            # Startup aborted (SyncError, registration failure, NATS failure,
            # etc.). Roll back whatever partial state we did create — an
            # open serial port or in-flight NATS subscription would linger
            # otherwise.
            await self._shutdown()
            raise

        try:
            await self._wait_for_shutdown_signal()
        finally:
            await self._shutdown()

    # ── Startup ────────────────────────────────────────────────────

    async def _startup(self) -> None:
        # 1) Serial + STM32 sync. Hard prerequisite for the rest.
        self._stm = StmLink(self._cfg.stm_port, self._cfg.stm_baudrate)
        await self._stm.open()
        try:
            await self._stm.sync()
        except SyncError:
            log.error("stm32 handshake failed; refusing to register with backend")
            raise
        log.info("stm32 sync complete")

        # 2) Register. Capabilities is "what did we learn from a healthy
        # STM32" — today that's just "sync worked". A future CMD_INFO
        # (firmware version, hw revision, sensor inventory) would land here.
        capabilities = {"stm32_synced": True}
        self._backend = BackendClient(self._cfg.backend_url)
        device_id = await self._backend.register(
            self._cfg.registration_token, capabilities,
        )
        # Registration is a one-shot call. Close the HTTP pool now instead
        # of holding it idle for the rest of the process lifetime.
        await self._backend.close()
        self._backend = None
        log.info("registered with backend", extra={"device_id": device_id})

        # 3) DeviceState is only constructable after registration (device_id
        # is required + set-once). Seed stm32_state from what we just
        # observed — the first heartbeat will publish SYNCED to the backend.
        self._state = DeviceState(
            device_id=device_id,
            stm32_state=common_pb2.Stm32State.STM32_STATE_SYNCED,
        )

        # 4) NATS.
        self._nats = NatsClient(self._cfg.nats_url)
        await self._nats.open()

        # 5) Heartbeat first: the initial publish is immediate, so by the
        # time the bridges come up the backend already has our current row.
        self._heartbeat = Heartbeat(
            self._nats, self._state,
            interval_s=self._cfg.heartbeat_interval_s,
        )
        self._heartbeat.start()

        # 6) Response bridge — install on StmLink BEFORE the command bridge
        # opens. Otherwise an unsolicited RSP_ERROR / STATUS_REPORT arriving
        # in the window between "command bridge subscribes" and "response
        # bridge attaches" would hit the noop hook and be silently dropped.
        self._response_bridge = ResponseBridge(self._nats, self._state)
        self._response_bridge.install_on(self._stm)

        # 7) Command bridge — now cloud lifecycle + runtime commands can flow.
        self._command_bridge = CommandBridge(
            self._nats, self._stm, self._state, self._response_bridge,
        )
        await self._command_bridge.open()

        # 8) OTA bridge.
        self._ota_bridge = OtaBridge(self._nats, self._state)
        await self._ota_bridge.open()

        log.info("agent up", extra={"device_id": device_id})

    # ── Shutdown ───────────────────────────────────────────────────

    async def _wait_for_shutdown_signal(self) -> None:
        """Block until SIGTERM or SIGINT."""
        loop = asyncio.get_running_loop()
        stop: asyncio.Future[int] = loop.create_future()

        def _on_signal(sig: int) -> None:
            if not stop.done():
                stop.set_result(sig)

        for sig in (signal.SIGTERM, signal.SIGINT):
            loop.add_signal_handler(sig, _on_signal, sig)

        received = await stop
        log.info("shutdown signal received", extra={"signal": received})

    async def _shutdown(self) -> None:
        """Graceful teardown, roughly the reverse of startup.

        Ordering matters here:

          1. Drain the bridges — refuse new cloud messages and wait for
             any in-flight handler to finish. Handlers still need to write
             their ACK forwards back to NATS, so the NATS connection must
             remain open during this phase.
          2. Heartbeat.stop() — cancels the periodic task, then publishes
             one final OFFLINE StatusEnvelope so the backend sees us go
             down before its last_seen_at staleness check would fire.
          3. Close NATS — unsubscribes any leftover consumers and drains
             the connection.
          4. Close STM — do this last so any RSP_ACK waiter that a
             draining handler was still awaiting has a chance to resolve
             (StmLink.close() cancels the reader and fails pending ACKs).

        Each step is independently guarded so a failure in one doesn't
        strand a subsequent component open.
        """
        if self._command_bridge is not None:
            try:
                await self._command_bridge.drain()
            except Exception:
                log.exception("command bridge drain failed")
            self._command_bridge = None

        if self._ota_bridge is not None:
            try:
                await self._ota_bridge.drain()
            except Exception:
                log.exception("ota bridge drain failed")
            self._ota_bridge = None

        if self._heartbeat is not None:
            try:
                await self._heartbeat.stop()
            except Exception:
                log.exception("heartbeat stop failed")
            self._heartbeat = None

        if self._nats is not None:
            try:
                await self._nats.close()
            except Exception:
                log.exception("nats close failed")
            self._nats = None

        if self._stm is not None:
            try:
                await self._stm.close()
            except Exception:
                log.exception("stm close failed")
            self._stm = None

        if self._backend is not None:
            try:
                await self._backend.close()
            except Exception:
                log.exception("backend close failed")
            self._backend = None

        log.info("agent shutdown complete")


# ── Entrypoint ─────────────────────────────────────────────────────

async def main() -> int:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s %(message)s",
    )
    cfg = AgentConfig.from_env()
    agent = Agent(cfg)
    try:
        await agent.run()
    except SyncError:
        # STM32 sync is the one failure mode with a distinct exit code:
        # provisioning can then decide whether to power-cycle the DUT or
        # page an operator. Everything else exits 1 by default.
        return 2
    return 0
