"""Single source of truth for the BBB's self-reported lifecycle state.

``DeviceState`` is the in-memory record of "what is this device currently
doing", split out from any one component so:

* The heartbeat (a *publisher*) can read it without owning it.
* The command bridge (a *mutator* on cloud lifecycle messages) writes
  ``session_id`` and ``online_status`` here.
* The response bridge stamps telemetry envelopes from ``session_id`` here.
* The orchestrator main loop sets ``stm32_state`` after the STM32
  handshake result is known.

There is exactly one ``DeviceState`` per running agent; everyone reads from
and writes to that instance. Python attribute access is atomic and asyncio
is single-threaded so no locking is required.
"""
from __future__ import annotations

from dataclasses import dataclass

from hilglebone.v1 import common_pb2, status_pb2


@dataclass
class DeviceState:
    """Shared state + identity for the running BBB agent.

    ``device_id`` is the BBB's identity, assigned by the backend at
    registration. It is documented as set-once even though Python doesn't
    enforce it; nothing in the agent should overwrite it after construction.

    The remaining fields are *mutable* state that components write as the
    BBB transitions through its lifecycle.
    """

    # Identity (set once at registration; do not mutate afterwards).
    device_id: str

    # Empty string means "not currently in any session".
    session_id: str = ""

    # Default until the orchestrator decides otherwise.
    online_status: int = status_pb2.OnlineStatus.ONLINE_STATUS_ONLINE

    # Default UNSPECIFIED until the boot handshake either succeeds (SYNCED)
    # or fails (UNSYNCED / ERROR).
    stm32_state: int = common_pb2.Stm32State.STM32_STATE_UNSPECIFIED
