"""Periodic device.{device_id}.status heartbeat.

Publishes a StatusEnvelope to the cloud at a fixed interval so the backend's
natssub subscriber can keep the bbb_devices row reconciled (`status`,
`last_seen_at`, `current_session_id`, `stm32_state`). The heartbeat does NOT
own its state — it reads from a shared :class:`DeviceState`. Whoever needs to
change the BBB's self-reported state writes there; the next heartbeat tick
picks it up.

Lifecycle:

    state = DeviceState()
    hb = Heartbeat(nats, device_id, state, interval_s=30)
    hb.start()
    state.session_id = "..."     # next tick will publish this
    await hb.stop()              # cancel + best-effort final OFFLINE
"""
from __future__ import annotations

import asyncio
import logging
import time

from comms import NatsClient
from hilglebone.v1 import status_pb2
from orchestrator.state import DeviceState

log = logging.getLogger(__name__)

# ── Configuration ──────────────────────────────────────────────────

# Heartbeat cadence. Backend's status stream is configured with
# max-msgs-per-subject=1 and 24h max-age, so a slower cadence still keeps the
# row "alive" — but liveness checks rely on `last_seen_at` being recent, so
# don't drift far above ~60s without coordinating with the backend's check.
DEFAULT_INTERVAL_S: float = 30.0


# ── Heartbeat ──────────────────────────────────────────────────────

class Heartbeat:
    """Async task that publishes a StatusEnvelope every `interval_s`."""

    def __init__(
        self,
        nats: NatsClient,
        state: DeviceState,
        *,
        interval_s: float = DEFAULT_INTERVAL_S,
    ) -> None:
        if not state.device_id:
            raise ValueError("state.device_id is empty")
        if interval_s <= 0:
            raise ValueError("interval_s must be > 0")
        self._nats = nats
        self._state = state
        self._interval_s = interval_s

        self._task: asyncio.Task | None = None
        self._subject = f"device.{state.device_id}.status"

    # ── lifecycle ──────────────────────────────────────────────────

    def start(self) -> None:
        """Schedule the periodic task. No-op if already started."""
        if self._task is not None:
            return
        self._task = asyncio.create_task(self._run(), name=f"heartbeat-{self._state.device_id}")
        log.info("heartbeat started", extra={"interval_s": self._interval_s})

    async def stop(self) -> None:
        """Cancel the task and best-effort publish a final OFFLINE status."""
        if self._task is None:
            return
        self._task.cancel()
        try:
            await self._task
        except asyncio.CancelledError:
            pass
        self._task = None
        try:
            await self._publish_once(
                override_online_status=status_pb2.OnlineStatus.ONLINE_STATUS_OFFLINE,
            )
            log.info("heartbeat stopped, OFFLINE status published")
        except Exception:
            # Shutdown path — log and continue. The backend will fall back to
            # last_seen_at staleness once enough time passes.
            log.exception("final OFFLINE heartbeat publish failed")

    # ── publishing ─────────────────────────────────────────────────

    async def _run(self) -> None:
        # First publish is immediate so the backend doesn't need to wait a full
        # interval to see this device come up.
        while True:
            try:
                await self._publish_once()
            except Exception:
                log.exception("heartbeat publish failed")
            await asyncio.sleep(self._interval_s)

    async def _publish_once(
        self,
        *,
        override_online_status: int | None = None,
    ) -> None:
        env = status_pb2.StatusEnvelope(
            device_id=self._state.device_id,
            session_id=self._state.session_id,
            timestamp_us=int(time.time() * 1_000_000),
            online_status=(
                override_online_status
                if override_online_status is not None
                else self._state.online_status
            ),
            stm32_state=self._state.stm32_state,
        )
        await self._nats.publish(self._subject, env.SerializeToString())
