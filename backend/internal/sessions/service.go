package sessions

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"time"

	"github.com/google/uuid"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
)

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
	pool *pgxpool.Pool
}

func NewService(pool *pgxpool.Pool) *Service {
	return &Service{pool: pool}
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

// Allocate creates a session in 'allocated' state, binding a user, BBB, DUT,
// and optional firmware. The INSERT ... SELECT FROM dut_devices verifies in a
// single query that the DUT is wired to the requested BBB: if not, zero rows
// match and the insert is a no-op.
func (s *Service) Allocate(ctx context.Context, req AllocateRequest) (*Session, error) {
	row := s.pool.QueryRow(ctx,
		`INSERT INTO sessions (user_id, bbb_device_id, dut_device_id, firmware_upload_id)
		 SELECT $1::uuid, dd.bbb_device_id, dd.id, $4
		 FROM dut_devices dd
		 WHERE dd.id = $3 AND dd.bbb_device_id = $2
		 RETURNING `+sessionFields,
		req.UserID, req.BBBDeviceID, req.DUTDeviceID, req.FirmwareUploadID,
	)
	session, err := scanSession(row)
	if err == nil {
		slog.Info("session allocated", "session_id", session.ID,
			"bbb_device_id", session.BBBDeviceID, "dut_device_id", session.DUTDeviceID)
		return session, nil
	}
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

func (s *Service) Start(ctx context.Context, id uuid.UUID) (*Session, error) {
	row := s.pool.QueryRow(ctx,
		`UPDATE sessions
		 SET status = 'running', started_at = now(), updated_at = now()
		 WHERE id = $1 AND status = 'allocated'
		 RETURNING `+sessionFields,
		id,
	)
	session, err := scanSession(row)
	if err == nil {
		slog.Info("session started", "session_id", id)
		return session, nil
	}
	if !errors.Is(err, pgx.ErrNoRows) {
		slog.Error("session start failed", "error", err, "session_id", id)
		return nil, fmt.Errorf("start session: %w", err)
	}
	return s.classifyTransitionFailure(ctx, id)
}

func (s *Service) Stop(ctx context.Context, id uuid.UUID) (*Session, error) {
	row := s.pool.QueryRow(ctx,
		`UPDATE sessions
		 SET status = 'stopped', stopped_at = now(), updated_at = now()
		 WHERE id = $1 AND status IN ('allocated', 'running')
		 RETURNING `+sessionFields,
		id,
	)
	session, err := scanSession(row)
	if err == nil {
		slog.Info("session stopped", "session_id", id)
		return session, nil
	}
	if !errors.Is(err, pgx.ErrNoRows) {
		slog.Error("session stop failed", "error", err, "session_id", id)
		return nil, fmt.Errorf("stop session: %w", err)
	}
	return s.classifyTransitionFailure(ctx, id)
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
