#!/bin/bash
# ==============================================================================
# tests/test_g3.sh - Automated Master Runner for Milestone G3
# Caro Online C11/Linux - 4-Tier Automated E2E Test Suite
# ==============================================================================

set -eo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PORT=9350
DB_FILE="$BASE_DIR/tests/test_g3.db"
LOG_FILE="$BASE_DIR/tests/test_g3_results.log"
SERVER_LOG="${CARO_SERVER_LOG:-$BASE_DIR/tests/server_test.log}"
SERVER_PID=""

cd "$BASE_DIR"

cleanup() {
    # Disable trap to avoid recursive calls
    trap - EXIT
    echo "-----------------------------------------------------------------"
    echo "[*] Cleaning up background caro_server (PID: ${SERVER_PID:-none})..."
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "[*] Sending SIGTERM to caro_server (PID: $SERVER_PID)..."
        kill -TERM "$SERVER_PID" 2>/dev/null || true
        WAIT_COUNT=0
        MAX_WAIT=30
        while kill -0 "$SERVER_PID" 2>/dev/null && [ $WAIT_COUNT -lt $MAX_WAIT ]; do
            sleep 0.1
            WAIT_COUNT=$((WAIT_COUNT + 1))
        done
        if kill -0 "$SERVER_PID" 2>/dev/null; then
            echo "[!] Warning: caro_server (PID: $SERVER_PID) did not stop within 3s; sending SIGKILL..."
            kill -KILL "$SERVER_PID" 2>/dev/null || true
            sleep 0.1
        fi
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -f "$DB_FILE" "$DB_FILE-wal" "$DB_FILE-shm"
    echo "[+] Server cleanup and database purge completed."
}
trap cleanup EXIT

echo "================================================================="
echo "   CARO ONLINE C11/LINUX - MILESTONE G3 AUTOMATED TEST SUITE    "
echo "================================================================="
echo "Working directory : $BASE_DIR"
echo "Server target     : 127.0.0.1:$PORT"
echo "Database file     : $DB_FILE"
echo "Output log        : $LOG_FILE"
echo "Server log        : $SERVER_LOG"
echo "Timestamp         : $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
echo "================================================================="

# Check if --invite-timeout was passed in arguments to export for server
for ((i=1; i<=$#; i++)); do
    if [ "${!i}" = "--invite-timeout" ]; then
        next_i=$((i+1))
        export CARO_INVITE_TIMEOUT="${!next_i}"
    fi
done

# 1. Clean Stale Artifacts & Build Project Binaries
echo "[*] Step 1: Cleaning stale database and logs, building binaries..."
rm -f "$DB_FILE" "$DB_FILE-wal" "$DB_FILE-shm" "$SERVER_LOG"
mkdir -p "$BASE_DIR/logs"

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

# 2. Port Conflict Check & Background Server Launch
echo "[*] Step 2: Clearing port $PORT and launching background caro_server..."
fuser -k "${PORT}/tcp" 2>/dev/null || true
sleep 0.2

export CARO_DB_PATH="$DB_FILE"
"$SERVER_BIN" -p "$PORT" -d "$DB_FILE" > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
sleep 0.5

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "[-] Error: caro_server failed to start or died immediately."
    if [ -s "$SERVER_LOG" ]; then
        echo "[-] Server log output ($SERVER_LOG):"
        cat "$SERVER_LOG"
    fi
    exit 1
fi
echo "[+] caro_server running with PID: $SERVER_PID (logging to $SERVER_LOG)"

# 3. Execute Comprehensive 4-Tier Python E2E Test Suite
echo "-----------------------------------------------------------------"
echo "[*] Step 3: Executing 4-Tier E2E test suite (test_g3_suite.py)..."
TEST_STATUS=0
python3 tests/test_g3_suite.py --host 127.0.0.1 --port "$PORT" --pid "$SERVER_PID" --db "$DB_FILE" --log "$LOG_FILE" "$@" || TEST_STATUS=$?

echo "-----------------------------------------------------------------"
if [ "$TEST_STATUS" -eq 0 ]; then
    echo "[+] SUCCESS: All Milestone G3 test tiers passed cleanly (Exit Code: 0)!"
else
    echo "[-] FAILURE: Some test cases failed (Exit Code: $TEST_STATUS)."
    echo "[-] Test execution log : $LOG_FILE"
    echo "[-] Server runtime log  : $SERVER_LOG"
    if [ -s "$SERVER_LOG" ]; then
        echo "[-] Last 20 lines of server log ($SERVER_LOG):"
        tail -n 20 "$SERVER_LOG" 2>/dev/null || true
    fi
fi
echo "================================================================="

exit "$TEST_STATUS"
