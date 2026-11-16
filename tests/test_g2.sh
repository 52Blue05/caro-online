#!/bin/bash
# ==============================================================================
# tests/test_g2.sh - Automated Master Runner for Milestone G2
# Caro Online C11/Linux - Stable Network, Framing, Concurrency & Rules
# ==============================================================================

set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PORT=9200
LOG_FILE="$SCRIPT_DIR/test_g2_results.log"

cd "$BASE_DIR"

echo "================================================================="
echo "   CARO ONLINE C11/LINUX - MILESTONE G2 AUTOMATED TEST SUITE    "
echo "================================================================="
echo "Working directory : $BASE_DIR"
echo "Server target     : 127.0.0.1:$PORT"
echo "Output log        : $LOG_FILE"
echo "Timestamp         : $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
echo "================================================================="

# 1. Clean Build & Unit Tests Execution
echo "[*] Step 1: Building project binaries and running unit tests..."
make clean
make dirs
make server
make client
make test

SERVER_BIN="./bin/caro_server"
CLIENT_BIN="./bin/caro_client"

if [ ! -x "$SERVER_BIN" ] || [ ! -x "$CLIENT_BIN" ]; then
    echo "[-] Error: Executables $SERVER_BIN or $CLIENT_BIN missing or not executable"
    exit 1
fi
echo "[+] Build completed successfully: server and client binaries verified."

# Kill any existing process on target test port
fuser -k "${PORT}/tcp" 2>/dev/null || true
sleep 0.2

# 2. Start server in background
echo "[*] Step 2: Starting background caro_server on port $PORT..."
"$SERVER_BIN" -p "$PORT" > /dev/null 2>&1 &
SERVER_PID=$!
sleep 0.5

cleanup() {
    echo "-----------------------------------------------------------------"
    echo "[*] Cleaning up background caro_server (PID: $SERVER_PID)..."
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    echo "[+] Server cleanup finished."
}
trap cleanup EXIT

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "[-] Error: caro_server failed to start or died immediately."
    exit 1
fi
echo "[+] caro_server running with PID: $SERVER_PID"

# 3. Run Comprehensive Python E2E & Bot Test Suite
echo "-----------------------------------------------------------------"
echo "[*] Step 3: Executing comprehensive E2E test suite (test_g2_suite.py)..."
python3 tests/test_g2_suite.py --host 127.0.0.1 --port "$PORT" --pid "$SERVER_PID" --log "$LOG_FILE"
TEST_STATUS=$?

echo "-----------------------------------------------------------------"
if [ $TEST_STATUS -eq 0 ]; then
    echo "[+] SUCCESS: All Milestone G2 test suites passed cleanly (Exit Code: 0)!"
else
    echo "[-] FAILURE: Some test suites failed (Exit Code: $TEST_STATUS)."
fi
echo "================================================================="

exit $TEST_STATUS
