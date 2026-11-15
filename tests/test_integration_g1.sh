#!/bin/bash
# ==============================================================================
# test_integration_g1.sh - Integration Test Suite for Milestone G1
# Agent Hung (20235737) - Verification of Two Concurrent Clients,
# Disconnect Handling, and Framing PING/PONG.
# ==============================================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PORT=9124
LOG_FILE="$SCRIPT_DIR/test_g1_results.log"

echo "================================================================="
echo "   CARO ONLINE C11/LINUX - INTEGRATION TEST SUITE G1 (SHELL)    "
echo "================================================================="
echo "Working directory: $BASE_DIR"
echo "Server target: 127.0.0.1:$PORT"
echo "Log file: $LOG_FILE"

cd "$BASE_DIR"

# 1. Clean build
echo "[*] Building project binaries..."
make clean
make all

SERVER_BIN="./bin/caro_server"
CLIENT_BIN="./bin/caro_client"

if [ ! -x "$SERVER_BIN" ] || [ ! -x "$CLIENT_BIN" ]; then
    echo "[-] Error: Executables $SERVER_BIN or $CLIENT_BIN not found or not executable"
    exit 1
fi

# 2. Start server in background
echo "[*] Starting caro_server in background on port $PORT..."
$SERVER_BIN -p $PORT > /dev/null 2>&1 &
SERVER_PID=$!
sleep 0.5

cleanup() {
    echo "[*] Cleaning up server PID $SERVER_PID..."
    kill -TERM "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
}
trap cleanup EXIT

# 3. Test 1: Two concurrent clients (--ping batch mode)
echo "-----------------------------------------------------------------"
echo "TEST 1: Two Concurrent Clients (--ping batch mode)"
$CLIENT_BIN -p $PORT --ping "client1_ping_alice" &
PID1=$!
$CLIENT_BIN -p $PORT --ping "client2_ping_bob" &
PID2=$!

wait $PID1
STATUS1=$?
wait $PID2
STATUS2=$?

if [ $STATUS1 -eq 0 ] && [ $STATUS2 -eq 0 ]; then
    echo "[+] PASS: Both Client 1 and Client 2 completed PING/PONG successfully!"
else
    echo "[-] FAIL: Concurrent clients test failed (Status: Client1=$STATUS1, Client2=$STATUS2)"
    exit 1
fi

# 4. Test 2: Interactive CLI through pipe with delay
echo "-----------------------------------------------------------------"
echo "TEST 2: Interactive Client CLI with Piped Commands"
(
    printf "help\n"
    sleep 0.3
    printf "ping hello_from_script\n"
    sleep 0.3
    printf "quit\n"
) | $CLIENT_BIN -p $PORT

echo "[+] PASS: Interactive CLI test completed cleanly!"

# 5. Run comprehensive Python integration suite
echo "-----------------------------------------------------------------"
echo "TEST 3: Running Comprehensive Python Integration Suite..."
python3 tests/test_integration_g1.py

echo "================================================================="
echo "[+] ALL INTEGRATION TESTS (G1) COMPLETED AND VERIFIED!"
echo "================================================================="
