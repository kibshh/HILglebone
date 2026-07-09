-- Initial HILglebone schema.
-- Requires PostgreSQL 13+ for gen_random_uuid() to be built-in.

-- ── users ─────────────────────────────────────────────────────────────────────
CREATE TABLE users (
    id              UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    email           TEXT        NOT NULL UNIQUE,
    username        TEXT        NOT NULL UNIQUE,
    password_hash   TEXT        NOT NULL,
    role            TEXT        NOT NULL DEFAULT 'user'
                                CHECK (role IN ('user', 'admin')),
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- ── bbb_devices ───────────────────────────────────────────────────────────────
-- Physical BeagleBone Black + STM32 edge nodes registered with the backend.
-- `auth_token_hash` is the hash of a pre-provisioned token the BBB presents at boot.
-- `capabilities` is a free-form JSON blob: supported protocols, peripheral counts,
-- firmware version, etc. — interpreted by the orchestrator, not by SQL.
CREATE TABLE bbb_devices (
    id                  UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    name                TEXT        NOT NULL UNIQUE,
    auth_token_hash     TEXT        NOT NULL UNIQUE,
    status              TEXT        NOT NULL DEFAULT 'offline'
                                    CHECK (status IN ('online', 'busy', 'offline')),
    -- BBB self-reports these via device.{id}.status heartbeats.
    -- `current_session_id` is the session the BBB claims to be in (NULL if idle).
    -- It is intentionally NOT foreign-keyed to sessions(id) so a transient
    -- inconsistency between the BBB's view and the sessions table doesn't
    -- block the heartbeat reconciliation.
    current_session_id  UUID,
    stm32_state         TEXT        NOT NULL DEFAULT 'unknown'
                                    CHECK (stm32_state IN ('unknown', 'synced', 'unsynced', 'error')),
    last_seen_at        TIMESTAMPTZ,
    capabilities        JSONB       NOT NULL DEFAULT '{}'::jsonb,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX bbb_devices_status_idx ON bbb_devices (status);

-- ── dut_devices ───────────────────────────────────────────────────────────────
-- Device Under Test: the user firmware's target board, physically wired to one BBB.
CREATE TABLE dut_devices (
    id              UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    bbb_device_id   UUID        NOT NULL REFERENCES bbb_devices(id) ON DELETE CASCADE,
    name            TEXT        NOT NULL,
    chip            TEXT        NOT NULL,                 -- e.g. 'STM32F401RE', 'nRF52840'
    programmer      TEXT        NOT NULL,                 -- e.g. 'stlink', 'jlink', 'openocd-rpi'
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    UNIQUE (bbb_device_id, name)  -- For future use if more DUTs would be connected to one BBB
);
CREATE INDEX dut_devices_bbb_device_id_idx ON dut_devices (bbb_device_id);

-- ── firmware_uploads ──────────────────────────────────────────────────────────
-- Binary firmware blobs uploaded by users. The blob itself lives in MinIO;
-- `storage_url` is the canonical pointer.
CREATE TABLE firmware_uploads (
    id              UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id         UUID        NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    dut_device_id   UUID        NOT NULL REFERENCES dut_devices(id) ON DELETE CASCADE,
    filename        TEXT        NOT NULL,
    size_bytes      BIGINT      NOT NULL CHECK (size_bytes > 0),
    sha256          TEXT        NOT NULL CHECK (length(sha256) = 64),  -- hex-encoded
    storage_url     TEXT        NOT NULL,                              -- MinIO object URL (e.g. http://minio:9000/firmware/<key>)
    created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX firmware_uploads_user_id_idx       ON firmware_uploads (user_id);
CREATE INDEX firmware_uploads_dut_device_id_idx ON firmware_uploads (dut_device_id);

-- ── sessions ──────────────────────────────────────────────────────────────────
-- A user's active test session: one BBB + one DUT, optionally flashing one
-- firmware upload, optionally driven by a scenario.
CREATE TABLE sessions (
    id                  UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
    user_id             UUID        NOT NULL REFERENCES users(id)            ON DELETE CASCADE,
    bbb_device_id       UUID        NOT NULL REFERENCES bbb_devices(id)      ON DELETE RESTRICT,
    dut_device_id       UUID        NOT NULL REFERENCES dut_devices(id)      ON DELETE RESTRICT,
    firmware_upload_id  UUID        REFERENCES firmware_uploads(id)          ON DELETE SET NULL,
    status              TEXT        NOT NULL DEFAULT 'running'
                                    CHECK (status IN ('running', 'stopped', 'error')),
    started_at          TIMESTAMPTZ,
    stopped_at          TIMESTAMPTZ,
    created_at          TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at          TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX sessions_user_id_idx            ON sessions (user_id);
CREATE INDEX sessions_bbb_device_id_idx      ON sessions (bbb_device_id);
CREATE INDEX sessions_dut_device_id_idx      ON sessions (dut_device_id);
CREATE INDEX sessions_firmware_upload_id_idx ON sessions (firmware_upload_id);
CREATE INDEX sessions_status_idx             ON sessions (status);

-- A BBB can have at most one non-terminal session at a time.
CREATE UNIQUE INDEX sessions_one_active_per_bbb_idx
    ON sessions (bbb_device_id)
    WHERE status = 'running';
