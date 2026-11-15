#!/usr/bin/env python3
"""
Integration Test Suite for Milestone G1 - Online Caro Game in C
Agent Hung (20235737) - Verification of Two Concurrent Clients,
Framing, Non-blocking Poll Event Loop, Disconnect Resilience, and Recovery.
"""

import sys
import os
import time
import socket
import struct
import subprocess
import signal

SERVER_PORT = 9123
SERVER_HOST = "127.0.0.1"
LOG_FILE_PATH = os.path.join(os.path.dirname(__file__), "test_g1_results.log")

# ANSI color helpers
GREEN = "\033[92m"
RED = "\033[91m"
YELLOW = "\033[93m"
BLUE = "\033[94m"
RESET = "\033[0m"

log_file = None

def log_print(msg, color=""):
    timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
    formatted = f"[{timestamp}] {msg}"
    if color:
        print(f"{color}{formatted}{RESET}")
    else:
        print(formatted)
    if log_file:
        log_file.write(formatted + "\n")
        log_file.flush()

def pack_frame(payload_str):
    if not payload_str.endswith("\n"):
        payload_str += "\n"
    payload_bytes = payload_str.encode("utf-8")
    header = struct.pack("!I", len(payload_bytes))
    return header + payload_bytes

def unpack_frame(sock, timeout=3.0):
    sock.settimeout(timeout)
    # Read 4-byte header
    header = b""
    while len(header) < 4:
        chunk = sock.recv(4 - len(header))
        if not chunk:
            return None
        header += chunk
    payload_len = struct.unpack("!I", header)[0]

    # Read payload
    payload = b""
    while len(payload) < payload_len:
        chunk = sock.recv(payload_len - len(payload))
        if not chunk:
            return None
        payload += chunk
    return payload.decode("utf-8", errors="replace")

