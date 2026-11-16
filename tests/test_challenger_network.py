#!/usr/bin/env python3
"""
tests/test_challenger_network.py
Extended Adversarial Stress & Edge Case Test Suite for Caro Server Milestone G2
"""

import sys
import os
import time
import socket
import struct
import threading
from typing import List, Dict, Optional, Tuple

class ExtendedStressHarness:
    def __init__(self, host: str = "127.0.0.1", port: int = 9300, server_pid: Optional[int] = None):
        self.host = host
        self.port = port
        self.server_pid = server_pid
        self.findings = []
        self.passes = []

    def log(self, msg: str):
        print(f"[{time.strftime('%H:%M:%S')}] {msg}")

    def get_server_fds(self) -> Optional[List[str]]:
        if not self.server_pid:
            return None
        fd_dir = f"/proc/{self.server_pid}/fd"
        try:
            return os.listdir(fd_dir)
        except Exception as e:
            return None

    def pack_frame(self, payload: str) -> bytes:
        if not payload.endswith("\n"):
            payload += "\n"
        b_pay = payload.encode("utf-8")
        header = struct.pack("!I", len(b_pay))
        return header + b_pay

    def recv_frame(self, sock: socket.socket, timeout: float = 3.0) -> Optional[str]:
        sock.settimeout(timeout)
        try:
            hdr = b""
            while len(hdr) < 4:
                chunk = sock.recv(4 - len(hdr))
                if not chunk:
                    return None
                hdr += chunk
            (length,) = struct.unpack("!I", hdr)
            body = b""
            while len(body) < length:
                chunk = sock.recv(length - len(body))
                if not chunk:
                    return None
                body += chunk
            return body.decode("utf-8", errors="replace")
        except socket.timeout:
            return None
        except Exception:
            return None

    # ---------------------------------------------------------
    # Test 1: Coalesced Frames Boundary Tests (16, 17, 25 frames)
    # ---------------------------------------------------------
    def test_coalesced_budget_boundaries(self):
        self.log("=== Test 1: Coalesced Frame Budget Boundary (16 vs 17 frames) ===")
        # Case A: Exactly 16 frames (equals SERVER_FRAME_BUDGET)
        s16 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s16.connect((self.host, self.port))
        s16.sendall(b"".join(self.pack_frame(f"PING\tframe_{i}") for i in range(16)))
        count16 = 0
        for _ in range(16):
            resp = self.recv_frame(s16, timeout=1.0)
            if resp:
                count16 += 1
            else:
                break
        s16.close()
        self.log(f"[*] 16 coalesced frames received: {count16} / 16")
        if count16 == 16:
            self.passes.append("16 coalesced frames (at frame budget boundary) passed cleanly")
        else:
            self.findings.append(f"16 coalesced frames failed: received only {count16} responses")

        # Case B: 17 frames (1 frame above SERVER_FRAME_BUDGET)
        s17 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s17.connect((self.host, self.port))
        s17.sendall(b"".join(self.pack_frame(f"PING\tframe_{i}") for i in range(17)))
        count17 = 0
        for _ in range(17):
            resp = self.recv_frame(s17, timeout=1.0)
            if resp:
                count17 += 1
            else:
                break
        self.log(f"[*] 17 coalesced frames received: {count17} / 17")
        if count17 == 17:
            self.passes.append("17 coalesced frames received all 17")
        else:
            self.findings.append(f"CRITICAL: Event loop starvation on coalesced frames > 16. Client sent 17 frames, server responded to 16 and stalled on 17th!")
            # Now verify if server drops this stalled client after 11 seconds due to incomplete frame timeout
            self.log("[*] Waiting 11 seconds to verify if server mistakenly disconnects client holding 17th frame...")
            time.sleep(11.0)
            s17.settimeout(2.0)
            try:
                extra = s17.recv(1024)
                if len(extra) == 0:
                    self.findings.append("CRITICAL: Stalled client holding 17th frame was terminated by server with EOF (mistaken for slowloris)!")
                    self.log("[!] EMPIRICALLY CONFIRMED: Stalled client terminated with EOF after 10s timeout!")
                else:
                    self.log(f"[*] Received extra data: {extra}")
            except Exception as e:
                self.log(f"[*] Exception reading stalled socket: {e}")
        s17.close()

    # ---------------------------------------------------------
    # Test 2: Maximum Payload Boundary (4096 vs 4097 bytes)
    # ---------------------------------------------------------
    def test_payload_size_boundaries(self):
        self.log("=== Test 2: Payload Size Boundaries (4096 vs 4097 bytes) ===")
        # Case A: Exactly 4096 bytes payload (valid max)
        # Note: PING\t<echo>\n, total payload length must be exactly 4096
        prefix = "PING\t"
        fill_len = 4096 - len(prefix) - 1 # -1 for \n
        payload_4096 = prefix + ("X" * fill_len) + "\n"
        assert len(payload_4096) == 4096

        s_max = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s_max.settimeout(3.0)
        s_max.connect((self.host, self.port))
        header = struct.pack("!I", 4096)
        s_max.sendall(header + payload_4096.encode("ascii"))
        resp = self.recv_frame(s_max, timeout=2.0)
        s_max.close()
        if resp and "PONG" in resp:
            self.passes.append("4096-byte maximum allowed payload accepted and processed successfully")
            self.log("[+] PASS: 4096-byte payload accepted")
        else:
            self.findings.append(f"4096-byte max payload was rejected or failed: {resp}")
            self.log(f"[-] FAIL: 4096-byte max payload failed: {resp}")

        # Case B: 4097 bytes payload (1 byte over PROTO_MAX_PAYLOAD_SIZE)
        s_over = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s_over.settimeout(3.0)
        s_over.connect((self.host, self.port))
        header_over = struct.pack("!I", 4097)
        s_over.sendall(header_over + b"Y" * 100) # send header + partial payload
        # Server must disconnect immediately
        data = s_over.recv(1024)
        s_over.close()
        if len(data) == 0:
            self.passes.append("4097-byte payload (> PROTO_MAX_PAYLOAD_SIZE) immediately closed by server")
            self.log("[+] PASS: 4097-byte oversized frame immediately rejected with connection close")
        else:
            self.findings.append(f"4097-byte oversized frame not disconnected: {data}")
            self.log(f"[-] FAIL: 4097-byte payload did not trigger disconnect")

    # ---------------------------------------------------------
    # Test 3: Maximum Server Capacity (64 clients + 65th rejected)
    # ---------------------------------------------------------
    def test_server_capacity_limit(self):
        self.log("=== Test 3: Server Capacity Limit (64 slots + 65th connection) ===")
        sockets = []
        try:
            # Connect up to 64 clients
            for i in range(64):
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(2.0)
                s.connect((self.host, self.port))
                # Send a ping to verify connection is accepted and active
                s.sendall(self.pack_frame(f"PING\tslot_{i}"))
                resp = self.recv_frame(s, timeout=1.0)
                if not resp or "PONG" not in resp:
                    self.findings.append(f"Failed to connect client {i} up to capacity 64")
                    break
                sockets.append(s)

            self.log(f"[*] Successfully connected {len(sockets)} clients to server (capacity=64)")

            # Attempt 65th connection
            s65 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s65.settimeout(2.0)
            s65.connect((self.host, self.port))
            # The server should close the 65th socket immediately
            data = s65.recv(1024)
            s65.close()
            if len(data) == 0:
                self.passes.append("Server strictly enforced 64-client limit; 65th connection immediately closed (EOF)")
                self.log("[+] PASS: 65th client connection immediately rejected (EOF)")
            else:
                self.findings.append("65th connection was not rejected!")
                self.log("[-] FAIL: 65th connection was not rejected")

        except Exception as e:
            self.findings.append(f"Capacity test exception: {e}")
            self.log(f"[-] FAIL: Exception during capacity test: {e}")
        finally:
            # Close all 64 sockets
            for s in sockets:
                try:
                    s.close()
                except Exception:
                    pass
            time.sleep(0.5)

    # ---------------------------------------------------------
    # Test 4: Concurrency Stress (30 clients x 50 rapid pings)
    # ---------------------------------------------------------
    def test_concurrency_stress(self, num_clients: int = 30, pings_per_client: int = 50):
        self.log(f"=== Test 4: Concurrency Stress ({num_clients} clients x {pings_per_client} pings = {num_clients*pings_per_client}) ===")
        barrier = threading.Barrier(num_clients)
        errors = []

        def client_worker(cid: int):
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.settimeout(5.0)
                s.connect((self.host, self.port))
                barrier.wait()
                for seq in range(pings_per_client):
                    s.sendall(self.pack_frame(f"PING\tconcurrent_{cid}_{seq}"))
                    resp = self.recv_frame(s, timeout=3.0)
                    if not resp or f"concurrent_{cid}_{seq}" not in resp:
                        errors.append(f"Client {cid} seq {seq} failed: {resp}")
                        s.close()
                        return
                s.close()
            except Exception as e:
                errors.append(f"Client {cid} error: {e}")

        threads = [threading.Thread(target=client_worker, args=(i,)) for i in range(num_clients)]
        start_t = time.time()
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        duration = time.time() - start_t

        if not errors:
            self.passes.append(f"{num_clients} concurrent clients exchanged {num_clients*pings_per_client} PING/PONGs in {duration:.2f}s with 0 errors")
            self.log(f"[+] PASS: Concurrency stress succeeded in {duration:.2f}s")
        else:
            self.findings.append(f"Concurrency stress had {len(errors)} errors: {errors[:3]}")
            self.log(f"[-] FAIL: Concurrency stress had {len(errors)} errors: {errors[:3]}")

    # ---------------------------------------------------------
    # Test 5: Linux FD Baseline Recovery & Canary Survival
    # ---------------------------------------------------------
    def test_fd_baseline_and_churn(self, cycles: int = 60):
        self.log(f"=== Test 5: FD Baseline Recovery & Canary Survival ({cycles} cycles) ===")
        fds_initial = self.get_server_fds()
        if fds_initial is None:
            self.log("[-] FD tracking unavailable")
            return

        self.log(f"[*] Initial FDs: {len(fds_initial)} ({sorted(fds_initial)})")

        canary = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        canary.connect((self.host, self.port))
        canary.sendall(self.pack_frame("PING\tcanary_baseline_pre"))
        self.recv_frame(canary)

        # Churn storm
        for i in range(cycles):
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.connect((self.host, self.port))
            if i % 3 == 0:
                s.sendall(self.pack_frame(f"PING\tchurn_{i}"))
                self.recv_frame(s, timeout=1.0)
            elif i % 3 == 1:
                # partial header
                s.sendall(b"\x00\x00\x01")
            s.close()

        time.sleep(0.5)

        # Verify canary is healthy
        canary.sendall(self.pack_frame("PING\tcanary_baseline_post"))
        resp = self.recv_frame(canary, timeout=2.0)
        if resp and "canary_baseline_post" in resp:
            self.passes.append("Canary survived 60 churn cycles unperturbed")
            self.log("[+] PASS: Canary survived churn unperturbed")
        else:
            self.findings.append("Canary failed post-churn verification")
            self.log("[-] FAIL: Canary failed after churn")

        canary.close()
        time.sleep(0.3)

        fds_final = self.get_server_fds()
        self.log(f"[*] Final FDs: {len(fds_final)} ({sorted(fds_final)})")
        if len(fds_final) == len(fds_initial):
            self.passes.append(f"Zero socket FD leaks verified after {cycles} churn cycles ({len(fds_initial)} -> {len(fds_final)})")
            self.log(f"[+] PASS: Exact 0 socket FD leaks ({len(fds_initial)} == {len(fds_final)})")
        else:
            diff = set(fds_final) - set(fds_initial)
            self.findings.append(f"FD leak detected: {diff}")
            self.log(f"[-] FAIL: FD leak detected: {diff}")

    def run_all(self):
        self.test_coalesced_budget_boundaries()
        self.test_payload_size_boundaries()
        self.test_server_capacity_limit()
        self.test_concurrency_stress(num_clients=30, pings_per_client=50)
        self.test_fd_baseline_and_churn(cycles=60)

        self.log("\n=======================================================")
        self.log(f"TOTAL TESTS: {len(self.passes)} PASSED, {len(self.findings)} FINDINGS/BUGS")
        for p in self.passes:
            self.log(f"  [PASS] {p}")
        for f in self.findings:
            self.log(f"  [CRITICAL/HIGH] {f}")
        self.log("=======================================================")

if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=9300)
    parser.add_argument("--pid", type=int, default=None)
    args = parser.parse_args()

    harness = ExtendedStressHarness(host=args.host, port=args.port, server_pid=args.pid)
    harness.run_all()
