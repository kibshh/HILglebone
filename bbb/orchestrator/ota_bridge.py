"""OTA (Over-The-Air firmware update) handler.

Subscribes to ``device.{device_id}.ota`` on the COMMANDS stream (same stream
that carries commands, different subject). For each OtaEnvelope:

1. Validate device_id (routing sanity) and session_id (must match current
   if we're in a session).
2. Stream the firmware from ``firmware_url`` (a short-lived MinIO presigned
   URL) into a local temp file, computing SHA-256 in the same pass.
3. Verify the downloaded blob's size and SHA-256 against the envelope.
4. Hand the verified file off to the flasher.

Flashing itself is a stub in this scaffold (logs "would flash"). The real
implementation invokes the DUT's programmer — ``st-flash``, ``JLinkExe``,
``openocd`` — which is hardware-specific and lives outside this class.
"""
from __future__ import annotations

import asyncio
import hashlib
import logging
from pathlib import Path

import httpx
from nats.js.api import DeliverPolicy

from comms import NatsClient
from hilglebone.v1 import ota_pb2
from orchestrator.state import DeviceState

log = logging.getLogger(__name__)

OTA_SUBJECT_FMT = "device.{device_id}.ota"

# Big firmware images (nRF52840, ESP32) can be a few MB but typically flash in
# a couple of seconds over the MinIO link; the timeout is generous so a slow
# LAN can't produce false failures.
DOWNLOAD_TIMEOUT_S: float = 300.0
DOWNLOAD_CHUNK: int = 64 * 1024

# Persistent firmware store on the BBB. Standard FHS location for stateful
# application data; provisioning owns creating it with the correct ownership
# (systemd `StateDirectory=hilglebone` handles this in the unit file).
# Overridable via the constructor for tests and for hosts that pin state to
# a different mount.
DEFAULT_FIRMWARE_DIR = Path("/var/lib/hilglebone/firmware")

# Suffix for in-flight downloads so a crash or verification failure never
# leaves a "looks-complete" firmware file behind that a future flash could
# mistake for a good image.
_TMP_SUFFIX = ".part"


