#!/bin/bash
set -e
PORT=9300
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$BASE_DIR"

fuser -k "${PORT}/tcp" 2>/dev/null || true
sleep 0.2

./bin/caro_server -p "$PORT" > /tmp/caro_srv.log 2>&1 &
SRV_PID=$!
sleep 0.5

cleanup() {
    kill -TERM "$SRV_PID" 2>/dev/null || true
    wait "$SRV_PID" 2>/dev/null || true
}
trap cleanup EXIT

python3 tests/test_challenger_network.py --port "$PORT" --pid "$SRV_PID"
