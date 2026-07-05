// Package natssub consumes BBB → cloud messages from NATS:
//
//   - device.*.status     → reconciled into the bbb_devices row
//   - device.*.telemetry  → logged for now; future WebSocket bridging hooks in here
//
// Subscriptions are JetStream push consumers using DeliverLastPerSubject for
// status (so a fresh backend immediately sees each device's last known state)
// and DeliverNew for telemetry (no point replaying historical ACKs/errors).
// Acks are explicit: on parse failure we ack to drop the poison message, and
// on transient DB failure we ack-and-log — the source of truth is the broker's
// next heartbeat, not a retry on this one.
package natssub

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
	"github.com/nats-io/nats.go"
	"google.golang.org/protobuf/encoding/protojson"
	"google.golang.org/protobuf/proto"

	pb "github.com/kibshh/HILglebone/backend/gen/hilglebone/v1"
	"github.com/kibshh/HILglebone/backend/internal/bus"
)

const (
	statusSubject    = "device.*.status"
	telemetrySubject = "device.*.telemetry"
	connectTimeout   = 5 * time.Second
	dbOpTimeout      = 3 * time.Second
)

type Subscriber struct {
	nc   *nats.Conn
	js   nats.JetStreamContext
	pool *pgxpool.Pool
	bus  *bus.Bus
}

// Open connects to NATS, attaches the status and telemetry subscriptions,
// and returns a live Subscriber. The subscriptions begin delivering as soon
// as Open returns; Close drains them. The bus parameter receives JSON-encoded
// events that WebSocket handlers (or other consumers) can fan out.
func Open(url string, pool *pgxpool.Pool, b *bus.Bus) (*Subscriber, error) {
	if url == "" {
		return nil, errors.New("nats url is empty")
	}
	if pool == nil {
		return nil, errors.New("db pool is nil")
	}
	if b == nil {
		return nil, errors.New("bus is nil")
	}
	nc, err := nats.Connect(url,
		nats.Timeout(connectTimeout),
		nats.MaxReconnects(-1),
		nats.ReconnectWait(2*time.Second),
	)
	if err != nil {
		return nil, fmt.Errorf("nats connect: %w", err)
	}
	js, err := nc.JetStream()
	if err != nil {
		nc.Close()
		return nil, fmt.Errorf("jetstream context: %w", err)
	}
	s := &Subscriber{nc: nc, js: js, pool: pool, bus: b}

	if _, err := s.js.Subscribe(statusSubject, s.handleStatus,
		nats.DeliverLastPerSubject(),
		nats.AckExplicit(),
	); err != nil {
		nc.Close()
		return nil, fmt.Errorf("subscribe %s: %w", statusSubject, err)
	}
	if _, err := s.js.Subscribe(telemetrySubject, s.handleTelemetry,
		nats.DeliverNew(),
		nats.AckExplicit(),
	); err != nil {
		nc.Close()
		return nil, fmt.Errorf("subscribe %s: %w", telemetrySubject, err)
	}
	slog.Info("nats subscriber open",
		"status_subject", statusSubject, "telemetry_subject", telemetrySubject)
	return s, nil
}

// Close drains the connection: drops subscriptions, waits for in-flight
// handlers, then closes. Safe to call once.
func (s *Subscriber) Close() {
	if s == nil || s.nc == nil {
		return
	}
	_ = s.nc.Drain()
}

func (s *Subscriber) handleStatus(msg *nats.Msg) {
	var env pb.StatusEnvelope
	if err := proto.Unmarshal(msg.Data, &env); err != nil {
		// Parse failure → no data to broadcast and nothing to persist.
		slog.Error("status unmarshal failed", "error", err, "subject", msg.Subject)
		_ = msg.Ack()
		return
	}
	// Broadcast first: WS clients should see the event even if persistence
	// fails or the envelope is missing fields the DB needs.
	s.publishToBus(&env, "device_status")

	statusStr := mapOnlineStatus(env.OnlineStatus)
	if statusStr == "" {
		slog.Warn("status envelope has unknown OnlineStatus",
			"device_id", env.DeviceId, "value", env.OnlineStatus)
		_ = msg.Ack()
		return
	}
	if env.DeviceId == "" || env.TimestampUs == 0 {
		slog.Warn("status envelope missing device_id or timestamp",
			"device_id", env.DeviceId, "timestamp_us", env.TimestampUs)
		_ = msg.Ack()
		return
	}

	lastSeen := time.UnixMicro(env.TimestampUs).UTC()
	sessionIDPtr := optionalString(env.SessionId)
	stmState := mapStm32State(env.Stm32State)

	ctx, cancel := context.WithTimeout(context.Background(), dbOpTimeout)
	defer cancel()

	// The (last_seen_at < $2) guard prevents an out-of-order delivery from
	// overwriting a newer heartbeat with an older one.
	tag, err := s.pool.Exec(ctx,
		`UPDATE bbb_devices
		 SET status              = $1,
		     last_seen_at        = $2,
		     current_session_id  = $4::uuid,
		     stm32_state         = $5,
		     updated_at          = now()
		 WHERE id = $3::uuid
		   AND (last_seen_at IS NULL OR last_seen_at < $2)`,
		statusStr, lastSeen, env.DeviceId, sessionIDPtr, stmState,
	)
	if err != nil {
		slog.Error("status persist failed", "error", err, "device_id", env.DeviceId)
		_ = msg.Ack()
		return
	}
	if tag.RowsAffected() == 0 {
		// Either the device id is unknown to us, or this message was older
		// than what we already have. Either way nothing to do.
		slog.Debug("status update no-op",
			"device_id", env.DeviceId, "status", statusStr, "last_seen", lastSeen)
	} else {
		slog.Info("device status updated",
			"device_id", env.DeviceId,
			"status", statusStr,
			"last_seen", lastSeen,
			"session_id", env.SessionId,
			"stm32_state", env.Stm32State.String(),
		)
	}
	_ = msg.Ack()
}