class OtaBridge:
    """Receives OtaEnvelope, downloads firmware, verifies, hands to flasher."""

    def __init__(
        self,
        nats: NatsClient,
        state: DeviceState,
        firmware_dir: Path = DEFAULT_FIRMWARE_DIR,
    ) -> None:
        if not state.device_id:
            raise ValueError("state.device_id is empty")
        self._nats = nats
        self._state = state
        self._firmware_dir = firmware_dir

    # ── Lifecycle ──────────────────────────────────────────────────

    async def open(self) -> None:
        """Attach the NATS subscription. Same DeliverPolicy rules as CommandBridge."""
        self._firmware_dir.mkdir(parents=True, exist_ok=True)
        subject = OTA_SUBJECT_FMT.format(device_id=self._state.device_id)
        durable = f"bbb-{self._state.device_id}-ota"
        await self._nats.subscribe(
            subject,
            self._on_message,
            durable=durable,
            deliver_policy=DeliverPolicy.ALL,
        )
        log.info(
            "ota bridge open",
            extra={"subject": subject, "firmware_dir": str(self._firmware_dir)},
        )

    # ── NATS handler ───────────────────────────────────────────────

    async def _on_message(self, msg) -> None:  # noqa: ANN001  (NATS Msg)
        try:
            env = ota_pb2.OtaEnvelope.FromString(msg.data)
        except Exception:
            log.exception("OtaEnvelope decode failed; dropping")
            await msg.ack()
            return

        # Sanity: device_id routing is enforced by the subject, but a
        # misrouted publish would still land here.
        if env.device_id != self._state.device_id:
            log.warning(
                "OTA on our subject but wrong device_id; dropping",
                extra={"env_device": env.device_id, "self": self._state.device_id},
            )
            await msg.ack()
            return

        # Session gate: OTA must be for our currently-assigned session.
        # The BBB learns its session from SessionStartCommand at allocation
        # time; the cloud is responsible for ordering allocate → OTA so that
        # SessionStart always reaches the BBB first. In the frontend flow
        # that ordering comes naturally (user uploads firmware between the
        # two events); for automated flows the cloud can either add a short
        # delay or poll the BBB's heartbeat until `current_session_id`
        # reflects the new session before publishing OTA.
        if env.session_id != self._state.session_id:
            log.warning(
                "OTA session_id does not match current session; dropping",
                extra={"current": self._state.session_id, "requested": env.session_id},
            )
            await msg.ack()
            return

        try:
            await self._handle(env)
        except Exception:
            log.exception("OTA handler raised")
        finally:
            await msg.ack()

    # ── OTA workflow ───────────────────────────────────────────────

    async def _handle(self, env: ota_pb2.OtaEnvelope) -> None:
        log.info(
            "OTA received",
            extra={
                "message_id": env.message_id,
                "session_id": env.session_id,
                "firmware_size": env.firmware_size,
                "firmware_sha256": env.firmware_sha256,
            },
        )

        dest = self._firmware_dir / f"{env.message_id}.bin"
        tmp = dest.with_suffix(dest.suffix + _TMP_SUFFIX)

        try:
            actual_sha256, actual_size = await self._download(env.firmware_url, tmp)
        except Exception:
            log.exception("firmware download failed", extra={"url": env.firmware_url})
            tmp.unlink(missing_ok=True)
            return

        if actual_size != env.firmware_size:
            log.error(
                "firmware size mismatch; refusing to flash",
                extra={"expected": env.firmware_size, "actual": actual_size},
            )
            tmp.unlink(missing_ok=True)
            return
        if actual_sha256 != env.firmware_sha256:
            log.error(
                "firmware sha256 mismatch; refusing to flash",
                extra={"expected": env.firmware_sha256, "actual": actual_sha256},
            )
            tmp.unlink(missing_ok=True)
            return

        # Atomic promotion: the final path only exists once the bytes have
        # been fully written AND verified, so a crash between download and
        # verify never leaves a good-looking image at the target path.
        # Path.replace maps to os.replace — atomic on POSIX, overwrites dest.
        tmp.replace(dest)

        log.info(
            "firmware verified",
            extra={"path": str(dest), "size": actual_size, "sha256": actual_sha256},
        )
        await self._flash(dest)

    async def _download(self, url: str, tmp: Path) -> tuple[str, int]:
        """Stream url → tmp while computing SHA-256. Single pass, chunked.

        Writes to a `.part` file so the final path never contains a partial
        image; the caller renames after size + SHA-256 verification passes.
        """
        hasher = hashlib.sha256()
        size = 0
        async with httpx.AsyncClient(timeout=DOWNLOAD_TIMEOUT_S) as client:
            async with client.stream("GET", url) as resp:
                resp.raise_for_status()
                # Open the file only after the server is happy; if the URL
                # was expired or the auth failed we don't leave an empty
                # tmp file behind.
                with tmp.open("wb") as f:
                    async for chunk in resp.aiter_bytes(DOWNLOAD_CHUNK):
                        f.write(chunk)
                        hasher.update(chunk)
                        size += len(chunk)
        return hasher.hexdigest(), size

    async def _flash(self, firmware_path: Path) -> None:
        """Scaffold: log-only. Real impl invokes the DUT's programmer.

        The programmer name (stlink / jlink / openocd-rpi) lives in the
        ``dut_devices.programmer`` DB column; a future orchestrator will
        thread it into the BBB via config / registration response so this
        method can dispatch to the correct subprocess.
        """
        log.info(
            "would flash firmware (stub — no programmer wired in yet)",
            extra={"path": str(firmware_path)},
        )
        # Simulate a bit of "flashing" time so downstream code can observe
        # a distinct 'flashing' phase in the future.
        await asyncio.sleep(0)
