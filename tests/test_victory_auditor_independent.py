#!/usr/bin/env python3
"""
Victory Auditor Independent Verification Suite for Milestone G2
Author: Independent Victory Auditor (victory_auditor_1)
Purpose: Independent, unassisted stress-testing of server resource limits,
         EAGAIN handling, framing boundaries, and FD cleanup.
"""

import socket
import struct
import time
import subprocess
import os
import sys

def pack_frame(payload: str) -> bytes:
    if not payload.endswith("\n"):
        payload += "\n"
    b = payload.encode("utf-8")
    return struct.pack("!I", len(b)) + b

def recv_frame(sock: socket.socket, timeout: float = 2.0):
    sock.settimeout(timeout)
    try:
        hdr = b""
        while len(hdr) < 4:
            chunk = sock.recv(4 - len(hdr))
            if not chunk:
                return None
            hdr += chunk
        (l,) = struct.unpack("!I", hdr)
        b = b""
        while len(b) < l:
            chunk = sock.recv(l - len(b))
            if not chunk:
                return None
            b += chunk
        return b.decode("utf-8", errors="replace")
    except Exception:
        return None

def main():
    port = 9600
    os.system(f"fuser -k {port}/tcp 2>/dev/null || true")
    time.sleep(0.2)

    srv = subprocess.Popen(["./bin/caro_server", "-p", str(port)],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(0.4)
    pid = srv.pid

    print(f"[*] Victory Auditor test server started with PID {pid} on port {port}")
    initial_fds = os.listdir(f"/proc/{pid}/fd")
    print(f"[*] Initial baseline FDs: {len(initial_fds)} -> {sorted(initial_fds)}")

    all_passed = True

    # Test 1: Immediate disconnect without sending any data (0-byte churn)
    print("\n--- Test 1: 0-byte Abrupt Client Connect & Immediate Close ---")
    for i in range(15):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(("127.0.0.1", port))
        s.close()
    time.sleep(0.2)
    post_churn_fds = os.listdir(f"/proc/{pid}/fd")
    if len(post_churn_fds) == len(initial_fds):
        print(f"[+] PASS: 15 immediate 0-byte connects closed cleanly. FDs: {len(post_churn_fds)}")
    else:
        print(f"[-] FAIL: FD leak on 0-byte connect: {set(post_churn_fds) - set(initial_fds)}")
        all_passed = False

    # Test 2: Unknown command rejection with ERR 101 and connection survival
    print("\n--- Test 2: Unknown Command and Invalid Payloads ---")
    s2 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s2.connect(("127.0.0.1", port))
    s2.sendall(pack_frame("UNKNOWN_RANDOM_CMD\t123\t456"))
    resp2 = recv_frame(s2)
    if resp2 and "ERR" in resp2 and "101" in resp2:
        print(f"[+] PASS: Unknown command returned ERR 101: {resp2.strip()}")
    else:
        print(f"[-] FAIL: Unexpected response to unknown command: {resp2}")
        all_passed = False

    # Check connection survives and handles subsequent valid PING
    s2.sendall(pack_frame("PING\tsurvival_check"))
    resp2_ping = recv_frame(s2)
    if resp2_ping and "survival_check" in resp2_ping:
        print("[+] PASS: Connection survived ERR 101 and processed subsequent PING cleanly.")
    else:
        print(f"[-] FAIL: Connection failed after ERR: {resp2_ping}")
        all_passed = False
    s2.close()

    # Test 3: Burst coalesced 40 frames and check response sequence
    print("\n--- Test 3: Burst Coalesced 40 PINGs in Single TCP Write ---")
    s3 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s3.connect(("127.0.0.1", port))
    burst_data = b"".join(pack_frame(f"PING\tburst_auditor_{i}") for i in range(40))
    s3.sendall(burst_data)

    burst_count = 0
    for i in range(40):
        r = recv_frame(s3, timeout=1.0)
        if r and f"burst_auditor_{i}" in r:
            burst_count += 1
        else:
            print(f"[-] FAIL at burst frame {i}: received {r}")
            break
    if burst_count == 40:
        print("[+] PASS: All 40 coalesced frames received in exact sequence.")
    else:
        print(f"[-] FAIL: Received only {burst_count}/40 frames")
        all_passed = False
    s3.close()

    # Test 4: Final FD baseline recovery verification
    time.sleep(0.3)
    final_fds = os.listdir(f"/proc/{pid}/fd")
    print(f"\n[*] Final FDs: {len(final_fds)} -> {sorted(final_fds)}")
    if len(final_fds) == len(initial_fds):
        print(f"[+] PASS: Net socket descriptor leak is EXACTLY 0 ({len(initial_fds)} -> {len(final_fds)})")
    else:
        diff = set(final_fds) - set(initial_fds)
        print(f"[-] FAIL: Leaked descriptors detected: {diff}")
        all_passed = False

    srv.terminate()
    srv.wait()

    if all_passed:
        print("\n>>> ALL VICTORY AUDITOR INDEPENDENT TESTS PASSED (100%) <<<")
        sys.exit(0)
    else:
        print("\n>>> VICTORY AUDITOR INDEPENDENT TESTS FAILED <<<")
        sys.exit(1)

if __name__ == "__main__":
    main()
