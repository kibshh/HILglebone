#!/usr/bin/env bash
# One-shot installer for every tool the HILglebone dev workflow needs.
#
#   ./scripts/dev-bootstrap.sh
#
# Idempotent: each tool is skipped if already on PATH at a usable version.
# Targets Debian / Ubuntu (checked at the top). Uses sudo for package installs;
# you'll be prompted once at the start.
#
# What gets installed
# -------------------
#   apt (system):  git curl ca-certificates gnupg jq protobuf-compiler
#                  python3.12 python3.12-venv python3-pip
#   Docker:        docker-ce + compose plugin, via Docker's official script
#   snap:          go (--classic)
#   go install:    protoc-gen-go   (proto-gen for backend)
#                  nats            (JetStream CLI)
#                  migrate         (golang-migrate, with the postgres driver)
#
# Not installed here (out of scope for local backend dev):
#   - Yocto / bitbake toolchain (see yocto-setup-plan.md)
#   - STM32 firmware toolchain (arm-none-eabi-gcc, openocd)

set -euo pipefail

# ── Output helpers ─────────────────────────────────────────────────

info()  { printf '\033[1;34m[bootstrap]\033[0m %s\n' "$*"; }
warn()  { printf '\033[1;33m[bootstrap]\033[0m %s\n' "$*" >&2; }
error() { printf '\033[1;31m[bootstrap]\033[0m %s\n' "$*" >&2; }
ok()    { printf '\033[1;32m[bootstrap]\033[0m %s\n' "$*"; }

# ── Preflight ──────────────────────────────────────────────────────

require_debian() {
    if ! [ -r /etc/os-release ]; then
        error "cannot read /etc/os-release; unsupported platform"
        exit 1
    fi
    # ID or ID_LIKE must contain 'debian' (covers Ubuntu, Debian, PopOS, Mint...).
    if ! grep -qE '^(ID|ID_LIKE)=.*(debian|ubuntu)' /etc/os-release; then
        error "this script targets Debian / Ubuntu"
        error "  detected: $(. /etc/os-release && echo "${PRETTY_NAME:-unknown}")"
        exit 1
    fi
}

require_sudo() {
    if [ "$(id -u)" -eq 0 ]; then
        return
    fi
    if ! command -v sudo >/dev/null 2>&1; then
        error "sudo is required (or run this script as root)"
        exit 1
    fi
    info "requesting sudo — you'll be prompted once"
    sudo -v
    # Refresh the sudo timestamp in the background so long installs don't
    # re-prompt. Killed when this shell exits.
    ( while true; do sudo -n true; sleep 50; kill -0 "$$" || exit; done ) 2>/dev/null &
}

# ── Small utilities ────────────────────────────────────────────────

on_path() { command -v "$1" >/dev/null 2>&1; }

apt_cache_refreshed=0
apt_update_once() {
    if [ "$apt_cache_refreshed" -eq 0 ]; then
        info "apt update"
        sudo apt-get update -y
        apt_cache_refreshed=1
    fi
}

ensure_apt_pkg() {
    local pkg="$1"
    if dpkg -s "$pkg" >/dev/null 2>&1; then
        return
    fi
    apt_update_once
    info "apt install $pkg"
    sudo apt-get install -y "$pkg"
}

# GOPATH/bin must be on PATH before `go install`-based tools work.
gobin() {
    local dir
    dir="$(go env GOBIN)"
    [ -z "$dir" ] && dir="$(go env GOPATH)/bin"
    printf '%s\n' "$dir"
}

ensure_gobin_on_path() {
    local dir; dir="$(gobin)"
    case ":$PATH:" in
        *":$dir:"*) return ;;
    esac
    warn "GOBIN ($dir) is not on your PATH — add it to your shell rc:"
    warn "  echo 'export PATH=\"\$PATH:$dir\"' >> ~/.bashrc"
    # Add for this shell so subsequent 'command -v' checks succeed.
    export PATH="$PATH:$dir"
}

ensure_go_tool() {
    local binary="$1" package="$2"
    shift 2
    if on_path "$binary"; then
        ok "$binary already installed ($(command -v "$binary"))"
        return
    fi
    info "go install $package (as $binary)"
    # shellcheck disable=SC2068
    GOFLAGS="" go install $@ "$package"
    ensure_gobin_on_path
}

# ── Installers ─────────────────────────────────────────────────────

ensure_apt_prereqs() {
    for pkg in git curl ca-certificates gnupg jq; do
        ensure_apt_pkg "$pkg"
    done
}

ensure_docker() {
    if on_path docker && docker compose version >/dev/null 2>&1; then
        ok "Docker already installed ($(docker --version))"
        return
    fi
    info "installing Docker CE + compose plugin via get.docker.com"
    curl -fsSL https://get.docker.com -o /tmp/get-docker.sh
    trap 'rm -f /tmp/get-docker.sh' RETURN
    sudo sh /tmp/get-docker.sh
    # Post-install: allow the current user to run docker without sudo.
    if ! id -nG "$USER" | grep -qw docker; then
        info "adding $USER to the docker group"
        sudo usermod -aG docker "$USER"
        warn "log out and back in (or run 'newgrp docker') for group membership to take effect"
    fi
}

ensure_go() {
    if on_path go; then
        ok "Go already installed ($(go version))"
        return
    fi
    info "installing Go via snap (--classic)"
    if ! on_path snap; then
        error "snap is not installed; install snap first or install Go manually"
        exit 1
    fi
    sudo snap install go --classic
}

ensure_python() {
    if on_path python3.12 && python3.12 -c 'import venv' >/dev/null 2>&1; then
        ok "Python 3.12 already installed ($(python3.12 --version))"
        return
    fi
    info "installing python3.12 + venv"
    ensure_apt_pkg python3.12
    ensure_apt_pkg python3.12-venv
    ensure_apt_pkg python3-pip
}

ensure_protoc() {
    if on_path protoc; then
        ok "protoc already installed ($(protoc --version))"
        return
    fi
    ensure_apt_pkg protobuf-compiler
}

ensure_go_tools() {
    ensure_go_tool protoc-gen-go google.golang.org/protobuf/cmd/protoc-gen-go@latest
    ensure_go_tool nats           github.com/nats-io/natscli/nats@latest
    # migrate CLI's DB drivers are behind build tags — without 'postgres'
    # the binary can't talk to our compose Postgres.
    ensure_go_tool migrate        github.com/golang-migrate/migrate/v4/cmd/migrate@latest \
                                  -tags 'postgres,file'
}

# ── Main ───────────────────────────────────────────────────────────

main() {
    require_debian
    require_sudo
    ensure_apt_prereqs
    ensure_docker
    ensure_go
    ensure_python
    ensure_protoc
    ensure_go_tools

    printf '\n'
    ok "bootstrap complete."
    ok "next steps:"
    ok "  1. cd backend && cp -n .env.example .env && docker compose up --build"
    ok "  2. source ./bbb/scripts/setup-env.sh   # BBB agent venv"
    ok "  3. ./backend/scripts/gen-proto.sh && ./bbb/scripts/gen-proto.sh   # regen protos"
}

main "$@"