// publishToBus serialises a status or telemetry envelope to JSON and pushes
// it onto the in-memory bus. Any other proto type is a programmer error and
// is dropped with an Error log.
func (s *Subscriber) publishToBus(env proto.Message, eventType string) {
	var sessionID, deviceID string
	switch e := env.(type) {
	case *pb.StatusEnvelope:
		sessionID, deviceID = e.SessionId, e.DeviceId
	case *pb.TelemetryEnvelope:
		sessionID, deviceID = e.SessionId, e.DeviceId
	default:
		slog.Error("publishToBus: unsupported envelope type",
			"type", fmt.Sprintf("%T", env), "event_type", eventType)
		return
	}

	payload, err := protojson.Marshal(env)
	if err != nil {
		slog.Error("bus marshal failed", "error", err, "event_type", eventType)
		return
	}
	s.bus.Publish(bus.Event{
		SessionID: sessionID,
		DeviceID:  deviceID,
		Type:      eventType,
		JSON:      payload,
	})
}

// optionalString returns nil for empty strings so pgx serialises SQL NULL.
// Used for `current_session_id`: when the BBB reports no session, the column
// must be NULL (UUID columns have no sensible sentinel value).
func optionalString(s string) *string {
	if s == "" {
		return nil
	}
	return &s
}

func (s *Subscriber) handleTelemetry(msg *nats.Msg) {
	var env pb.TelemetryEnvelope
	if err := proto.Unmarshal(msg.Data, &env); err != nil {
		slog.Error("telemetry unmarshal failed", "error", err, "subject", msg.Subject)
		_ = msg.Ack()
		return
	}
	eventType := "telemetry_unknown"
	switch p := env.Payload.(type) {
	case *pb.TelemetryEnvelope_Ack:
		eventType = "telemetry_ack"
		slog.Info("telemetry ack",
			"device_id", env.DeviceId, "session_id", env.SessionId,
			"sequence_num", p.Ack.SequenceNum, "cmd_type", p.Ack.CmdType,
			"error_code", p.Ack.ErrorCode, "sensor_id", p.Ack.SensorId)
	case *pb.TelemetryEnvelope_Error:
		eventType = "telemetry_error"
		slog.Warn("telemetry error",
			"device_id", env.DeviceId, "session_id", env.SessionId,
			"sensor_id", p.Error.SensorId, "error_code", p.Error.ErrorCode)
	case *pb.TelemetryEnvelope_StatusReport:
		eventType = "telemetry_status_report"
		slog.Info("telemetry status_report",
			"device_id", env.DeviceId, "uptime_s", p.StatusReport.UptimeS,
			"active_sensors", p.StatusReport.ActiveSensorCount,
			"queue_depth", p.StatusReport.CommandQueueDepth)
	default:
		slog.Warn("telemetry envelope has no payload variant", "device_id", env.DeviceId)
	}
	s.publishToBus(&env, eventType)
	_ = msg.Ack()
}

func mapOnlineStatus(s pb.OnlineStatus) string {
	switch s {
	case pb.OnlineStatus_ONLINE_STATUS_ONLINE:
		return "online"
	case pb.OnlineStatus_ONLINE_STATUS_BUSY:
		return "busy"
	case pb.OnlineStatus_ONLINE_STATUS_OFFLINE:
		return "offline"
	default:
		return ""
	}
}

func mapStm32State(s pb.Stm32State) string {
	switch s {
	case pb.Stm32State_STM32_STATE_SYNCED:
		return "synced"
	case pb.Stm32State_STM32_STATE_UNSYNCED:
		return "unsynced"
	case pb.Stm32State_STM32_STATE_ERROR:
		return "error"
	default:
		// STM32_STATE_UNSPECIFIED or any unknown value lands here.
		return "unknown"
	}
}