def run_tests():
    global log_file
    os.makedirs(os.path.dirname(LOG_FILE_PATH), exist_ok=True)
    log_file = open(LOG_FILE_PATH, "w", encoding="utf-8")

    log_print("=================================================================", BLUE)
    log_print("   CARO ONLINE C11/LINUX - INTEGRATION TEST SUITE G1 (T1H4)     ", BLUE)
    log_print("=================================================================", BLUE)
    log_print(f"Server Target: {SERVER_HOST}:{SERVER_PORT}")
    log_print(f"Test Log File: {os.path.abspath(LOG_FILE_PATH)}")

    # Detect binary paths
    base_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    server_bin = os.path.join(base_dir, "bin", "caro_server")
    client_bin = os.path.join(base_dir, "bin", "caro_client")
    if sys.platform == "win32" and not os.path.exists(server_bin):
        server_bin += ".exe"
        client_bin += ".exe"

    if not os.path.exists(server_bin) or not os.path.exists(client_bin):
        log_print(f"[-] Binaries not found: server={server_bin}, client={client_bin}", RED)
        return False

    log_print(f"[+] Found server binary: {server_bin}")
    log_print(f"[+] Found client binary: {client_bin}")

    passed_count = 0
    total_tests = 7

    # Start Server in Background
    log_print("[*] Starting caro_server in background on port %d..." % SERVER_PORT)
    server_proc = subprocess.Popen(
        [server_bin, "-p", str(SERVER_PORT)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd=base_dir,
        text=True
    )
    time.sleep(0.5)

    try:
        # TEST 1: Two Concurrent C Client CLI processes (--ping batch mode)
        log_print("-----------------------------------------------------------------")
        log_print("TEST 1: Two Concurrent C Clients (--ping batch mode)")
        p1 = subprocess.Popen(
            [client_bin, "-p", str(SERVER_PORT), "--ping", "client_alice_batch_ping"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=base_dir, text=True
        )
        p2 = subprocess.Popen(
            [client_bin, "-p", str(SERVER_PORT), "--ping", "client_bob_batch_ping"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=base_dir, text=True
        )
        out1, err1 = p1.communicate(timeout=5)
        out2, err2 = p2.communicate(timeout=5)

        if p1.returncode == 0 and p2.returncode == 0 and "Verified" in out1 and "Verified" in out2:
            log_print("[+] PASS: Both C clients connected concurrently and exchanged PING/PONG!", GREEN)
            passed_count += 1
        else:
            log_print(f"[-] FAIL: Client 1 code={p1.returncode}, Client 2 code={p2.returncode}", RED)
            log_print(f"Client 1 out: {out1}\nerr: {err1}")
            log_print(f"Client 2 out: {out2}\nerr: {err2}")

        # TEST 2: Two Concurrent Persistent TCP Connections with PING/PONG
        log_print("-----------------------------------------------------------------")
        log_print("TEST 2: Two Concurrent Persistent Sockets (Alice & Bob PING/PONG)")
        s_alice = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s_bob = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s_alice.connect((SERVER_HOST, SERVER_PORT))
        s_bob.connect((SERVER_HOST, SERVER_PORT))

        # Alice sends PING
        s_alice.sendall(pack_frame("PING\talice_persistent_1"))
        resp_alice = unpack_frame(s_alice)
        log_print(f"Alice received: {repr(resp_alice)}")

        # Bob sends PING
        s_bob.sendall(pack_frame("PING\tbob_persistent_1"))
        resp_bob = unpack_frame(s_bob)
        log_print(f"Bob received: {repr(resp_bob)}")

        if resp_alice == "PONG\talice_persistent_1\n" and resp_bob == "PONG\tbob_persistent_1\n":
            log_print("[+] PASS: Concurrent persistent socket exchange succeeded!", GREEN)
            passed_count += 1
        else:
            log_print("[-] FAIL: Persistent socket response mismatch", RED)

        # TEST 3: Disconnect Alice, verify Bob is completely unaffected
        log_print("-----------------------------------------------------------------")
        log_print("TEST 3: Client Disconnect Resilience (Close Alice, verify Bob)")
        s_alice.close()
        time.sleep(0.2)  # Allow server to process EOF and clean up slot

        # Bob sends another ping
        s_bob.sendall(pack_frame("PING\tbob_still_alive_after_alice_left"))
        resp_bob2 = unpack_frame(s_bob)
        log_print(f"Bob received after Alice disconnected: {repr(resp_bob2)}")

        if resp_bob2 == "PONG\tbob_still_alive_after_alice_left\n":
            log_print("[+] PASS: Bob unaffected by Alice disconnect; server event loop resilient!", GREEN)
            passed_count += 1
        else:
            log_print("[-] FAIL: Bob did not receive expected PONG after Alice disconnected", RED)

        # TEST 4: Connect Charlie (Client 3) to verify slot reuse and acceptance
        log_print("-----------------------------------------------------------------")
        log_print("TEST 4: Slot Reuse & Client 3 (Charlie) Acceptance")
        s_charlie = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s_charlie.connect((SERVER_HOST, SERVER_PORT))
        s_charlie.sendall(pack_frame("PING\tcharlie_new_connection"))
        resp_charlie = unpack_frame(s_charlie)
        log_print(f"Charlie received: {repr(resp_charlie)}")

        # Check Bob is still alive too
        s_bob.sendall(pack_frame("PING\tbob_with_charlie"))
        resp_bob3 = unpack_frame(s_bob)
        log_print(f"Bob received: {repr(resp_bob3)}")

        if resp_charlie == "PONG\tcharlie_new_connection\n" and resp_bob3 == "PONG\tbob_with_charlie\n":
            log_print("[+] PASS: New client accepted and slot reused cleanly!", GREEN)
            passed_count += 1
        else:
            log_print("[-] FAIL: Client 3 connection or Bob communication failed", RED)

        s_bob.close()
        s_charlie.close()
        time.sleep(0.2)

        # TEST 5: Adverse Network - Fragmentation (Byte-by-byte partial frame delivery)
        log_print("-----------------------------------------------------------------")
        log_print("TEST 5: Partial Frame Stream (Adverse network streaming byte-by-byte)")
        s_frag = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s_frag.connect((SERVER_HOST, SERVER_PORT))

        frame = pack_frame("PING\tfragmented_packet_resilience_test")
        log_print(f"Sending {len(frame)} bytes in 1-byte increments with 20ms delays...")
        for b in frame:
            s_frag.sendall(bytes([b]))
            time.sleep(0.02)

        resp_frag = unpack_frame(s_frag, timeout=3.0)
        log_print(f"Fragmented stream response received: {repr(resp_frag)}")
        if resp_frag == "PONG\tfragmented_packet_resilience_test\n":
            log_print("[+] PASS: Server accumulator buffer successfully reassembled fragmented frame!", GREEN)
            passed_count += 1
        else:
            log_print(f"[-] FAIL: Expected fragmented PONG, got: {repr(resp_frag)}", RED)
        s_frag.close()

        # TEST 6: Coalesced Frames (Two frames merged into single TCP packet)
        log_print("-----------------------------------------------------------------")
        log_print("TEST 6: Coalesced Frames (Two frames in single write)")
        s_coal = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s_coal.connect((SERVER_HOST, SERVER_PORT))

        frame1 = pack_frame("PING\tcoalesced_frame_1")
        frame2 = pack_frame("PING\tcoalesced_frame_2")
        merged = frame1 + frame2

        s_coal.sendall(merged)
        r1 = unpack_frame(s_coal, timeout=2.0)
        r2 = unpack_frame(s_coal, timeout=2.0)
        log_print(f"Response 1: {repr(r1)}")
        log_print(f"Response 2: {repr(r2)}")

        if r1 == "PONG\tcoalesced_frame_1\n" and r2 == "PONG\tcoalesced_frame_2\n":
            log_print("[+] PASS: Both coalesced frames unpacked and responded to accurately!", GREEN)
            passed_count += 1
        else:
            log_print("[-] FAIL: Coalesced frames not unpacked correctly", RED)
        s_coal.close()

        # TEST 7: Interactive CLI Piped Commands & Help Menu Verification
        log_print("-----------------------------------------------------------------")
        log_print("TEST 7: Client Interactive CLI Mode via Piped Stdin")
        cli_proc = subprocess.Popen(
            [client_bin, "-p", str(SERVER_PORT)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=base_dir,
            text=True
        )
        cli_proc.stdin.write("help\n")
        cli_proc.stdin.flush()
        time.sleep(0.2)
        cli_proc.stdin.write("ping cli_pipe_test\n")
        cli_proc.stdin.flush()
        time.sleep(0.4)
        cli_proc.stdin.write("quit\n")
        cli_proc.stdin.flush()
        cli_out, cli_err = cli_proc.communicate(timeout=5)
        log_print(f"Client output:\n{cli_out}")

        if (cli_proc.returncode == 0 and
            "CARO ONLINE CLIENT COMMANDS" in cli_out and
            "PONG: cli_pipe_test" in cli_out and
            "Client exited cleanly" in cli_out):
            log_print("[+] PASS: Interactive CLI processed commands, displayed help, and handled PONG cleanly!", GREEN)
            passed_count += 1
        else:
            log_print(f"[-] FAIL: Interactive CLI output unexpected (returncode={cli_proc.returncode})", RED)

    finally:
        # Shutdown server gracefully
        log_print("-----------------------------------------------------------------")
        log_print("[*] Stopping caro_server gracefully...")
        if server_proc.poll() is None:
            if sys.platform == "win32":
                server_proc.terminate()
            else:
                server_proc.send_signal(signal.SIGINT)
            try:
                server_proc.wait(timeout=3)
                log_print("[+] Server stopped cleanly.", GREEN)
            except subprocess.TimeoutExpired:
                server_proc.kill()
                log_print("[!] Server killed after timeout.", YELLOW)

    log_print("=================================================================", BLUE)
    log_print(f"   FINAL INTEGRATION RESULTS: {passed_count} / {total_tests} SUITES PASSED",
              GREEN if passed_count == total_tests else RED)
    log_print("=================================================================", BLUE)

    if log_file:
        log_file.close()

    return passed_count == total_tests

if __name__ == "__main__":
    success = run_tests()
    sys.exit(0 if success else 1)
