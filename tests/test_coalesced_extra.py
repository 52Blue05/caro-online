#!/usr/bin/env python3
import socket
import struct
import time
import os
import subprocess
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
    except Exception as e:
        return None

def main():
    port = 9450
    # Clean up any lingering port users
    os.system(f"fuser -k {port}/tcp 2>/dev/null || true")
    time.sleep(0.2)

    srv = subprocess.Popen(["./bin/caro_server", "-p", str(port)],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(0.4)
    pid = srv.pid

    print(f"Server started with PID {pid} on port {port}")

    # Measure baseline FDs before connecting canary
    initial_fds = os.listdir(f"/proc/{pid}/fd")
    print(f"[*] Baseline FDs: {len(initial_fds)}: {sorted(initial_fds)}")

    # Canary socket
    canary = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    canary.connect(("127.0.0.1", port))
    canary.sendall(pack_frame("PING\tcanary_init"))
    resp = recv_frame(canary)
    assert resp and "canary_init" in resp, "Canary init failed"
    print("[+] Canary connected and verified responsive.")

    all_passed = True
    test_counts = [16, 17, 25, 50, 100]

    for n in test_counts:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(("127.0.0.1", port))
        t0 = time.time()
        payload = b"".join(pack_frame(f"PING\tbatch_{n}_{i}") for i in range(n))
        s.sendall(payload)

        count = 0
        for i in range(n):
            r = recv_frame(s, timeout=1.0)
            if r and f"batch_{n}_{i}" in r:
                count += 1
            else:
                break
        dur = time.time() - t0
        s.close()
        passed = (count == n) and (dur < 1.0)
        status_str = "PASS" if passed else "FAIL"
        print(f"[{status_str}] Coalesced {n} frames: {count}/{n} received in {dur*1000:.2f} ms")
        if not passed:
            all_passed = False

    # Check canary health
    canary.sendall(pack_frame("PING\tcanary_post"))
    resp = recv_frame(canary, timeout=2.0)
    if resp and "canary_post" in resp:
        print("[+] PASS: Canary survived all coalesced frame bursts!")
    else:
        print("[-] FAIL: Canary did not survive or did not respond!")
        all_passed = False
    canary.close()
    time.sleep(0.3)

    final_fds = os.listdir(f"/proc/{pid}/fd")
    print(f"[*] Final FDs: {len(final_fds)}: {sorted(final_fds)}")
    if len(final_fds) == len(initial_fds):
        print(f"[+] PASS: Exact 0 FD leaks: {len(initial_fds)} -> {len(final_fds)}")
    else:
        diff = set(final_fds) - set(initial_fds)
        print(f"[-] FAIL: FD leak detected: {diff}")
        all_passed = False

    srv.terminate()
    srv.wait()

    if all_passed:
        print("\n>>> ALL COALESCED FRAME TESTS & CANARY VERIFIED SUCCESSFULLY! <<<")
        sys.exit(0)
    else:
        print("\n>>> VERIFICATION FAILED! <<<")
        sys.exit(1)

if __name__ == "__main__":
    main()
