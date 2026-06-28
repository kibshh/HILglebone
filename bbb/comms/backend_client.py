"""HTTP client for the cloud backend.

Owns one httpx.AsyncClient. Long-lived: opened at agent startup, closed at
shutdown. Today only exposes register() — the BBB's boot handshake.
Subsequent calls (firmware download, future device-API endpoints) will hang
off the same client and share its connection pool.
"""
from __future__ import annotations

import logging
from typing import Any

import httpx

# ── Configuration ──────────────────────────────────────────────────

REGISTER_PATH: str          = "/api/v1/devices/register"
REQUEST_TIMEOUT_S: float    = 10.0

log = logging.getLogger(__name__)


# ── Errors ─────────────────────────────────────────────────────────

class RegistrationError(Exception):
    """Backend rejected the registration or the request could not complete.

    Includes a `status` attribute (None on transport failures) so callers can
    decide whether to retry: 401 is fatal (bad token), 5xx is worth retrying.
    """

    def __init__(self, message: str, status: int | None = None) -> None:
        super().__init__(message)
        self.status: int | None = status


# ── Client ─────────────────────────────────────────────────────────

class BackendClient:
    """Async HTTP client to the cloud backend."""

    def __init__(self, base_url: str) -> None:
        if not base_url:
            raise ValueError("base_url is empty")
        self._client = httpx.AsyncClient(
            base_url=base_url,
            timeout=REQUEST_TIMEOUT_S,
        )

    async def close(self) -> None:
        """Close the underlying connection pool."""
        await self._client.aclose()

    async def register(
        self,
        token: str,
        capabilities: dict[str, Any] | None = None,
    ) -> str:
        """POST /api/v1/devices/register. Return device_id on success.

        Raises RegistrationError on any non-2xx response or transport failure.
        Inspect .status to choose retry policy (None or 5xx → retry,
        401 → operator intervention required).
        """
        if not token:
            raise ValueError("token is empty")

        body = {"token": token, "capabilities": capabilities or {}}
        try:
            resp = await self._client.post(REGISTER_PATH, json=body)
        except httpx.HTTPError as e:
            raise RegistrationError(f"register transport failure: {e}") from e

        if resp.status_code != 200:
            raise RegistrationError(
                f"backend returned {resp.status_code}: {resp.text[:200]}",
                status=resp.status_code,
            )

        try:
            data = resp.json()
        except ValueError as e:
            raise RegistrationError(f"register response not JSON: {e}") from e

        device_id = data.get("device_id")
        if not device_id:
            raise RegistrationError("register response missing device_id")

        log.info("registered with backend: device_id=%s", device_id)
        return device_id
