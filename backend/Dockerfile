# syntax=docker/dockerfile:1.7
#
# Two named build targets sharing one compile stage:
#
#   docker build --target dev  -t hilglebone-backend:dev  .
#   docker build --target prod -t hilglebone-backend:prod .
#
# docker-compose.yml       → target: dev   (local development)
# docker-compose.prod.yml  → target: prod  (single-node prod / image for k8s)

# ── Shared compile stage ───────────────────────────────────────────
FROM golang:1.25-alpine AS build

WORKDIR /src

# Cache module downloads across builds: copying go.mod/go.sum first means
# `go mod download` is only re-run when dependencies change, not on every
# source edit.
COPY go.mod go.sum ./
RUN go mod download

# Explicit COPY of only the directories the Go build actually reads. This
# stops edits to migrations/, infra/, scripts/, docker-compose.yml, etc.
# from invalidating the source layer and forcing a needless `go build`.
COPY cmd/      ./cmd/
COPY internal/ ./internal/
COPY gen/      ./gen/

# CGO_ENABLED=0  → pure-Go build, no libc dependency (runs on both alpine
#                  and distroless static without extra shims).
# -trimpath      → reproducible paths in stack traces (no /src/... noise).
# -ldflags -s -w → strip symbol + DWARF tables; a few MB smaller.
RUN CGO_ENABLED=0 GOOS=linux go build \
    -trimpath \
    -ldflags "-s -w" \
    -o /out/server \
    ./cmd/server

# ── Dev runtime ────────────────────────────────────────────────────
# Full alpine base with a shell + ca-certificates so you can
# `docker compose exec backend sh` and poke around when something's wrong.
# Bigger and less locked-down than prod on purpose.
FROM alpine:3.20 AS dev

RUN apk add --no-cache ca-certificates

COPY --from=build /out/server /server

EXPOSE 8080
ENTRYPOINT ["/server"]

# ── Prod runtime ───────────────────────────────────────────────────
# Distroless: no shell, no package manager, nonroot user. The image only
# contains the compiled server + the CA bundle needed for outbound TLS
# (MinIO with TLS, NATS with TLS, etc.).
FROM gcr.io/distroless/static-debian12:nonroot AS prod

COPY --from=build /out/server /server

EXPOSE 8080
USER nonroot:nonroot
ENTRYPOINT ["/server"]
