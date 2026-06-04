# NATS JetStream Streams

Three persistent streams cover the full command and telemetry path between the
cloud backend and the BeagleBone agents.

---

## Subject hierarchy

```
session.{session_id}.command   cloud → BBB   sensor configuration and control
session.{session_id}.ota       cloud → BBB   firmware delivery trigger
device.{device_id}.telemetry   BBB → cloud   ACKs, error responses, sensor readings
device.{device_id}.status      BBB → cloud   heartbeat: online state, current session
```

`session_id` and `device_id` are opaque UUIDs assigned by the backend.

---

## Streams

### COMMANDS

| Property              | Value                                   |
|-----------------------|-----------------------------------------|
| Subjects              | `session.*.command`, `session.*.ota`    |
| Retention             | workqueue                               |
| Storage               | file                                    |
| Max age               | 1 hour                                  |
| Dedup window          | 2 minutes                               |

**Workqueue retention** means a message is deleted from the stream as soon as
one consumer ACKs it.  This is the right model for commands: there is exactly
one BBB per session, and once the BBB has processed the command there is no
reason to keep it.

The 1-hour max-age evicts any command the BBB never consumed (e.g. the device
went offline before delivery).  The backend treats an undelivered command as
failed and returns an error to the frontend.

Both `session.*.command` and `session.*.ota` live in the same stream so a
single durable consumer on the BBB side can pull both without managing two
subscriptions.

### TELEMETRY

| Property              | Value                                   |
|-----------------------|-----------------------------------------|
| Subjects              | `device.*.telemetry`                    |
| Retention             | limits                                  |
| Storage               | file                                    |
| Max age               | 24 hours                                |
| Dedup window          | 2 minutes                               |

Limits retention keeps messages until they age out or the stream hits a byte or
message cap (none configured — only max-age applies).  The backend pushes
telemetry to the WebSocket client in real time and also persists it to
PostgreSQL, so 24 hours of JetStream buffer is more than enough for any
reconnect scenario.

### STATUS

| Property              | Value                                   |
|-----------------------|-----------------------------------------|
| Subjects              | `device.*.status`                       |
| Retention             | limits                                  |
| Storage               | file                                    |
| Max age               | 24 hours                                |
| Max msgs per subject  | 1                                       |

Each device publishes a heartbeat every ~30 seconds.  `max-msgs-per-subject=1`
ensures JetStream keeps only the latest status for each device — querying
current state never requires scanning a history.  The 24-hour max-age preserves
the last known status for debugging even after a device goes silent; liveness is
determined by the backend comparing the message timestamp to wall clock rather
than relying on the message expiring.

---

## Consumer pattern

The BBB agent uses a **durable pull consumer** on COMMANDS, bound to its own
`session_id`.  Pull consumers let the agent control the receive rate and handle
backpressure naturally during heavy workloads.

The backend uses an **ephemeral push consumer** on TELEMETRY and STATUS,
delivering messages to an in-process subscription.  Push consumers are
sufficient here because the backend is always online and the subjects are
append-only from the BBB side.

---

## Setup

```bash
# Start the NATS container
docker compose -f backend/docker-compose.yml up -d nats

# Create / update streams (requires nats CLI)
backend/infra/nats/setup-streams.sh
```

The script is idempotent: running it against an existing deployment updates
stream config rather than failing.
