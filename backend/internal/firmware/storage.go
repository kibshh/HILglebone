// Package firmware uploads, hashes, and tracks DUT firmware binaries.
//
// Binary blobs live in MinIO (S3-compatible). The firmware_uploads table is
// the source of truth for metadata; the MinIO object is referenced via its
// canonical s3:// URL and exposed to the BBB via short-lived presigned URLs.
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
	Endpoint  string // host:port, no scheme
	AccessKey string
	SecretKey string
	Bucket    string
	UseSSL    bool
}

const bucketCheckTimeout = 5 * time.Second

// OpenStorage dials MinIO, verifies/creates the bucket, and returns a client
// ready for PutObject / PresignedGetObject calls.
func OpenStorage(ctx context.Context, cfg StorageConfig) (*minio.Client, error) {
	if cfg.Endpoint == "" {
		return nil, errors.New("minio endpoint is empty")
	}
	if cfg.Bucket == "" {
		return nil, errors.New("minio bucket is empty")
	}

	mc, err := minio.New(cfg.Endpoint, &minio.Options{
		Creds:  credentials.NewStaticV4(cfg.AccessKey, cfg.SecretKey, ""),
		Secure: cfg.UseSSL,
	})
	if err != nil {
		return nil, fmt.Errorf("minio client: %w", err)
	}

	checkCtx, cancel := context.WithTimeout(ctx, bucketCheckTimeout)
	defer cancel()
	exists, err := mc.BucketExists(checkCtx, cfg.Bucket)
	if err != nil {
		return nil, fmt.Errorf("bucket exists check: %w", err)
	}
	if !exists {
		if err := mc.MakeBucket(checkCtx, cfg.Bucket, minio.MakeBucketOptions{}); err != nil {
			return nil, fmt.Errorf("create bucket %q: %w", cfg.Bucket, err)
		}
	}
	return mc, nil
}
