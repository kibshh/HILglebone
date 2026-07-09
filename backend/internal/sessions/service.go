package sessions

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"time"

	"github.com/google/uuid"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"

	pb "github.com/kibshh/HILglebone/backend/gen/hilglebone/v1"
	"github.com/kibshh/HILglebone/backend/internal/bus"
)

// CommandPublisher is the minimal interface the session service needs to
// publish lifecycle events to the BBB. Implemented by natspub.Publisher.
// The publisher derives the routing subject from env.SessionId so the
// caller cannot accidentally route a message to the wrong session.
type CommandPublisher interface {
	PublishCommand(ctx context.Context, env *pb.CommandEnvelope) error
}

var (
	ErrNotFound          = errors.New("session not found")
	ErrInvalidTransition = errors.New("invalid session state for this transition")
	ErrBBBBusy           = errors.New("bbb already has an active session")
	ErrInvalidReference  = errors.New("invalid bbb_device_id, dut_device_id, or firmware_upload_id")
)

// PostgreSQL SQLSTATE codes we map to specific session errors.
// https://www.postgresql.org/docs/current/errcodes-appendix.html
const (
	pgUniqueViolation     = "23505"
	pgForeignKeyViolation = "23503"
)

type Service struct {
	pool      *pgxpool.Pool
	publisher CommandPublisher
	bus       *bus.Bus
}

func NewService(pool *pgxpool.Pool, publisher CommandPublisher, b *bus.Bus) *Service {
	return &Service{pool: pool, publisher: publisher, bus: b}
}

type Session struct {
	ID               uuid.UUID  `json:"id"`
	UserID           uuid.UUID  `json:"user_id"`
	BBBDeviceID      uuid.UUID  `json:"bbb_device_id"`
	DUTDeviceID      uuid.UUID  `json:"dut_device_id"`
	FirmwareUploadID *uuid.UUID `json:"firmware_upload_id"`
	Status           string     `json:"status"`
	StartedAt        *time.Time `json:"started_at"`
	StoppedAt        *time.Time `json:"stopped_at"`
	CreatedAt        time.Time  `json:"created_at"`
	UpdatedAt        time.Time  `json:"updated_at"`
}

type AllocateRequest struct {
	UserID           uuid.UUID  `json:"user_id"`
	BBBDeviceID      uuid.UUID  `json:"bbb_device_id"`
	DUTDeviceID      uuid.UUID  `json:"dut_device_id"`
	FirmwareUploadID *uuid.UUID `json:"firmware_upload_id,omitempty"`
}

type ListFilter struct {
	UserID *uuid.UUID
	Status string
	Limit  *int // nil means no limit
}

const sessionFields = `id, user_id, bbb_device_id, dut_device_id, firmware_upload_id,
	status, started_at, stopped_at, created_at, updated_at`

func scanSession(row pgx.Row) (*Session, error) {
	var s Session
	err := row.Scan(
		&s.ID, &s.UserID, &s.BBBDeviceID, &s.DUTDeviceID, &s.FirmwareUploadID,
		&s.Status, &s.StartedAt, &s.StoppedAt, &s.CreatedAt, &s.UpdatedAt,
	)
	if err != nil {
		return nil, err
	}
	return &s, nil
}

// Allocate creates a session, immediately marks it running, and notifies
// the target BBB. The INSERT ... SELECT FROM dut_devices verifies in a
// single query that the DUT is wired to the requested BBB: if not, zero
// rows match and the insert is a no-op.
//
// A BBB is physically committed to a session the moment it's allocated,
// so there is no meaningful "allocated but not started" state. The row
// goes straight to `running` and SessionStartCommand fires to tell the
// BBB it's now BUSY on this session.
func (s *Service) Allocate(ctx context.Context, req AllocateRequest) (*Session, error) {
	row := s.pool.QueryRow(ctx,
		`INSERT INTO sessions (user_id, bbb_device_id, dut_device_id, firmware_upload_id, status, started_at)
		 SELECT $1::uuid, dd.bbb_device_id, dd.id, $4, 'running', now()
		 FROM dut_devices dd
		 WHERE dd.id = $3 AND dd.bbb_device_id = $2
		 RETURNING `+sessionFields,
		req.UserID, req.BBBDeviceID, req.DUTDeviceID, req.FirmwareUploadID,
	)
	session, err := scanSession(row)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, ErrInvalidReference
		}
		var pgErr *pgconn.PgError
		if errors.As(err, &pgErr) {
			switch pgErr.Code {
			case pgUniqueViolation:
				return nil, ErrBBBBusy
			case pgForeignKeyViolation:
				return nil, ErrInvalidReference
			}
		}
		slog.Error("session allocate failed", "error", err)
		return nil, fmt.Errorf("allocate session: %w", err)
	}
	slog.Info("session allocated", "session_id", session.ID,
		"bbb_device_id", session.BBBDeviceID, "dut_device_id", session.DUTDeviceID)

	if pubErr := s.publishLifecycle(ctx, session, &pb.CommandEnvelope{
		Payload: &pb.CommandEnvelope_SessionStart{SessionStart: &pb.SessionStartCommand{}},
	}); pubErr != nil {
		s.revertAllocate(ctx, session.ID, pubErr)
		return nil, fmt.Errorf("publish session start: %w", pubErr)
	}

	s.publishToBus(session, "session_start")
	return session, nil
}

