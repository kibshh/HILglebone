# NATS JetStream Streams

Three persistent streams cover the full command and telemetry path between the
cloud backend and the BeagleBone agents.

---

## Subject hierarchy

```
device.{device_id}.command     cloud → BBB   sensor / lifecycle commands
device.{device_id}.ota         cloud → BBB   firmware delivery trigger
device.{device_id}.telemetry   BBB → cloud   ACKs, error responses, sensor readings
device.{device_id}.status      BBB → cloud   heartbeat: online state, current session
```

All subjects are keyed by `device_id`. Session is carried in the envelope
body (so the BBB can validate against its current session) but is not used
for routing — keeping the routing key device-scoped means each BBB has its
own subject and the workqueue stream's "delivered to exactly one consumer"
semantics map cleanly onto "delivered to exactly one BBB."

`device_id` is an opaque UUID assigned by the backend at provisioning.

---

## Streams

### COMMANDS

| Property              | Value                                   |
|-----------------------|-----------------------------------------|
| Subjects              | `device.*.command`, `device.*.ota`      |
| Retention             | workqueue                               |
| Storage               | file                                    |
| Max age               | 1 hour                                  |
| Dedup window          | 2 minutes                               |

**Workqueue retention** means a message is deleted from the stream as soon as
one consumer ACKs it.  This is the right model for commands: each device's
subject is consumed by exactly one BBB, and once that BBB has processed the
command there is no reason to keep it.

The 1-hour max-age evicts any command the BBB never consumed (e.g. the device
went offline before delivery).  The backend treats an undelivered command as
failed and returns an error to the frontend.

Both `device.*.command` and `device.*.ota` live in the same stream so a
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

The BBB agent uses a **durable consumer** on COMMANDS, bound to its own
`device.{device_id}.command` subject (and the matching `.ota`). This pairs
naturally with workqueue retention: each device's subject has exactly one
consumer, so the broker never has to choose between candidates.

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
