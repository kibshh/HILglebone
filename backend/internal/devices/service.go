package devices

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"time"

	"github.com/google/uuid"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
)

var ErrUnauthorized = errors.New("unauthorized")

type Service struct {
	pool *pgxpool.Pool
}

func NewService(pool *pgxpool.Pool) *Service {
	return &Service{pool: pool}
}

type RegisterRequest struct {
	Token        string          `json:"token"`
	Capabilities json.RawMessage `json:"capabilities,omitempty"`
}

type RegisterResult struct {
	DeviceID uuid.UUID `json:"device_id"`
}

// Register authenticates a pre-provisioned BBB by its token and marks it online.
// The token is high-entropy random (256 bits) so it serves as both identifier
// and credential — the SHA-256 hash is a UNIQUE column, making the lookup an
// indexed equality test. Returns ErrUnauthorized whenever no row matches, so
// the caller cannot distinguish "token unknown" from any other failure.
func (s *Service) Register(ctx context.Context, req RegisterRequest) (*RegisterResult, error) {
	if req.Token == "" {
		return nil, ErrUnauthorized
	}

	tokenHash := hashToken(req.Token)

	var id uuid.UUID
	err := s.pool.QueryRow(ctx,
		`SELECT id FROM bbb_devices WHERE auth_token_hash = $1`,
		tokenHash,
	).Scan(&id)
	if err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return nil, ErrUnauthorized
		}
		slog.Error("device register failed", "error", err)
		return nil, fmt.Errorf("lookup device: %w", err)
	}

	caps := req.Capabilities
	if len(caps) == 0 {
		caps = json.RawMessage(`{}`)
	}

	now := time.Now().UTC()
	_, err = s.pool.Exec(ctx,
		`UPDATE bbb_devices
		 SET status = 'online', last_seen_at = $1, capabilities = $2, updated_at = $1
		 WHERE id = $3`,
		now, caps, id,
	)
	if err != nil {
		slog.Error("device register failed", "error", err, "device_id", id)
		return nil, fmt.Errorf("update device: %w", err)
	}

	slog.Info("device registered", "device_id", id)
	return &RegisterResult{DeviceID: id}, nil
}