func (s *Service) Stop(ctx context.Context, id uuid.UUID) (*Session, error) {
	row := s.pool.QueryRow(ctx,
		`UPDATE sessions
		 SET status = 'stopped', stopped_at = now(), updated_at = now()
		 WHERE id = $1 AND status = 'running'
		 RETURNING `+sessionFields,
		id,
	)
	session, err := scanSession(row)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return s.classifyTransitionFailure(ctx, id)
		}
		slog.Error("session stop failed", "error", err, "session_id", id)
		return nil, fmt.Errorf("stop session: %w", err)
	}

	if pubErr := s.publishLifecycle(ctx, session, &pb.CommandEnvelope{
		Payload: &pb.CommandEnvelope_SessionStop{SessionStop: &pb.SessionStopCommand{}},
	}); pubErr != nil {
		s.revertStop(ctx, id, pubErr)
		return nil, fmt.Errorf("publish session stop: %w", pubErr)
	}

	slog.Info("session stopped", "session_id", id)
	s.publishToBus(session, "session_stop")
	return session, nil
}


// publishToBus pushes a lifecycle event onto the in-memory bus so WebSocket
// clients listening on this session can react. Called only after the NATS
// publish succeeds, so the bus event reflects the same state the BBB sees.
func (s *Service) publishToBus(session *Session, eventType string) {
	payload, err := json.Marshal(session)
	if err != nil {
		slog.Error("bus marshal failed", "error", err, "event_type", eventType, "session_id", session.ID)
		return
	}
	s.bus.Publish(bus.Event{
		SessionID: session.ID.String(),
		DeviceID:  session.BBBDeviceID.String(),
		Type:      eventType,
		JSON:      payload,
	})
}

// publishLifecycle fills in the common envelope fields and publishes it to
// the session's subject. Returns an error on publish failure so the caller
// can revert the DB transition (strict consistency).
func (s *Service) publishLifecycle(ctx context.Context, session *Session, env *pb.CommandEnvelope) error {
	env.MessageId = uuid.NewString()
	env.SessionId = session.ID.String()
	env.DeviceId = session.BBBDeviceID.String()
	env.TimestampUs = time.Now().UTC().UnixMicro()
	return s.publisher.PublishCommand(ctx, env)
}

// revertAllocate undoes an Allocate transition when the BBB notification
// publish failed. Deletes the row we just inserted so the frontend never
// sees a session the BBB doesn't know about. Rollback failure is logged
// loudly — the row would then be orphaned in 'allocated' state and would
// need a human / reconciler.
func (s *Service) revertAllocate(ctx context.Context, id uuid.UUID, pubErr error) {
	_, err := s.pool.Exec(ctx,
		`DELETE FROM sessions WHERE id = $1 AND status = 'allocated'`,
		id,
	)
	if err != nil {
		slog.Error("session allocate rollback failed",
			"error", err, "publish_error", pubErr, "session_id", id)
	}
}

// revertStop undoes a Stop transition when the publish failed. With the
// two-state model (running / stopped) the previous status is always
// 'running', so no CTE-captured prev is needed.
func (s *Service) revertStop(ctx context.Context, id uuid.UUID, pubErr error) {
	_, err := s.pool.Exec(ctx,
		`UPDATE sessions
		 SET status = 'running', stopped_at = NULL, updated_at = now()
		 WHERE id = $1 AND status = 'stopped'`,
		id,
	)
	if err != nil {
		slog.Error("session stop rollback failed",
			"error", err, "publish_error", pubErr, "session_id", id)
	}
}

// classifyTransitionFailure disambiguates a no-op UPDATE: was the session
// missing, or just in the wrong state? Get returns ErrNotFound for the former;
// the latter becomes ErrInvalidTransition.
func (s *Service) classifyTransitionFailure(ctx context.Context, id uuid.UUID) (*Session, error) {
	if _, err := s.Get(ctx, id); err != nil {
		return nil, err
	}
	return nil, ErrInvalidTransition
}

func (s *Service) Get(ctx context.Context, id uuid.UUID) (*Session, error) {
	row := s.pool.QueryRow(ctx,
		`SELECT `+sessionFields+` FROM sessions WHERE id = $1`,
		id,
	)
	session, err := scanSession(row)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, ErrNotFound
		}
		return nil, fmt.Errorf("get session: %w", err)
	}
	return session, nil
}

func (s *Service) List(ctx context.Context, filter ListFilter) ([]*Session, error) {
	// LIMIT NULL in PostgreSQL means "no limit", so passing a nil *int through
	// pgx (sent as SQL NULL) returns all matching rows. A non-nil *int caps.
	rows, err := s.pool.Query(ctx,
		`SELECT `+sessionFields+` FROM sessions
		 WHERE ($1::uuid IS NULL OR user_id = $1::uuid)
		   AND ($2::text = '' OR status = $2::text)
		 ORDER BY created_at DESC
		 LIMIT $3`,
		filter.UserID, filter.Status, filter.Limit,
	)
	if err != nil {
		return nil, fmt.Errorf("list sessions: %w", err)
	}
	defer rows.Close()

	var result []*Session
	for rows.Next() {
		session, err := scanSession(rows)
		if err != nil {
			return nil, fmt.Errorf("scan session: %w", err)
		}
		result = append(result, session)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate sessions: %w", err)
	}
	return result, nil
}
