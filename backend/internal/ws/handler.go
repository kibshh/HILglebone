// Package ws hosts the WebSocket fan-out endpoint that pushes session-scoped
// events (telemetry, status, lifecycle) to the frontend in real time.
//
// One subscription per WebSocket connection, filtered by session id from the
// URL path. The handler validates the session exists, then loops forwarding
// every matching bus event as a JSON frame. Slow-client policy: the bus
// drops events silently when its 64-deep buffer fills; if the slowness
// persists, conn.Write hits its 10s deadline and the connection is closed
// so the client reconnects and resyncs.
//
// Auth: TODO — currently any caller can subscribe to any session. Wire user
// authentication once the frontend has a login flow.
package ws

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"time"

	"github.com/coder/websocket"
	"github.com/google/uuid"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/kibshh/HILglebone/backend/internal/bus"
)

const (
	// Hard cap on a single client's connection lifetime — forces periodic
	// reconnect-and-resync so a long-lived client can't drift forever on a
	// stale set of bus events.
	maxConnDuration = 30 * time.Minute
	// Per-message send deadline. If the client can't drain in this window,
	// the connection is closed and the bus subscription is freed.
	writeTimeout = 10 * time.Second
)

type Handler struct {
	pool *pgxpool.Pool
	bus  *bus.Bus
}

func NewHandler(pool *pgxpool.Pool, b *bus.Bus) *Handler {
	return &Handler{pool: pool, bus: b}
}

// Telemetry serves GET /ws/sessions/{id}/telemetry.
// Path parameter: session id (UUID).
func (h *Handler) Telemetry(w http.ResponseWriter, r *http.Request) {
	id, err := uuid.Parse(r.PathValue("id"))
	if err != nil {
		http.Error(w, "invalid session id", http.StatusBadRequest)
		return
	}

	// Verify the session exists before upgrading; otherwise a typo silently
	// gives the client a connection that never delivers anything.
	if err := h.checkSessionExists(r.Context(), id); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			http.Error(w, "session not found", http.StatusNotFound)
			return
		}
		slog.Error("ws session lookup failed", "error", err, "session_id", id)
		http.Error(w, "internal", http.StatusInternalServerError)
		return
	}

	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{
		// TODO: lock down OriginPatterns once a frontend origin is known.
		InsecureSkipVerify: true,
	})
	if err != nil {
		slog.Error("ws upgrade failed", "error", err)
		return
	}
	// PolicyViolation = "you misbehaved and we're closing." If we exit
	// normally we override this with NormalClosure below.
	defer conn.Close(websocket.StatusPolicyViolation, "closed")

	sessionID := id.String()
	sub := h.bus.Subscribe(sessionID)
	defer sub.Close()

	slog.Info("ws client connected", "session_id", sessionID)

	ctx, cancel := context.WithTimeout(r.Context(), maxConnDuration)
	defer cancel()

	// Force-close watcher: when sub.Done fires (escalation OR our own
	// deferred Close), slam the underlying connection so any in-flight
	// conn.Write inside pump returns an error immediately instead of
	// waiting for its writeTimeout. coder/websocket's Close is safe to
	// call concurrently and is idempotent, so racing the deferred close
	// at the top of this function is fine.
	go func() {
		<-sub.Done()
		_ = conn.Close(websocket.StatusGoingAway, "subscription closed")
	}()

	h.pump(ctx, conn, sub)

	conn.Close(websocket.StatusNormalClosure, "")
	slog.Info("ws client disconnected",
		"session_id", sessionID, "dropped", sub.Dropped())
}

// Existence check via QueryRow+Scan.
// If a session row exists, Scan succeeds and returns nil.
// If no row matches the id, Scan returns pgx.ErrNoRows.
// The actual value scanned ("true") is irrelevant; success/failure of Scan
// is what tells us whether the session exists.
func (h *Handler) checkSessionExists(ctx context.Context, id uuid.UUID) error {
	var exists bool
	return h.pool.QueryRow(ctx,
		`SELECT true FROM sessions WHERE id = $1`, id,
	).Scan(&exists)
}

// pump forwards bus events to the WS client until the client disconnects, the
// context expires, or a write fails. The read goroutine exists only so we
// learn promptly when the client goes away.
func (h *Handler) pump(ctx context.Context, conn *websocket.Conn, sub *bus.Subscription) {
	clientGone := make(chan struct{})
	go func() {
		defer close(clientGone)
		// We don't expect inbound messages today; reading just blocks until
		// the client closes, which is how we detect disconnect.
		for {
			if _, _, err := conn.Read(ctx); err != nil {
				return
			}
		}
	}()

	for {
		select {
		case <-ctx.Done():
			return
		case <-clientGone:
			return
		case <-sub.Done():
			// Bus signalled escalation (or someone called Close); exit
			// immediately without first draining whatever is left in
			// sub.Events().
			return
		case ev, ok := <-sub.Events():
			if !ok {
				return
			}
			if err := writeEvent(ctx, conn, ev); err != nil {
				slog.Warn("ws write failed", "error", err)
				return
			}
		}
	}
}

// writeEvent wraps the bus event in a frame the client can route on:
//
//	{ "type": "telemetry_ack", "session_id": "...", "device_id": "...",
//	  "data": <the original envelope JSON> }
func writeEvent(ctx context.Context, conn *websocket.Conn, ev bus.Event) error {
	frame := struct {
		Type      string          `json:"type"`
		SessionID string          `json:"session_id"`
		DeviceID  string          `json:"device_id"`
		Data      json.RawMessage `json:"data"`
	}{
		Type:      ev.Type,
		SessionID: ev.SessionID,
		DeviceID:  ev.DeviceID,
		Data:      ev.JSON,
	}
	payload, err := json.Marshal(frame)
	if err != nil {
		return fmt.Errorf("marshal frame: %w", err)
	}
	writeCtx, cancel := context.WithTimeout(ctx, writeTimeout)
	defer cancel()
	return conn.Write(writeCtx, websocket.MessageText, payload)
}
