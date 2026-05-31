#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IOTA_PROXY_DIR="$ROOT_DIR/iota_proxy"
BUSINESS_DIR="$ROOT_DIR/business_service"
LOG_DIR="$ROOT_DIR/logs"

IOTA_PROXY_BIN="$IOTA_PROXY_DIR/build/iota_proxy"
BUSINESS_BIN="$BUSINESS_DIR/demo"
IOTA_PROXY_CONFIG="$IOTA_PROXY_DIR/config/edge_pipeline.json"
BUSINESS_CONFIG="$BUSINESS_DIR/config/config.json"

mkdir -p "$LOG_DIR"

if [ ! -x "$IOTA_PROXY_BIN" ] || [ ! -x "$BUSINESS_BIN" ]; then
  echo "missing binary, run ./scripts/build_all.sh first" >&2
  exit 1
fi

proxy_pid=""
business_pid=""

cleanup() {
  set +e
  if [ -n "$business_pid" ] && kill -0 "$business_pid" 2>/dev/null; then
    kill "$business_pid" 2>/dev/null
  fi
  if [ -n "$proxy_pid" ] && kill -0 "$proxy_pid" 2>/dev/null; then
    kill "$proxy_pid" 2>/dev/null
  fi
  wait "$business_pid" 2>/dev/null
  wait "$proxy_pid" 2>/dev/null
}

trap cleanup EXIT INT TERM

echo "[start] iota_proxy"
(
  cd "$IOTA_PROXY_DIR"
  exec "$IOTA_PROXY_BIN" --disable-tcp-ingress --edge-config "$IOTA_PROXY_CONFIG"
) >"$LOG_DIR/iota_proxy.log" 2>&1 &
proxy_pid=$!

sleep 1
if ! kill -0 "$proxy_pid" 2>/dev/null; then
  echo "iota_proxy exited early, see $LOG_DIR/iota_proxy.log" >&2
  exit 1
fi

echo "[start] business_service"
(
  cd "$BUSINESS_DIR"
  export IOTA_BUSINESS_CONFIG="$BUSINESS_CONFIG"
  exec "$BUSINESS_BIN"
) >"$LOG_DIR/business_service.log" 2>&1 &
business_pid=$!

sleep 1
if ! kill -0 "$business_pid" 2>/dev/null; then
  echo "business_service exited early, see $LOG_DIR/business_service.log" >&2
  exit 1
fi

echo "[running]"
echo "  iota_proxy pid: $proxy_pid, log: $LOG_DIR/iota_proxy.log"
echo "  business_service pid: $business_pid, log: $LOG_DIR/business_service.log"
echo "press Ctrl+C to stop"

wait -n "$proxy_pid" "$business_pid"
status=$?
echo "one service exited, stopping the rest"
exit "$status"
