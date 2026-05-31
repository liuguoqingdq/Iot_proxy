#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IOTA_PROXY_DIR="$ROOT_DIR/iota_proxy"
BUSINESS_DIR="$ROOT_DIR/business_service"
STORAGE_DIR="$ROOT_DIR/storage_service"

if ! command -v go >/dev/null 2>&1 && [ -x "$HOME/.local/sdk/go/bin/go" ]; then
  export GOROOT="$HOME/.local/sdk/go"
  export GOPATH="${GOPATH:-$HOME/go}"
  export PATH="$GOROOT/bin:$GOPATH/bin:$PATH"
fi

echo "[build] iota_proxy"
cmake -S "$IOTA_PROXY_DIR" -B "$IOTA_PROXY_DIR/build"
cmake --build "$IOTA_PROXY_DIR/build"

echo "[build] business_service"
cd "$BUSINESS_DIR"
go mod tidy
go build -buildvcs=false -o demo

echo "[build] storage_service"
cd "$STORAGE_DIR"
go mod tidy
go build -buildvcs=false -o storage_service ./cmd/storage_service

echo "[build] done"
echo "  $IOTA_PROXY_DIR/build/iota_proxy"
echo "  $BUSINESS_DIR/demo"
echo "  $STORAGE_DIR/storage_service"
