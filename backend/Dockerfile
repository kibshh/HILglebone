# syntax=docker/dockerfile:1.7
#
# Multi-stage build for the HILglebone backend.
#
# Stage 1 (build) — compile a static binary with CGO off so it runs on any
#   Linux, then strip debug info to keep the shipped artefact small.
# Stage 2 (runtime) — distroless static: no shell, no package manager,
#   nonroot user. The image only contains the compiled server plus the
#   CA bundle needed for outbound TLS (MinIO / NATS with TLS).

FROM golang:1.25-alpine AS build

WORKDIR /src

# Cache module downloads across builds: copying go.mod/go.sum first means
# `go mod download` is only re-run when dependencies change, not on every
# source edit.
COPY go.mod go.sum ./
RUN go mod download

COPY . .

# CGO_ENABLED=0 → pure-Go build, no libc dependency.
# -trimpath           → reproducible paths in stack traces (no /src/... noise).
# -ldflags "-s -w"    → strip symbol + DWARF tables; a few MB smaller.
RUN CGO_ENABLED=0 GOOS=linux go build \
    -trimpath \
    -ldflags "-s -w" \
    -o /out/server \
    ./cmd/server

# --- runtime ---
FROM gcr.io/distroless/static-debian12:nonroot

COPY --from=build /out/server /server

EXPOSE 8080
USER nonroot:nonroot
ENTRYPOINT ["/server"]
