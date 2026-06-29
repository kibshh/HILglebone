#!/usr/bin/env bash
# Creates or updates the three JetStream streams used by HILglebone.
# Run after the NATS container is up: ./setup-streams.sh
# Requires the 'nats' CLI to be installed (https://github.com/nats-io/natscli).

set -euo pipefail

NATS_URL="${NATS_URL:-nats://localhost:4222}"

echo "Connecting to ${NATS_URL}..."

# upsert_stream <name> <add-args...>
# Tries `stream add`; falls back to `stream edit` with the same args on conflict.
upsert_stream() {
    local name="$1"
    shift
    if nats --server "${NATS_URL}" stream add "${name}" "$@" 2>/dev/null; then
        echo "Created stream: ${name}"
    else
        nats --server "${NATS_URL}" stream edit "${name}" "$@" 2>/dev/null
        echo "Updated stream: ${name}"
    fi
}

# ── COMMANDS stream ────────────────────────────────────────────────────────────
# Subjects: device.*.command  (cloud → BBB sensor / lifecycle commands)
#           device.*.ota      (cloud → BBB firmware delivery)
#
# Routing is keyed by device_id, not session_id: each BBB has its own subject,
# so the workqueue stream's "delivered to exactly one consumer" semantics map
# cleanly onto "delivered to exactly one BBB."
#
# Workqueue retention: each message is delivered to exactly one consumer and
# deleted on ACK.  No point storing commands after they've been executed.
# 1-hour max-age evicts any command that was never consumed (e.g. BBB offline).

upsert_stream COMMANDS \
    --subjects "device.*.command,device.*.ota" \
    --retention work \
    --storage file \
    --replicas 1 \
    --max-age 1h \
    --max-msgs -1 \
    --max-bytes -1 \
    --max-msg-size -1 \
    --discard old \
    --dupe-window 2m \
    --no-allow-rollup

# ── TELEMETRY stream ───────────────────────────────────────────────────────────
# Subject: device.*.telemetry  (BBB → cloud sensor readings and ACKs)
#
# Limits retention: keep the last 24 h of telemetry per stream (not per device).
# The backend reads and forwards to WebSocket; long-term storage goes to the DB.

upsert_stream TELEMETRY \
    --subjects "device.*.telemetry" \
    --retention limits \
    --storage file \
    --replicas 1 \
    --max-age 24h \
    --max-msgs -1 \
    --max-bytes -1 \
    --max-msg-size -1 \
    --discard old \
    --dupe-window 2m \
    --no-allow-rollup

# ── STATUS stream ──────────────────────────────────────────────────────────────
# Subject: device.*.status  (BBB heartbeat: online, current session, STM32 state)
#
# Limits retention with max-msgs-per-subject=1: each device keeps only its
# latest status message.  24-hour max-age preserves the last known state for
# debug purposes; the backend determines liveness by comparing message timestamp
# to wall clock, not by whether the message exists.

upsert_stream STATUS \
    --subjects "device.*.status" \
    --retention limits \
    --storage file \
    --replicas 1 \
    --max-age 24h \
    --max-msgs-per-subject 1 \
    --max-msgs -1 \
    --max-bytes -1 \
    --max-msg-size -1 \
    --discard old \
    --no-allow-rollup

echo "Done."
