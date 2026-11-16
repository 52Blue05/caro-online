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
    except Exception:
        return None

def main():
    port = 9550
    os.system(f"fuser -k {port}/tcp 2>/dev/null || true")
    time.sleep(0.2)

    srv = subprocess.Popen(["./bin/caro_server", "-p", str(port)],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    time.sleep(0.4)
    pid = srv.pid

    print(f"Server started with PID {pid} on port {port}")

    # Case 1: Incomplete frame (Slowloris attack) - header says 20 bytes payload, but client only sends 5 bytes
    s_slow = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s_slow.connect(("127.0.0.1", port))
    header = struct.pack("!I", 20)
    s_slow.sendall(header + b"12345") # 5 bytes out of 20
    print("[*] Sent partial frame (5/20 bytes). Waiting 11 seconds for SERVER_FRAME_TIMEOUT_SEC (10s)...")
    time.sleep(11.0)

    s_slow.settimeout(2.0)
    try:
        data = s_slow.recv(1024)
        if len(data) == 0:
            print("[+] PASS: Incomplete frame (Slowloris) client disconnected with EOF after 10s timeout!")
        else:
            print(f"[-] FAIL: Slowloris client received unexpected data: {data}")
            sys.exit(1)
    except Exception as e:
        print(f"[-] FAIL: Exception on slow socket: {e}")
        sys.exit(1)
    finally:
        s_slow.close()

    # Case 2: Healthy active client periodically sending frames does NOT time out
    s_healthy = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s_healthy.connect(("127.0.0.1", port))
    for i in range(3):
        s_healthy.sendall(pack_frame(f"PING\thealthy_{i}"))
        resp = recv_frame(s_healthy, timeout=1.0)
        assert resp and f"healthy_{i}" in resp, f"Healthy ping {i} failed"
        time.sleep(1.0)
    s_healthy.close()
    print("[+] PASS: Healthy client operating normally without false timeout.")

    srv.terminate()
    srv.wait()
    print(">>> TIMEOUT CLASSIFICATION VERIFIED EMPIRICALLY! <<<")

if __name__ == "__main__":
    main()
