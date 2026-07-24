// Package config centralises all backend configuration. Inputs are
// environment variables; output is a typed Config the rest of the program
// reads. The Mode field distinguishes local development from production,
// and a handful of security-sensitive settings (MinIO TLS, WebSocket
// origins, log level) are gated on it.
//
// Default-safe rule: if APP_ENV is unset, we assume production. A
// forgotten env var should never silently relax security.
package config

import (
	"errors"
	"fmt"
	"log/slog"
	"os"
	"strings"
)

type Mode string

const (
	ModeDevelopment Mode = "development"
	ModeProduction  Mode = "production"
)

type Config struct {
	Mode Mode

	BackendAddr string
	DatabaseURL string
	NATSURL     string

	// Internal endpoint the backend uses for admin ops (bucket create,
	// PutObject, RemoveObject).
	MinIOEndpoint string
	// Public endpoint baked into presigned URLs handed to the BBB. Must
	// be resolvable from wherever the BBB runs — the BBB is always
	// off-network (real hardware on the LAN or a host-side test process),
	// so this is never `minio:9000`. In dev compose it's `localhost:9000`
	// (or the dev host's LAN IP for a real BBB); in prod it's the public
	// hostname the fleet dials.
	//
	// Falls back to MinIOEndpoint when unset only so single-process test
	// setups (backend + MinIO both on localhost, no compose) don't need
	// to specify two identical values. Compose-based configs should set
	// it explicitly.
	MinIOPublicEndpoint string
	MinIOAccessKey      string
	MinIOSecretKey      string
	MinIOBucket         string
	MinIOUseSSL         bool
	// TLS flag for the public endpoint. Falls back to MinIOUseSSL when
	// unset — the same rule as MinIOPublicEndpoint above.
	MinIOPublicUseSSL bool

	// WSAllowedOrigins is the list of Origin patterns the WS upgrader
	// accepts. Empty means "any origin" (only allowed in development).
	WSAllowedOrigins []string

	// LogLevel is the minimum slog level. Dev defaults to Debug, prod to Info.
	LogLevel slog.Level
}

func (c *Config) IsDevelopment() bool { return c.Mode == ModeDevelopment }
func (c *Config) IsProduction() bool  { return c.Mode == ModeProduction }

func Load() (*Config, error) {
	mode, err := loadMode()
	if err != nil {
		return nil, err
	}

	cfg := &Config{
		Mode:                mode,
		BackendAddr:         os.Getenv("BACKEND_ADDR"),
		DatabaseURL:         os.Getenv("DATABASE_URL"),
		NATSURL:             os.Getenv("NATS_URL"),
		MinIOEndpoint:       os.Getenv("MINIO_ENDPOINT"),
		MinIOPublicEndpoint: os.Getenv("MINIO_PUBLIC_ENDPOINT"),
		MinIOAccessKey:      os.Getenv("MINIO_ACCESS_KEY"),
		MinIOSecretKey:      os.Getenv("MINIO_SECRET_KEY"),
		MinIOBucket:         os.Getenv("MINIO_BUCKET"),
	}
	// Public endpoint defaults to the internal one — pure-compose dev
	// works with a single value; on-prem / prod sets both explicitly.
	if cfg.MinIOPublicEndpoint == "" {
		cfg.MinIOPublicEndpoint = cfg.MinIOEndpoint
	}

	if missing := cfg.requiredMissing(); len(missing) > 0 {
		return nil, fmt.Errorf("required env vars missing: %s", strings.Join(missing, ", "))
	}

	if err := cfg.loadMinIOSSL(); err != nil {
		return nil, err
	}
	if err := cfg.loadWSOrigins(); err != nil {
		return nil, err
	}
	cfg.LogLevel = defaultLogLevel(mode)

	return cfg, nil
}

func loadMode() (Mode, error) {
	v := os.Getenv("APP_ENV")
	if v == "" {
		return ModeProduction, nil
	}
	switch Mode(v) {
	case ModeDevelopment, ModeProduction:
		return Mode(v), nil
	default:
		return "", fmt.Errorf("APP_ENV must be %q or %q, got %q",
			ModeDevelopment, ModeProduction, v)
	}
}

func (c *Config) requiredMissing() []string {
	pairs := []struct {
		name, value string
	}{
		{"BACKEND_ADDR", c.BackendAddr},
		{"DATABASE_URL", c.DatabaseURL},
		{"NATS_URL", c.NATSURL},
		{"MINIO_ENDPOINT", c.MinIOEndpoint},
		{"MINIO_BUCKET", c.MinIOBucket},
	}
	// Credentials are required in production only. In development, MinIO
	// may run anonymously or with implicit auth from a local docker setup.
	if c.Mode == ModeProduction {
		pairs = append(pairs,
			struct{ name, value string }{"MINIO_ACCESS_KEY", c.MinIOAccessKey},
			struct{ name, value string }{"MINIO_SECRET_KEY", c.MinIOSecretKey},
		)
	}
	var missing []string
	for _, p := range pairs {
		if p.value == "" {
			missing = append(missing, p.name)
		}
	}
	return missing
}

// loadMinIOSSL enforces TLS in production; allows explicit opt-out in dev.
// Loads MinIOUseSSL (internal endpoint TLS) and MinIOPublicUseSSL (public
// endpoint TLS); the latter defaults to the former.
func (c *Config) loadMinIOSSL() error {
	switch os.Getenv("MINIO_USE_SSL") {
	case "true":
		c.MinIOUseSSL = true
	case "false":
		if c.Mode == ModeProduction {
			return errors.New("MINIO_USE_SSL=false is not allowed in production")
		}
		c.MinIOUseSSL = false
	case "":
		if c.Mode == ModeDevelopment {
			return errors.New("development requires MINIO_USE_SSL to be \"true\" or \"false\"")
		}
		// Production with the var unset: default to TLS.
		c.MinIOUseSSL = true
	default:
		return fmt.Errorf("MINIO_USE_SSL must be \"true\" or \"false\", got %q",
			os.Getenv("MINIO_USE_SSL"))
	}

	// MINIO_PUBLIC_USE_SSL: same accepted values as MINIO_USE_SSL. Unset
	// means "same as internal" — matches how MinIOPublicEndpoint defaults
	// to MinIOEndpoint when the split isn't needed.
	switch os.Getenv("MINIO_PUBLIC_USE_SSL") {
	case "true":
		c.MinIOPublicUseSSL = true
	case "false":
		c.MinIOPublicUseSSL = false
	case "":
		c.MinIOPublicUseSSL = c.MinIOUseSSL
	default:
		return fmt.Errorf("MINIO_PUBLIC_USE_SSL must be \"true\" or \"false\", got %q",
			os.Getenv("MINIO_PUBLIC_USE_SSL"))
	}
	return nil
}

// loadWSOrigins requires an explicit allow-list in production; dev is open.
func (c *Config) loadWSOrigins() error {
	raw := os.Getenv("WS_ALLOWED_ORIGINS")
	if raw != "" {
		for _, o := range strings.Split(raw, ",") {
			if o = strings.TrimSpace(o); o != "" {
				c.WSAllowedOrigins = append(c.WSAllowedOrigins, o)
			}
		}
	}
	if c.Mode == ModeProduction && len(c.WSAllowedOrigins) == 0 {
		return errors.New("WS_ALLOWED_ORIGINS is required in production " +
			"(comma-separated list of Origin patterns)")
	}
	return nil
}

func defaultLogLevel(mode Mode) slog.Level {
	if mode == ModeDevelopment {
		return slog.LevelDebug
	}
	return slog.LevelInfo
}
