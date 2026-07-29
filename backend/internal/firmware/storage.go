// Package firmware uploads, hashes, and tracks DUT firmware binaries.
//
// Binary blobs live in MinIO (S3-compatible). The firmware_uploads table is
// the source of truth for metadata; the MinIO object is referenced via its
// canonical s3:// URL and exposed to the BBB via short-lived presigned URLs.
//
// # Two clients, not one
//
// The BBB fetches firmware over the internet using a hostname the *BBB*
// can resolve; the backend PUTs and RemoveObjects over the compose /
// internal network using a hostname it can reach. Those are usually two
// different hostnames — e.g. `minio:9000` (internal, in compose) and
// `firmware.hilglebone.com` (public, what the BBB dials).
//
// Because S3-style signatures cover the request's Host header, you can't
// sign against one hostname and hit MinIO with another — the signature
// won't validate. So we hold two separate *minio.Client instances:
//
//	Admin   — dials the internal endpoint, does bucket ops, PutObject,
//	          RemoveObject. Actually connects.
//	Presign — configured with the public endpoint. Never connects; only
//	          used to compute PresignedGetObject signatures against the
//	          hostname the BBB will use.
//
// When Endpoint == PublicEndpoint (and TLS flags match) the two clients
// are effectively the same, and callers that only need one can use
// Admin. When they differ, PresignedGetObject MUST go through Presign.
package firmware

import (
	"context"
	"errors"
	"fmt"
	"time"

	"github.com/minio/minio-go/v7"
	"github.com/minio/minio-go/v7/pkg/credentials"
)

type StorageConfig struct {
	// Internal endpoint (host:port, no scheme) the backend reaches for
	// admin ops.
	Endpoint string
	// Public endpoint (host:port, no scheme) baked into presigned URLs.
	// The caller is responsible for populating this (see
	// internal/config/config.go, which defaults it to Endpoint when the
	// MINIO_PUBLIC_ENDPOINT env var is unset). Must be non-empty here.
	PublicEndpoint string
	AccessKey      string
	SecretKey      string
	Bucket         string
	UseSSL         bool
	// TLS flag for PublicEndpoint. Independent of UseSSL because prod
	// commonly has plaintext MinIO inside the network and TLS on the
	// public reverse proxy.
	PublicUseSSL bool
}

// Clients bundles the two MinIO clients the service needs.
type Clients struct {
	Admin   *minio.Client
	Presign *minio.Client
}

const bucketCheckTimeout = 5 * time.Second

// OpenStorage dials MinIO on the internal endpoint, verifies / creates the
// bucket, and returns both clients ready for use.
func OpenStorage(ctx context.Context, cfg StorageConfig) (*Clients, error) {
	if cfg.Endpoint == "" {
		return nil, errors.New("minio endpoint is empty")
	}
	if cfg.Bucket == "" {
		return nil, errors.New("minio bucket is empty")
	}

	creds := credentials.NewStaticV4(cfg.AccessKey, cfg.SecretKey, "")

	admin, err := minio.New(cfg.Endpoint, &minio.Options{
		Creds:  creds,
		Secure: cfg.UseSSL,
	})
	if err != nil {
		return nil, fmt.Errorf("minio admin client: %w", err)
	}

	checkCtx, cancel := context.WithTimeout(ctx, bucketCheckTimeout)
	defer cancel()
	exists, err := admin.BucketExists(checkCtx, cfg.Bucket)
	if err != nil {
		return nil, fmt.Errorf("bucket exists check: %w", err)
	}
	if !exists {
		if err := admin.MakeBucket(checkCtx, cfg.Bucket, minio.MakeBucketOptions{}); err != nil {
			return nil, fmt.Errorf("create bucket %q: %w", cfg.Bucket, err)
		}
	}

	// Presign client is a pure signature-computation object; it never
	// dials MinIO, so we don't verify anything against publicEndpoint
	// here. A misconfigured public endpoint surfaces later as the BBB
	// getting DNS-unreachable URLs — that's a diagnostic problem, not a
	// silent one.
	//
	// Shared-bucket assumption
	// ------------------------
	// A minio.Client does NOT hold a bucket — bucket is a per-call
	// argument. Callers (see internal/firmware/service.go) pass the
	// same s.bucket to both admin.PutObject and presign.PresignedGetObject,
	// so both clients target the same *bucket name*.
	//
	// What makes them see the same *bytes* is a deployment-level
	// assumption we can't check from here: cfg.Endpoint and
	// cfg.PublicEndpoint must resolve to the same MinIO instance (or the
	// same MinIO cluster). In compose that's trivially true — there's
	// one `minio:` service. In prod with a reverse proxy, whoever wires
	// nginx / Traefik / Caddy must proxy the public hostname at the same
	// backend the internal network reaches at cfg.Endpoint. If the two
	// endpoints somehow point at *different* MinIO instances, every OTA
	// fetch will 404 (upload landed on server A, BBB is trying server
	// B) — a real failure mode the code can't prevent, only document.
	presign, err := minio.New(cfg.PublicEndpoint, &minio.Options{
		Creds:  creds,
		Secure: cfg.PublicUseSSL,
	})
	if err != nil {
		return nil, fmt.Errorf("minio presign client: %w", err)
	}

	return &Clients{Admin: admin, Presign: presign}, nil
}
