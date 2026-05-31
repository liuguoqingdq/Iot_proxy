#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STORAGE_DIR="$ROOT_DIR/storage_service"
STORAGE_BIN="$STORAGE_DIR/storage_service"
STORAGE_CONFIG="$STORAGE_DIR/config/config.json"

if [ ! -x "$STORAGE_BIN" ]; then
  echo "missing storage_service binary, run ./scripts/build_all.sh first" >&2
  exit 1
fi

cd "$STORAGE_DIR"
export IOTA_STORAGE_CONFIG="$STORAGE_CONFIG"
exec "$STORAGE_BIN"
