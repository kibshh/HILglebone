"""Async NATS client for the BeagleBone agent.

Owns a single connection to the cloud NATS broker. Wraps `nats-py` so the
rest of the agent doesn't have to think about reconnect logic or JetStream
context lifetime.

Usage:

    client = NatsClient("nats://localhost:4222")
    await client.open()
    try:
        await client.subscribe("session.X.command", handler, durable="bbb-X")
        await client.publish("device.X.telemetry", payload, msg_id=uuid4().hex)
    finally:
        await client.close()

The client subscribes via JetStream so messages survive disconnects, and
publishes via JetStream so the broker acks persistence (`PublishAck`) before
the call returns. A returned ack means the message hit the stream; a raised
exception means it did not.
"""
from __future__ import annotations

import logging
import uuid
from collections.abc import Awaitable, Callable

import nats
from nats.aio.client import Client as NatsConn
from nats.aio.msg import Msg
from nats.js import JetStreamContext
from nats.js.api import ConsumerConfig, DeliverPolicy

# ── Configuration ──────────────────────────────────────────────────

# Reconnect tuning. nats-py handles the loop; we only pick the cadence.
RECONNECT_WAIT_S: float          = 2.0
MAX_RECONNECT_ATTEMPTS: int      = -1     # -1 = retry forever

# Per-publish wall-clock budget. JetStream PublishMsg blocks waiting for the
# broker ack; this caps how long a flaky broker can stall a producer.
PUBLISH_TIMEOUT_S: float         = 3.0

# Per-message processing budget. JetStream redelivers if a consumer doesn't
# ack within this window — important so a hung handler can't permanently lose
# a message.
ACK_WAIT_S: float                = 30.0

# ── Types ──────────────────────────────────────────────────────────

MessageHandler = Callable[[Msg], Awaitable[None]]
"""Async function the caller passes to subscribe(). It must `await msg.ack()`
explicitly when processing succeeds — auto-ack would force a policy decision
the client can't make in general."""

log = logging.getLogger(__name__)


# ── Errors ─────────────────────────────────────────────────────────

class NotOpenError(RuntimeError):
    """Raised when subscribe/publish are called before open() (or after close())."""


# ── Client ─────────────────────────────────────────────────────────

class NatsClient:
    """Lifecycle wrapper around nats-py with JetStream.

    One instance, one underlying TCP connection. open() must be awaited before
    any other method; close() drains the connection.
    """

    def __init__(self, url: str) -> None:
        if not url:
            raise ValueError("nats url is empty")
        self._url: str = url
        self._nc: NatsConn | None = None
        self._js: JetStreamContext | None = None
        self._subs: list = []

    async def open(self) -> None:
        """Connect to the broker and prepare a JetStream context."""
        if self._nc is not None:
            return
        self._nc = await nats.connect(
            self._url,
            reconnect_time_wait=RECONNECT_WAIT_S,
            max_reconnect_attempts=MAX_RECONNECT_ATTEMPTS,
            disconnected_cb=self._on_disconnected,
            reconnected_cb=self._on_reconnected,
            closed_cb=self._on_closed,
            error_cb=self._on_error,
        )
        self._js = self._nc.jetstream()
        log.info("nats open", extra={"url": self._url})

    async def close(self) -> None:
        """Unsubscribe and drain. Idempotent."""
        if self._nc is None:
            return
        for sub in self._subs:
            try:
                await sub.unsubscribe()
            except Exception:
                log.exception("nats unsubscribe failed")
        try:
            await self._nc.drain()
        except Exception:
            log.exception("nats drain failed")
        finally:
            self._nc = None
            self._js = None
            self._subs.clear()
            log.info("nats closed")

    async def subscribe(
        self,
        subject: str,
        handler: MessageHandler,
        *,
        durable: str | None = None,
        deliver_policy: DeliverPolicy = DeliverPolicy.NEW,
    ) -> None:
        """Subscribe to a JetStream subject.

        durable: name of the durable consumer. Pass to survive client restarts;
        leave None for an ephemeral consumer (loses state on reconnect, fine
        for status/heartbeat-style traffic).
        """
        if self._js is None:
            raise NotOpenError("client is not open")
        config = ConsumerConfig(
            durable_name=durable,
            deliver_policy=deliver_policy,
            ack_wait=ACK_WAIT_S,
        )
        sub = await self._js.subscribe(subject, cb=handler, durable=durable, config=config)
        self._subs.append(sub)
        log.info("nats subscribe", extra={"subject": subject, "durable": durable})

    async def publish(
        self,
        subject: str,
        payload: bytes,
        *,
        msg_id: str | None = None,
    ) -> None:
        """Publish via JetStream, waiting for the broker's persistence ack.

        msg_id is sent as the Nats-Msg-Id header so JetStream can deduplicate
        retried publishes server-side. If omitted, a fresh UUID is generated —
        callers that want app-level dedup should pass their own.
        """
        if self._js is None:
            raise NotOpenError("client is not open")
        if msg_id is None:
            msg_id = uuid.uuid4().hex
        await self._js.publish(
            subject,
            payload,
            headers={"Nats-Msg-Id": msg_id},
            timeout=PUBLISH_TIMEOUT_S,
        )

    # ── Connection-state callbacks ─────────────────────────────────

    async def _on_disconnected(self) -> None:
        log.warning("nats disconnected")

    async def _on_reconnected(self) -> None:
        log.info("nats reconnected", extra={"url": self._nc.connected_url.netloc if self._nc else None})

    async def _on_closed(self) -> None:
        log.info("nats connection closed")

    async def _on_error(self, e: Exception) -> None:
        log.error("nats error: %r", e)
