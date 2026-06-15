package server

import (
	"context"
	"log/slog"
	"net/http"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"

	"github.com/kibshh/HILglebone/backend/internal/devices"
	"github.com/kibshh/HILglebone/backend/internal/httpx"
	"github.com/kibshh/HILglebone/backend/internal/sessions"
)

const readyPingTimeout = 2 * time.Second

func New(addr string, pool *pgxpool.Pool) *http.Server {
	deviceSvc := devices.NewService(pool)
	deviceHandler := devices.NewHandler(deviceSvc)

	sessionSvc := sessions.NewService(pool)
	sessionHandler := sessions.NewHandler(sessionSvc)

	mux := http.NewServeMux()
	mux.HandleFunc("GET /healthz", healthz)
	mux.HandleFunc("GET /readyz", readyz(pool))
	mux.HandleFunc("POST /api/v1/devices/register", deviceHandler.Register)
	mux.HandleFunc("POST /api/v1/sessions", sessionHandler.Allocate)
	mux.HandleFunc("GET /api/v1/sessions", sessionHandler.List)
	mux.HandleFunc("GET /api/v1/sessions/{id}", sessionHandler.Get)
	mux.HandleFunc("POST /api/v1/sessions/{id}/start", sessionHandler.Start)
	mux.HandleFunc("POST /api/v1/sessions/{id}/stop", sessionHandler.Stop)

	return &http.Server{
		Addr:              addr,
		Handler:           withLogging(mux),
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       15 * time.Second,
		WriteTimeout:      15 * time.Second,
		IdleTimeout:       60 * time.Second,
	}
}

func healthz(w http.ResponseWriter, _ *http.Request) {
	httpx.WriteJSON(w, http.StatusOK, map[string]string{"status": "ok"})
}

func readyz(pool *pgxpool.Pool) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		ctx, cancel := context.WithTimeout(r.Context(), readyPingTimeout)
		defer cancel()
		if err := pool.Ping(ctx); err != nil {
			httpx.WriteJSON(w, http.StatusServiceUnavailable, map[string]string{"status": "unready", "reason": "db"})
			return
		}
		httpx.WriteJSON(w, http.StatusOK, map[string]string{"status": "ready"})
	}
}

func withLogging(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		sw := &statusWriter{ResponseWriter: w, status: http.StatusOK}
		next.ServeHTTP(sw, r)
		slog.Info("request",
			"method", r.Method,
			"path", r.URL.Path,
			"status", sw.status,
			"duration_ms", time.Since(start).Milliseconds(),
			"remote", r.RemoteAddr,
		)
	})
}

type statusWriter struct {
	http.ResponseWriter
	status int
}

func (s *statusWriter) WriteHeader(code int) {
	s.status = code
	s.ResponseWriter.WriteHeader(code)
}
