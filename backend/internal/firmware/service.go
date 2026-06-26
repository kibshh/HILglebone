package firmware

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"time"

	"github.com/google/uuid"
	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
	"github.com/minio/minio-go/v7"
)

const (
	presignedURLDuration  = time.Hour
	pgForeignKeyViolation = "23503"
)

var (
	ErrInvalidReference = errors.New("invalid user_id or dut_device_id")
	ErrEmpty            = errors.New("empty file")
)

type Service struct {
	pool   *pgxpool.Pool
	mc     *minio.Client
	bucket string
}

func NewService(pool *pgxpool.Pool, mc *minio.Client, bucket string) *Service {
	return &Service{pool: pool, mc: mc, bucket: bucket}
}

type UploadRequest struct {
	UserID      uuid.UUID
	DUTDeviceID uuid.UUID
	Filename    string
	Data        io.Reader
	Size        int64
}

type UploadResult struct {
	ID         uuid.UUID `json:"id"`
	Filename   string    `json:"filename"`
	SizeBytes  int64     `json:"size_bytes"`
	SHA256     string    `json:"sha256"`
	StorageURL string    `json:"storage_url"` // canonical s3:// reference (DB record)
	SignedURL  string    `json:"signed_url"`  // short-lived presigned download URL
}

// Upload streams the firmware to MinIO while hashing in a single pass, then
// records metadata in the firmware_uploads table. On a DB insert failure the
// just-uploaded MinIO object is removed best-effort so we don't leak orphans.
// The returned UploadResult includes a presigned URL the BBB can use to fetch
// the binary directly from MinIO.
func (s *Service) Upload(ctx context.Context, req UploadRequest) (*UploadResult, error) {
	if req.Size <= 0 {
		return nil, ErrEmpty
	}

	id := uuid.New()
	objectKey := fmt.Sprintf("%s/%s", id, req.Filename)

	// TeeReader: MinIO consumes the body, hasher gets a copy. One stream, no
	// double buffering of a potentially 64 MB blob.
	hasher := sha256.New()
	reader := io.TeeReader(req.Data, hasher)

	if _, err := s.mc.PutObject(ctx, s.bucket, objectKey,
		reader, req.Size,
		minio.PutObjectOptions{ContentType: "application/octet-stream"},
	); err != nil {
		return nil, fmt.Errorf("minio put: %w", err)
	}

	sha := hex.EncodeToString(hasher.Sum(nil))
	storageURL := fmt.Sprintf("s3://%s/%s", s.bucket, objectKey)

	if _, err := s.pool.Exec(ctx,
		`INSERT INTO firmware_uploads (id, user_id, dut_device_id, filename, size_bytes, sha256, storage_url)
		 VALUES ($1, $2, $3, $4, $5, $6, $7)`,
		id, req.UserID, req.DUTDeviceID, req.Filename, req.Size, sha, storageURL,
	); err != nil {
		// Clean up the orphan MinIO object before returning. Best-effort.
		if rmErr := s.mc.RemoveObject(ctx, s.bucket, objectKey, minio.RemoveObjectOptions{}); rmErr != nil {
			slog.Error("firmware orphan cleanup failed",
				"error", rmErr, "bucket", s.bucket, "key", objectKey)
		}
		var pgErr *pgconn.PgError
		if errors.As(err, &pgErr) && pgErr.Code == pgForeignKeyViolation {
			return nil, ErrInvalidReference
		}
		return nil, fmt.Errorf("insert firmware: %w", err)
	}

	presigned, err := s.mc.PresignedGetObject(ctx, s.bucket, objectKey, presignedURLDuration, nil)
	if err != nil {
		return nil, fmt.Errorf("presign: %w", err)
	}

	slog.Info("firmware uploaded",
		"id", id, "user_id", req.UserID, "dut_device_id", req.DUTDeviceID,
		"size_bytes", req.Size, "sha256", sha)

	return &UploadResult{
		ID:         id,
		Filename:   req.Filename,
		SizeBytes:  req.Size,
		SHA256:     sha,
		StorageURL: storageURL,
		SignedURL:  presigned.String(),
	}, nil
}
