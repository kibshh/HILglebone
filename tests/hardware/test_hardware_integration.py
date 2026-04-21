#!/usr/bin/env python3
"""Hardware integration test: BBB sends command sequence, verifies ACK from STM32.

Requires:
  - STM32 flashed and powered
  - BBB UART TX/RX wired to STM32 PA9/PA10 (with GND shared)

Usage:
    python3 tests/hardware/test_hardware_integration.py --port /dev/ttyS1 --baud 115200 --verbose
"""
from __future__ import annotations

import argparse
import asyncio
import sys
import time
from pathlib import Path

# Allow running from repo root or from tests/hardware/.
_REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(_REPO_ROOT / "bbb"))

from comms import CommandError, StmLink, SyncError
from protocol import (
    FrameType,
    ParseError,
    ProtocolId,
)
from protocol.constants import I2cAddressMode
from protocol.i2c import I2cSensorConfig, pack_i2c_cfg, pack_i2c_set_output

# ── Test helpers ───────────────────────────────────────────────────

PASS = "\033[32mPASS\033[0m"
FAIL = "\033[31mFAIL\033[0m"

_verbose = False


def log(msg: str) -> None:
    if _verbose:
        print(f"  {msg}")


def result(name: str, ok: bool, elapsed_ms: float, detail: str = "") -> None:
    status = PASS if ok else FAIL
    suffix = f"  ({detail})" if detail else ""
    print(f"  [{status}]  {name}  {elapsed_ms:.1f} ms{suffix}")


# ── Test cases ─────────────────────────────────────────────────────

async def run_tests(port: str, baud: int) -> int:
    """Run the full integration sequence. Returns number of failures."""
    failures = 0

    print(f"\nHILglebone hardware integration test")
    print(f"Port: {port}  Baud: {baud}\n")

    # Wire parse/unsolicited events to verbose log
    def _on_parse_error(e: ParseError) -> None:
        log(f"parse error: {e.reason.name}  ft=0x{e.frame_type:02X}  seq={e.seq}")

    def _on_unsolicited(f) -> None:
        log(f"unsolicited: type=0x{f.type:02X}  seq={f.seq}")

    try:
        link = StmLink(port, baud)
        link.on_parse_error = _on_parse_error
        link.on_unsolicited_frame = _on_unsolicited
        link.open()
    except Exception as exc:
        print(f"  [{FAIL}]  Cannot open {port}: {exc}")
        return 1

    try:
        # ── 1. CMD_SYNC ───────────────────────────────────────────
        print("1. CMD_SYNC handshake")
        t0 = time.perf_counter()
        try:
            await link.sync(retries=5, timeout_s=1.0)
            elapsed = (time.perf_counter() - t0) * 1000
            result("CMD_SYNC", True, elapsed)
        except SyncError as exc:
            elapsed = (time.perf_counter() - t0) * 1000
            result("CMD_SYNC", False, elapsed, str(exc))
            failures += 1
            print("\n  STM32 did not respond. Aborting remaining tests.")
            return failures

        # ── 2. CMD_SETUP_SENSOR ───────────────────────────────────
        print("2. CMD_SETUP_SENSOR (I2C, addr=0x48, 100 kHz)")
        cfg = I2cSensorConfig(
            clock_hz=100_000,
            address_mode=I2cAddressMode.MODE_7BIT,
            primary_addr=0x48,
            register_count=2,
        )
        t0 = time.perf_counter()
        try:
            ack = await link.send_command(
                FrameType.CMD_SETUP_SENSOR,
                bytes([ProtocolId.I2C]) + pack_i2c_cfg(cfg),
            )
            elapsed = (time.perf_counter() - t0) * 1000
            sensor_id = ack.sensor_id
            if sensor_id == 0:
                raise CommandError("SETUP_SENSOR returned sensor_id=0")
            result("CMD_SETUP_SENSOR", True, elapsed, f"sensor_id={sensor_id}")
        except CommandError as exc:
            elapsed = (time.perf_counter() - t0) * 1000
            result("CMD_SETUP_SENSOR", False, elapsed, str(exc))
            failures += 1
            print("\n  SETUP failed — cannot continue SET_OUTPUT / STOP tests.")
            return failures

        # ── 3. CMD_SET_OUTPUT ─────────────────────────────────────
        print(f"3. CMD_SET_OUTPUT  (sensor_id={sensor_id})")
        values = pack_i2c_set_output(reg_start=0x00, values=b"\xA5\x5A")
        t0 = time.perf_counter()
        try:
            ack = await link.send_command(
                FrameType.CMD_SET_OUTPUT,
                bytes([sensor_id]) + values,
            )
            elapsed = (time.perf_counter() - t0) * 1000
            if ack.sensor_id != sensor_id:
                raise CommandError(
                    f"SET_OUTPUT: ACK sensor_id={ack.sensor_id}, expected {sensor_id}"
                )
            result("CMD_SET_OUTPUT", True, elapsed)
        except CommandError as exc:
            elapsed = (time.perf_counter() - t0) * 1000
            result("CMD_SET_OUTPUT", False, elapsed, str(exc))
            failures += 1

        # ── 4. CMD_STOP_SENSOR ────────────────────────────────────
        print(f"4. CMD_STOP_SENSOR (sensor_id={sensor_id})")
        t0 = time.perf_counter()
        try:
            ack = await link.send_command(
                FrameType.CMD_STOP_SENSOR,
                bytes([sensor_id]),
            )
            elapsed = (time.perf_counter() - t0) * 1000
            if ack.sensor_id != sensor_id:
                raise CommandError(
                    f"STOP_SENSOR: ACK sensor_id={ack.sensor_id}, expected {sensor_id}"
                )
            result("CMD_STOP_SENSOR", True, elapsed)
        except CommandError as exc:
            elapsed = (time.perf_counter() - t0) * 1000
            result("CMD_STOP_SENSOR", False, elapsed, str(exc))
            failures += 1

    except Exception as exc:
        print(f"\n  [{FAIL}]  Unexpected error: {exc}")
        failures += 1
    finally:
        link.close()

    # ── Summary ───────────────────────────────────────────────────
    total = 4
    passed = total - failures
    print(f"\n{'─' * 40}")
    print(f"  Result: {passed}/{total} passed", end="")
    if failures == 0:
        print(f"  {PASS}")
    else:
        print(f"  {FAIL}")
    print()

    return failures


# ── Entry point ────────────────────────────────────────────────────

def main() -> None:
    global _verbose

    parser = argparse.ArgumentParser(
        description="HILglebone hardware integration test (BBB ↔ STM32)"
    )
    parser.add_argument(
        "--port", default="/dev/ttyS1",
        help="Serial port connected to STM32 (default: /dev/ttyS1)",
    )
    parser.add_argument(
        "--baud", type=int, default=115_200,
        help="Baud rate (default: 115200)",
    )
    parser.add_argument(
        "--verbose", "-v", action="store_true",
        help="Print parse errors and unsolicited frames",
    )
    args = parser.parse_args()
    _verbose = args.verbose

    failures = asyncio.run(run_tests(args.port, args.baud))
    sys.exit(0 if failures == 0 else 1)


if __name__ == "__main__":
    main()
