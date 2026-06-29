"""Periodic device.{device_id}.status heartbeat.

Publishes a StatusEnvelope to the cloud at a fixed interval so the backend's
natssub subscriber can keep the bbb_devices row reconciled (`status`,
`last_seen_at`, `current_session_id`, `stm32_state`). The caller updates the
self-reported state via setters; the next heartbeat picks up the new value.

Lifecycle:

    hb = Heartbeat(nats, device_id, interval_s=30)
    hb.start()                 # schedule the periodic task
    hb.set_stm32_state(SYNCED) # values picked up on next tick
    hb.set_session(session_id) # ...
    await hb.stop()            # cancel + best-effort final OFFLINE publish
"""
from __future__ import annotations

import asyncio
import logging
import time

from comms import NatsClient
from hilglebone.v1 import common_pb2, status_pb2

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
        device_id: str,
        *,
        interval_s: float = DEFAULT_INTERVAL_S,
    ) -> None:
        if not device_id:
            raise ValueError("device_id is empty")
        if interval_s <= 0:
            raise ValueError("interval_s must be > 0")
        self._nats = nats
        self._device_id = device_id
        self._interval_s = interval_s

        # Self-reported state. Defaults: online, no session, STM32 unknown.
        self._session_id: str = ""
        self._online_status = status_pb2.OnlineStatus.ONLINE_STATUS_ONLINE
        self._stm32_state = common_pb2.Stm32State.STM32_STATE_UNSPECIFIED

        self._task: asyncio.Task | None = None
        self._subject = f"device.{device_id}.status"

    # ── state setters (called from the orchestrator main loop) ─────

    def set_session(self, session_id: str) -> None:
        self._session_id = session_id

    def set_online_status(self, value: status_pb2.OnlineStatus.ValueType) -> None:
        self._online_status = value

    def set_stm32_state(self, value: common_pb2.Stm32State.ValueType) -> None:
        self._stm32_state = value

    # ── lifecycle ──────────────────────────────────────────────────

    def start(self) -> None:
        """Schedule the periodic task. No-op if already started."""
        if self._task is not None:
            return
        self._task = asyncio.create_task(self._run(), name=f"heartbeat-{self._device_id}")
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
                online_status=status_pb2.OnlineStatus.ONLINE_STATUS_OFFLINE,
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
        online_status: status_pb2.OnlineStatus.ValueType | None = None,
    ) -> None:
        env = status_pb2.StatusEnvelope(
            device_id=self._device_id,
            session_id=self._session_id,
            timestamp_us=int(time.time() * 1_000_000),
            online_status=online_status if online_status is not None else self._online_status,
            stm32_state=self._stm32_state,
        )
        await self._nats.publish(self._subject, env.SerializeToString())
