package main

import (
	"context"
	"errors"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/kibshh/HILglebone/backend/internal/bus"
	"github.com/kibshh/HILglebone/backend/internal/config"
	"github.com/kibshh/HILglebone/backend/internal/db"
	"github.com/kibshh/HILglebone/backend/internal/firmware"
	"github.com/kibshh/HILglebone/backend/internal/natspub"
	"github.com/kibshh/HILglebone/backend/internal/natssub"
	"github.com/kibshh/HILglebone/backend/internal/server"
)

const shutdownTimeout = 10 * time.Second

func main() {
	cfg, err := config.Load()
	if err != nil {
		// Use a fallback handler since LogLevel isn't loaded yet.
		slog.SetDefault(slog.New(slog.NewJSONHandler(os.Stdout, nil)))
		slog.Error("config load failed", "error", err)
		os.Exit(1)
	}
	slog.SetDefault(slog.New(slog.NewJSONHandler(os.Stdout, &slog.HandlerOptions{Level: cfg.LogLevel})))
	slog.Info("config loaded", "mode", cfg.Mode)

	ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer cancel()

	pool, err := db.NewPool(ctx, cfg.DatabaseURL)
	if err != nil {
		slog.Error("db connect failed", "error", err)
		os.Exit(1)
	}
	defer pool.Close()

	eventBus := bus.New()

	publisher, err := natspub.Open(cfg.NATSURL)
	if err != nil {
		slog.Error("nats publisher open failed", "error", err)
		os.Exit(1)
	}
	defer publisher.Close()

	subscriber, err := natssub.Open(cfg.NATSURL, pool, eventBus)
	if err != nil {
		slog.Error("nats subscriber open failed", "error", err)
		os.Exit(1)
	}
	defer subscriber.Close()

	mc, err := firmware.OpenStorage(ctx, firmware.StorageConfig{
		Endpoint:  cfg.MinIOEndpoint,
		AccessKey: cfg.MinIOAccessKey,
		SecretKey: cfg.MinIOSecretKey,
		Bucket:    cfg.MinIOBucket,
		UseSSL:    cfg.MinIOUseSSL,
	})
	if err != nil {
		slog.Error("minio open failed", "error", err)
		os.Exit(1)
	}

	srv := server.New(cfg.BackendAddr, pool, publisher, eventBus, mc, cfg.MinIOBucket, cfg.WSAllowedOrigins)

	serverErr := make(chan error, 1)
	go func() {
		slog.Info("server starting", "addr", cfg.BackendAddr)
		if err := srv.ListenAndServe(); !errors.Is(err, http.ErrServerClosed) {
			serverErr <- err
		}
	}()

	select {
	case <-ctx.Done():
		slog.Info("shutdown signal received")
	case err := <-serverErr:
		if err != nil {
			slog.Error("server error", "error", err)
			os.Exit(1)
		}
	}

	shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), shutdownTimeout)
	defer shutdownCancel()

	if err := srv.Shutdown(shutdownCtx); err != nil {
		slog.Error("graceful shutdown failed", "error", err)
		os.Exit(1)
	}
	slog.Info("server stopped")
}
