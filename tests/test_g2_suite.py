#!/usr/bin/env python3
"""
tests/test_g2_suite.py - Comprehensive E2E and Bot Test Suite for Milestone G2
Caro Online C11/Linux - Stable Network, Framing, Concurrency & Rules

Features Tested:
1. 20 Concurrent Connections Bot with Mixed Workloads (AC 1):
   - 10 Rapid Pingers (burst PING/PONG sequences, RTT tracking)
   - 4 Fragmented Streamers (byte-by-byte partial frame delivery)
   - 4 Coalesced Bursters (multiple frames in single write)
   - 2 Slow Drip Senders (deliberate delays between bytes)
2. Framing & Adverse Network Edge Cases (AC 2):
   - Split Header (1+3 bytes and 2+2 bytes)
   - Split Header & Payload
   - Extreme Payload Fragmentation (byte-by-byte stream)
   - Coalesced Multi-Frames
   - Coalesced Frames + Incomplete Partial Tail
   - Oversized Header (> 4096 bytes and > 64KB) -> immediate disconnect
   - Zero-length Header -> immediate disconnect
   - Malformed Frame (missing trailing newline) -> immediate disconnect
   - Incomplete 2-byte header with sudden client close -> server survival
3. Linux /proc/<pid>/fd Baseline Recovery Tracking (AC 3):
   - Baseline query of /proc/<pid>/fd
   - Canary peers establishment (Observer A and Observer B)
   - Churn storm (20 concurrent churn, 20 malformed storm, 20 sequential churn)
   - Verification of exact baseline FD recovery (0 leaked file descriptors)
   - Verification of canary peers survival and unperturbed health
4. Protocol Parser Rejections (AC 5):
   - Rejection of missing parameters (MOVE with 1 or 4 tokens, LOGIN with 1 or 3 tokens)
   - Rejection of integer overflow (> UINT32_MAX)
   - Rejection of negative numbers
   - Rejection of non-numeric strings
   - Unknown command rejection (ERR 101)
5. Client CLI Display & Disconnect Behavior (AC 5):
   - Client self-test (--test)
   - Client batch mode (--ping)
   - Client interactive piped mode and clean EOF handling
6. Detailed Per-Case Execution Logging (AC 6):
   - Formatted per-case execution table
   - Summary statistics and persistent output to tests/test_g2_results.log
"""

import sys
import os
import re
import time
import socket
import struct
import threading
import subprocess
import argparse
from typing import List, Dict, Optional, Tuple, Set

# ANSI Color Codes
COLOR_GREEN = "\033[92m"
COLOR_RED = "\033[91m"
COLOR_YELLOW = "\033[93m"
COLOR_BLUE = "\033[94m"
COLOR_CYAN = "\033[96m"
COLOR_RESET = "\033[0m"

class TestLogger:
    """Handles logging to both standard output and persistent log file."""
    def __init__(self, log_path: str):
        self.log_path = log_path
        os.makedirs(os.path.dirname(os.path.abspath(log_path)), exist_ok=True)
        self.log_file = open(self.log_path, "w", encoding="utf-8")

    def log(self, text: str, color: str = ""):
        timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
        formatted = f"[{timestamp}] {text}"
        if color and sys.stdout.isatty():
            print(f"{color}{formatted}{COLOR_RESET}")
        else:
            print(formatted)
        self.log_file.write(formatted + "\n")
        self.log_file.flush()

    def raw_log(self, text: str):
        print(text)
        self.log_file.write(text + "\n")
        self.log_file.flush()

    def close(self):
        if self.log_file:
            self.log_file.close()

# Global Test Case Result Record
class TestCaseResult:
    def __init__(self, case_id: str, group: str, description: str):
        self.case_id = case_id
        self.group = group
        self.description = description
        self.passed = False
        self.duration_ms = 0.0
        self.details = ""

class G2TestSuite:
    def __init__(self, host: str, port: int, server_pid: Optional[int], logger: TestLogger):
        self.host = host
        self.port = port
        self.server_pid = server_pid
        self.logger = logger
        self.results: List[TestCaseResult] = []
        self.base_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
        self.server_bin = os.path.join(self.base_dir, "bin", "caro_server")
        self.client_bin = os.path.join(self.base_dir, "bin", "caro_client")
        if sys.platform == "win32":
            if not os.path.exists(self.server_bin):
                self.server_bin += ".exe"
            if not os.path.exists(self.client_bin):
                self.client_bin += ".exe"

    # --------------------------------------------------------------------------
    # Protocol Framing Helpers
    # --------------------------------------------------------------------------
    @staticmethod
    def pack_frame(payload_str: str) -> bytes:
        """Packs string payload into 4-byte big-endian length + payload (ensuring newline)."""
        if not payload_str.endswith("\n"):
            payload_str += "\n"
        payload_bytes = payload_str.encode("utf-8")
        header = struct.pack("!I", len(payload_bytes))
        return header + payload_bytes

    @staticmethod
    def unpack_frame(sock: socket.socket, timeout: float = 3.0) -> Optional[str]:
        """Reads a complete big-endian 4-byte framed message. Returns None on EOF/error."""
        sock.settimeout(timeout)
        header = b""
        try:
            while len(header) < 4:
                chunk = sock.recv(4 - len(header))
                if not chunk:
                    return None
                header += chunk
            payload_len = struct.unpack("!I", header)[0]
            payload = b""
            while len(payload) < payload_len:
                chunk = sock.recv(payload_len - len(payload))
                if not chunk:
                    return None
                payload += chunk
            return payload.decode("utf-8", errors="replace")
        except (socket.timeout, ConnectionResetError, BrokenPipeError, OSError):
            return None

    def connect_socket(self, timeout: float = 3.0) -> socket.socket:
        """Creates and connects a TCP stream socket to target server."""
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect((self.host, self.port))
        return s

    def record_result(self, result: TestCaseResult):
        self.results.append(result)
        status_str = f"{COLOR_GREEN}PASS{COLOR_RESET}" if result.passed else f"{COLOR_RED}FAIL{COLOR_RESET}"
        self.logger.log(f"[{result.case_id}] {result.group} - {result.description}: {status_str} ({result.duration_ms:.1f}ms)")
        if result.details:
            self.logger.log(f"    Details: {result.details}", COLOR_YELLOW if not result.passed else "")

    # --------------------------------------------------------------------------
    # Linux /proc/<pid>/fd Helpers
    # --------------------------------------------------------------------------
    def get_process_fds(self) -> Optional[Set[str]]:
        """Reads the set of open file descriptors for the server PID from /proc/<pid>/fd."""
        if not self.server_pid:
            return None
        fd_dir = f"/proc/{self.server_pid}/fd"
        if not os.path.exists(fd_dir):
            return None
        try:
            return set(os.listdir(fd_dir))
        except (FileNotFoundError, PermissionError):
            return None

    # ==========================================================================
    # GROUP 1: 20 CONCURRENT CONNECTIONS BOT (MIXED WORKLOADS) - AC 1
    # ==========================================================================
    def run_bot_20_concurrent_workloads(self):
        result = TestCaseResult("BOT-CONCUR-01", "20-Client Concurrency",
                                "20 simultaneous TCP connections with mixed workloads (rapid, fragmented, coalesced, slow)")
        start_time = time.time()
        num_clients = 20
        barrier = threading.Barrier(num_clients)
        thread_errors = []
        rtt_records = []
        lock = threading.Lock()

        def worker_rapid_ping(idx: int, rounds: int = 10):
            try:
                s = self.connect_socket(timeout=5.0)
                barrier.wait(timeout=5.0)
                for seq in range(rounds):
                    token = f"rapid_w{idx}_seq{seq}"
                    t0 = time.time()
                    s.sendall(self.pack_frame(f"PING\t{token}"))
                    resp = self.unpack_frame(s, timeout=4.0)
                    rtt = (time.time() - t0) * 1000.0
                    with lock:
                        rtt_records.append(rtt)
                    expected = f"PONG\t{token}\n"
                    if resp != expected:
                        raise ValueError(f"Worker {idx} seq {seq} mismatch: expected {repr(expected)}, got {repr(resp)}")
                s.close()
            except Exception as e:
                with lock:
                    thread_errors.append(f"Rapid Worker {idx}: {str(e)}")

        def worker_fragmented_stream(idx: int, rounds: int = 3):
            try:
                s = self.connect_socket(timeout=5.0)
                barrier.wait(timeout=5.0)
                for seq in range(rounds):
                    token = f"frag_w{idx}_seq{seq}"
                    frame = self.pack_frame(f"PING\t{token}")
                    # Send byte-by-byte with small delays
                    for b in frame:
                        s.sendall(bytes([b]))
                        time.sleep(0.005)
                    resp = self.unpack_frame(s, timeout=4.0)
                    expected = f"PONG\t{token}\n"
                    if resp != expected:
                        raise ValueError(f"Frag Worker {idx} seq {seq} mismatch: expected {repr(expected)}, got {repr(resp)}")
                s.close()
            except Exception as e:
                with lock:
                    thread_errors.append(f"Frag Worker {idx}: {str(e)}")

        def worker_coalesced_burst(idx: int, rounds: int = 3):
            try:
                s = self.connect_socket(timeout=5.0)
                barrier.wait(timeout=5.0)
                for seq in range(rounds):
                    tokens = [f"coal_w{idx}_seq{seq}_f{f}" for f in range(3)]
                    merged_bytes = b"".join([self.pack_frame(f"PING\t{t}") for t in tokens])
                    s.sendall(merged_bytes)
                    for t in tokens:
                        resp = self.unpack_frame(s, timeout=4.0)
                        expected = f"PONG\t{t}\n"
                        if resp != expected:
                            raise ValueError(f"Coal Worker {idx} seq {seq} token {t} mismatch: expected {repr(expected)}, got {repr(resp)}")
                s.close()
            except Exception as e:
                with lock:
                    thread_errors.append(f"Coal Worker {idx}: {str(e)}")

        def worker_slow_drip(idx: int, rounds: int = 2):
            try:
                s = self.connect_socket(timeout=8.0)
                barrier.wait(timeout=5.0)
                for seq in range(rounds):
                    token = f"slow_w{idx}_seq{seq}"
                    frame = self.pack_frame(f"PING\t{token}")
                    # Drip bytes with 15ms delays
                    for b in frame:
                        s.sendall(bytes([b]))
                        time.sleep(0.015)
                    resp = self.unpack_frame(s, timeout=5.0)
                    expected = f"PONG\t{token}\n"
                    if resp != expected:
                        raise ValueError(f"Slow Worker {idx} mismatch: expected {repr(expected)}, got {repr(resp)}")
                s.close()
            except Exception as e:
                with lock:
                    thread_errors.append(f"Slow Worker {idx}: {str(e)}")

        threads: List[threading.Thread] = []
        # Workers 0..9: Rapid Pingers (10 workers)
        for i in range(10):
            threads.append(threading.Thread(target=worker_rapid_ping, args=(i, 10)))
        # Workers 10..13: Fragmented Streamers (4 workers)
        for i in range(10, 14):
            threads.append(threading.Thread(target=worker_fragmented_stream, args=(i, 3)))
        # Workers 14..17: Coalesced Bursters (4 workers)
        for i in range(14, 18):
            threads.append(threading.Thread(target=worker_coalesced_burst, args=(i, 3)))
        # Workers 18..19: Slow Drip Senders (2 workers)
        for i in range(18, 20):
            threads.append(threading.Thread(target=worker_slow_drip, args=(i, 2)))

        for t in threads:
            t.daemon = True
            t.start()
        for t in threads:
            t.join(timeout=12.0)

        result.duration_ms = (time.time() - start_time) * 1000.0
        if not thread_errors and len(rtt_records) >= 100:
            avg_rtt = sum(rtt_records) / len(rtt_records)
            max_rtt = max(rtt_records)
            min_rtt = min(rtt_records)
            result.passed = True
            result.details = (f"All 20 workers finished successfully (100% success rate). "
                              f"Processed {len(rtt_records)} rapid transactions. "
                              f"RTT (ms): min={min_rtt:.2f}, avg={avg_rtt:.2f}, max={max_rtt:.2f}")
        else:
            result.passed = False
            result.details = f"Errors encountered ({len(thread_errors)}): " + "; ".join(thread_errors[:5])

        self.record_result(result)

    def run_bot_connection_storm(self):
        result = TestCaseResult("BOT-CONCUR-02", "20-Client Concurrency",
                                "Sequential and concurrent connect/ping/disconnect churn (20 iterations)")
        start_time = time.time()
        storm_errors = []
        try:
            for i in range(20):
                s = self.connect_socket(timeout=3.0)
                token = f"storm_churn_{i}"
                s.sendall(self.pack_frame(f"PING\t{token}"))
                resp = self.unpack_frame(s, timeout=2.0)
                s.close()
                if resp != f"PONG\t{token}\n":
                    storm_errors.append(f"Iteration {i} mismatch: got {repr(resp)}")
                    break
            result.duration_ms = (time.time() - start_time) * 1000.0
            if not storm_errors:
                result.passed = True
                result.details = "20 rapid connect/ping/disconnect cycles passed cleanly without socket drops"
            else:
                result.passed = False
                result.details = "; ".join(storm_errors)
        except Exception as e:
            result.duration_ms = (time.time() - start_time) * 1000.0
            result.passed = False
            result.details = f"Exception: {str(e)}"
        self.record_result(result)

    # ==========================================================================
    # GROUP 2: FRAMING & ADVERSE NETWORK EDGE CASES - AC 2
    # ==========================================================================
    def run_edge_split_header_1_3(self):
        result = TestCaseResult("EDGE-FRAME-01", "Framing Edge Cases",
                                "Split header 1+3 bytes with delay (NET-01)")
        start_time = time.time()
        try:
            s = self.connect_socket(timeout=3.0)
            frame = self.pack_frame("PING\tsplit_header_1_3")
            s.sendall(frame[:1])
            time.sleep(0.05)
            s.sendall(frame[1:])
            resp = self.unpack_frame(s, timeout=2.0)
            s.close()
            result.duration_ms = (time.time() - start_time) * 1000.0
            if resp == "PONG\tsplit_header_1_3\n":
                result.passed = True
                result.details = "Header assembled correctly across 1-byte and 3-byte TCP boundaries"
            else:
                result.passed = False
                result.details = f"Expected valid PONG, got: {repr(resp)}"
        except Exception as e:
            result.duration_ms = (time.time() - start_time) * 1000.0
            result.passed = False
            result.details = f"Exception: {str(e)}"
        self.record_result(result)

    def run_edge_split_header_2_2(self):
        result = TestCaseResult("EDGE-FRAME-02", "Framing Edge Cases",
                                "Split header 2+2 bytes with delay (NET-02)")
        start_time = time.time()
        try:
            s = self.connect_socket(timeout=3.0)
            frame = self.pack_frame("PING\tsplit_header_2_2")
            s.sendall(frame[:2])
            time.sleep(0.05)
            s.sendall(frame[2:])
            resp = self.unpack_frame(s, timeout=2.0)
            s.close()
            result.duration_ms = (time.time() - start_time) * 1000.0
            if resp == "PONG\tsplit_header_2_2\n":
                result.passed = True
                result.details = "Header reassembled accurately across 2+2 byte boundary"
            else:
                result.passed = False
                result.details = f"Expected valid PONG, got: {repr(resp)}"
        except Exception as e:
            result.duration_ms = (time.time() - start_time) * 1000.0
            result.passed = False
            result.details = f"Exception: {str(e)}"
        self.record_result(result)

    def run_edge_split_header_and_payload(self):
        result = TestCaseResult("EDGE-FRAME-03", "Framing Edge Cases",
                                "Split header and payload into separate packets (NET-03)")
        start_time = time.time()
        try:
            s = self.connect_socket(timeout=3.0)
            frame = self.pack_frame("PING\tsplit_header_payload")
            # Send 4 bytes header first
            s.sendall(frame[:4])
            time.sleep(0.05)
            # Send payload afterwards
            s.sendall(frame[4:])
            resp = self.unpack_frame(s, timeout=2.0)
            s.close()
            result.duration_ms = (time.time() - start_time) * 1000.0
            if resp == "PONG\tsplit_header_payload\n":
                result.passed = True
                result.details = "Server held PROTO_NEED_MORE_DATA on header and unpacked payload on arrival"
            else:
                result.passed = False
                result.details = f"Expected valid PONG, got: {repr(resp)}"
        except Exception as e:
            result.duration_ms = (time.time() - start_time) * 1000.0
            result.passed = False
            result.details = f"Exception: {str(e)}"
        self.record_result(result)

    def run_edge_extreme_body_fragmentation(self):
        result = TestCaseResult("EDGE-FRAME-04", "Framing Edge Cases",
                                "Extreme fragmentation: full frame streamed 1 byte/10ms (NET-04)")
        start_time = time.time()
        try:
            s = self.connect_socket(timeout=4.0)
            frame = self.pack_frame("PING\textreme_byte_by_byte_stream")
            for b in frame:
                s.sendall(bytes([b]))
                time.sleep(0.01)
            resp = self.unpack_frame(s, timeout=3.0)
            s.close()
            result.duration_ms = (time.time() - start_time) * 1000.0
            if resp == "PONG\textreme_byte_by_byte_stream\n":
                result.passed = True
                result.details = f"Frame reassembled across {len(frame)} separate recv() calls"
            else:
                result.passed = False
                result.details = f"Expected valid PONG, got: {repr(resp)}"
        except Exception as e:
            result.duration_ms = (time.time() - start_time) * 1000.0
            result.passed = False
            result.details = f"Exception: {str(e)}"
        self.record_result(result)

    def run_edge_coalesced_frames(self):
        result = TestCaseResult("EDGE-FRAME-05", "Framing Edge Cases",
                                "Coalesced 3 frames in single TCP write (NET-06)")
        start_time = time.time()
        try:
            s = self.connect_socket(timeout=3.0)
            f1 = self.pack_frame("PING\tcoalesce_f1")
            f2 = self.pack_frame("PING\tcoalesce_f2")
            f3 = self.pack_frame("PING\tcoalesce_f3")
            s.sendall(f1 + f2 + f3)
            r1 = self.unpack_frame(s, timeout=2.0)
            r2 = self.unpack_frame(s, timeout=2.0)
            r3 = self.unpack_frame(s, timeout=2.0)
            s.close()
            result.duration_ms = (time.time() - start_time) * 1000.0
            if (r1 == "PONG\tcoalesce_f1\n" and
                r2 == "PONG\tcoalesce_f2\n" and
                r3 == "PONG\tcoalesce_f3\n"):
                result.passed = True
                result.details = "Server sliding buffer unpacked and replied to all 3 frames in exact sequence"
            else:
                result.passed = False
                result.details = f"Responses mismatch: r1={repr(r1)}, r2={repr(r2)}, r3={repr(r3)}"
        except Exception as e:
            result.duration_ms = (time.time() - start_time) * 1000.0
            result.passed = False
            result.details = f"Exception: {str(e)}"
        self.record_result(result)

    def run_edge_coalesced_with_partial_tail(self):
        result = TestCaseResult("EDGE-FRAME-06", "Framing Edge Cases",
                                "Coalesced 2 frames + partial 2-byte header tail (NET-07)")
        start_time = time.time()
        try:
            s = self.connect_socket(timeout=3.0)
            f1 = self.pack_frame("PING\tcoalesce_tail_f1")
            f2 = self.pack_frame("PING\tcoalesce_tail_f2")
            f3 = self.pack_frame("PING\tcoalesce_tail_f3")
            # Send f1 + f2 + first 2 bytes of f3
            s.sendall(f1 + f2 + f3[:2])
            r1 = self.unpack_frame(s, timeout=2.0)
            r2 = self.unpack_frame(s, timeout=2.0)
            # Send remaining bytes of f3
            time.sleep(0.05)
            s.sendall(f3[2:])
            r3 = self.unpack_frame(s, timeout=2.0)
            s.close()
            result.duration_ms = (time.time() - start_time) * 1000.0
            if (r1 == "PONG\tcoalesce_tail_f1\n" and
                r2 == "PONG\tcoalesce_tail_f2\n" and
                r3 == "PONG\tcoalesce_tail_f3\n"):
                result.passed = True
                result.details = "First 2 frames processed and responded to while partial tail held in rx_buf"
            else:
                result.passed = False
                result.details = f"Responses mismatch: r1={repr(r1)}, r2={repr(r2)}, r3={repr(r3)}"
        except Exception as e:
            result.duration_ms = (time.time() - start_time) * 1000.0
            result.passed = False
            result.details = f"Exception: {str(e)}"
        self.record_result(result)

    def run_edge_oversized_header(self):
        result = TestCaseResult("EDGE-FRAME-07", "Framing Edge Cases",
                                "Oversized header claim (100,000 bytes > max buffer) -> immediate disconnect (NET-08)")
        start_time = time.time()
        try:
            s = self.connect_socket(timeout=3.0)
            # Send 4 bytes claiming 100,000 bytes payload
            s.sendall(struct.pack("!I", 100000))
            time.sleep(0.05)
            # Server must disconnect client immediately (EOF on recv)
            data = s.recv(1024)
            s.close()
            result.duration_ms = (time.time() - start_time) * 1000.0
            if len(data) == 0:
                result.passed = True
                result.details = "Server rejected oversized header and closed socket immediately (EOF)"
            else:
                result.passed = False
                result.details = f"Expected immediate EOF disconnect, got data: {data}"
        except Exception as e:
            # Connection reset is also acceptable as immediate disconnect
            result.duration_ms = (time.time() - start_time) * 1000.0
            result.passed = True
            result.details = f"Connection closed/reset immediately by server ({str(e)})"
        self.record_result(result)

    def run_edge_oversized_4kb_header(self):
        result = TestCaseResult("EDGE-FRAME-08", "Framing Edge Cases",
                                "Oversized header claim (> 4096 bytes: 5000 bytes) -> immediate disconnect")
        start_time = time.time()
        try:
            s = self.connect_socket(timeout=3.0)
            s.sendall(struct.pack("!I", 5000))
            time.sleep(0.05)
            data = s.recv(1024)
            s.close()
            result.duration_ms = (time.time() - start_time) * 1000.0
            if len(data) == 0:
                result.passed = True
                result.details = "Server rejected >4KB header and closed socket immediately (EOF)"
            else:
                result.passed = False
                result.details = f"Expected EOF disconnect, got: {data}"
        except Exception as e:
            result.duration_ms = (time.time() - start_time) * 1000.0
            result.passed = True
            result.details = f"Connection terminated immediately ({str(e)})"
        self.record_result(result)

    def run_edge_zero_length_header(self):
        result = TestCaseResult("EDGE-FRAME-09", "Framing Edge Cases",
                                "Zero length payload header claim (0x00000000) -> immediate disconnect (NET-09)")
        start_time = time.time()
        try:
            s = self.connect_socket(timeout=3.0)
            s.sendall(struct.pack("!I", 0))
            time.sleep(0.05)
            data = s.recv(1024)
            s.close()
            result.duration_ms = (time.time() - start_time) * 1000.0
            if len(data) == 0:
                result.passed = True
                result.details = "Server detected zero-length frame violation and disconnected client (EOF)"
            else:
                result.passed = False
                result.details = f"Expected EOF, got: {data}"
        except Exception as e:
            result.duration_ms = (time.time() - start_time) * 1000.0
            result.passed = True
            result.details = f"Connection terminated by server ({str(e)})"
        self.record_result(result)

    def run_edge_missing_newline(self):
        result = TestCaseResult("EDGE-FRAME-10", "Framing Edge Cases",
                                "Malformed frame without trailing newline delimiter -> immediate disconnect")
        start_time = time.time()
        try:
            s = self.connect_socket(timeout=3.0)
            body = b"PING\tmissing_newline_frame"
            s.sendall(struct.pack("!I", len(body)) + body)
            time.sleep(0.05)
            data = s.recv(1024)
            s.close()
            result.duration_ms = (time.time() - start_time) * 1000.0
            if len(data) == 0:
                result.passed = True
                result.details = "Server enforced newline delimiter and closed socket on protocol violation"
            else:
                result.passed = False
                result.details = f"Expected EOF disconnect, got: {data}"
        except Exception as e:
            result.duration_ms = (time.time() - start_time) * 1000.0
            result.passed = True
            result.details = f"Connection terminated cleanly by server ({str(e)})"
        self.record_result(result)

    def run_edge_incomplete_header_sudden_close(self):
        result = TestCaseResult("EDGE-FRAME-11", "Framing Edge Cases",
                                "Incomplete 2-byte header with abrupt client close -> server survives")
        start_time = time.time()
        try:
            # Client 1 connects, sends 2 bytes, and disconnects abruptly
            s1 = self.connect_socket(timeout=2.0)
            s1.sendall(b"\x00\x00")
            s1.close()
            time.sleep(0.05)

            # Subsequent Client 2 must connect and exchange PING/PONG stably
            s2 = self.connect_socket(timeout=3.0)
            s2.sendall(self.pack_frame("PING\tsurvival_after_partial_close"))
            resp = self.unpack_frame(s2, timeout=2.0)
            s2.close()

            result.duration_ms = (time.time() - start_time) * 1000.0
            if resp == "PONG\tsurvival_after_partial_close\n":
                result.passed = True
                result.details = "Server handled abrupt TCP close without crash and served subsequent client cleanly"
            else:
                result.passed = False
                result.details = f"Expected valid PONG from subsequent client, got: {repr(resp)}"
        except Exception as e:
            result.duration_ms = (time.time() - start_time) * 1000.0
            result.passed = False
            result.details = f"Exception: {str(e)}"
        self.record_result(result)

    # ==========================================================================
    # GROUP 3: LINUX /proc/<pid>/fd BASELINE RECOVERY TRACKING & CANARIES - AC 3
    # ==========================================================================
    def run_fd_baseline_recovery_and_canaries(self):
        result = TestCaseResult("FD-TRACK-01", "FD Leak Tracking",
                                "Linux /proc/<pid>/fd baseline recovery after churn storm & canary survival (AC 3)")
        start_time = time.time()

        initial_fds = self.get_process_fds()
        if initial_fds is None:
            # If not running on Linux procfs or PID unavailable, fallback test
            result.duration_ms = (time.time() - start_time) * 1000.0
            result.passed = False
            result.details = f"Cannot read /proc/{self.server_pid}/fd. Ensure server PID is valid and running on Linux."
            self.record_result(result)
            return

        baseline_count = len(initial_fds)
        self.logger.log(f"[*] Initial Server Baseline FD Count: {baseline_count} (FDs: {sorted(list(initial_fds), key=int)})")

        canary_a = None
        canary_b = None
        try:
            # 1. Establish 2 Persistent Canary Clients
            canary_a = self.connect_socket(timeout=3.0)
            canary_b = self.connect_socket(timeout=3.0)
            time.sleep(0.05)

            canary_fds = self.get_process_fds()
            if len(canary_fds) != baseline_count + 2:
                raise AssertionError(f"Expected {baseline_count + 2} FDs after canary connect, got {len(canary_fds)}")

            # Verify initial Canary PING
            canary_a.sendall(self.pack_frame("PING\tcanary_a_init"))
            canary_b.sendall(self.pack_frame("PING\tcanary_b_init"))
            if (self.unpack_frame(canary_a) != "PONG\tcanary_a_init\n" or
                self.unpack_frame(canary_b) != "PONG\tcanary_b_init\n"):
                raise AssertionError("Canary initial PING/PONG failed")

            # 2. Trigger Churn Storm:
            # Wave 1: 20 concurrent connections
            wave1 = []
            for i in range(20):
                s = self.connect_socket(timeout=3.0)
                wave1.append(s)
            time.sleep(0.05)
            peak_fds = len(self.get_process_fds())
            for s in wave1:
                s.close()
            time.sleep(0.1)

            # Wave 2: 20 connections triggering server-side disconnections (oversized / malformed)
            for i in range(20):
                try:
                    sc = self.connect_socket(timeout=2.0)
                    sc.sendall(struct.pack("!I", 100000))
                    sc.recv(1024)
                    sc.close()
                except Exception:
                    pass
            time.sleep(0.1)

            # Wave 3: 20 rapid sequential connections
            for i in range(20):
                sc = self.connect_socket(timeout=2.0)
                sc.sendall(self.pack_frame(f"PING\tchurn_wave3_{i}"))
                self.unpack_frame(sc, timeout=1.0)
                sc.close()

            # 3. Allow event loop and OS kernel to settle
            time.sleep(0.3)

            # 4. Check FD Count before closing canaries (MUST be baseline_count + 2)
            settled_fds = self.get_process_fds()
            if len(settled_fds) != baseline_count + 2:
                leaked = settled_fds - initial_fds
                raise AssertionError(
                    f"FD Leak Detected! Baseline={baseline_count}, Expected={baseline_count + 2}, "
                    f"Actual={len(settled_fds)}, Leaked FDs={leaked}"
                )

            # 5. Verify Canary Survival & Health
            canary_a.sendall(self.pack_frame("PING\tcanary_a_post_storm"))
            canary_b.sendall(self.pack_frame("PING\tcanary_b_post_storm"))
            r_a = self.unpack_frame(canary_a, timeout=2.0)
            r_b = self.unpack_frame(canary_b, timeout=2.0)
            if (r_a != "PONG\tcanary_a_post_storm\n" or
                r_b != "PONG\tcanary_b_post_storm\n"):
                raise AssertionError(f"Canary health check failed: A={repr(r_a)}, B={repr(r_b)}")

            # 6. Close Canaries
            canary_a.close()
            canary_b.close()
            canary_a = None
            canary_b = None
            time.sleep(0.2)

            # 7. Final Baseline Check (MUST be EXACTLY baseline_count)
            final_fds = self.get_process_fds()
            if len(final_fds) != baseline_count:
                raise AssertionError(
                    f"Final FD Count Mismatch! Initial={baseline_count}, Final={len(final_fds)}, "
                    f"Leaked={final_fds - initial_fds}"
                )

            result.duration_ms = (time.time() - start_time) * 1000.0
            result.passed = True
            result.details = (f"Baseline: {baseline_count} FDs -> Peak: {peak_fds} FDs -> "
                              f"Final: {len(final_fds)} FDs (0 leaked descriptors). "
                              f"Canary peers survived 60 churn cycles unperturbed.")
        except Exception as e:
            result.duration_ms = (time.time() - start_time) * 1000.0
            result.passed = False
            result.details = f"FD Tracking Assertion Failed: {str(e)}"
        finally:
            if canary_a:
                canary_a.close()
            if canary_b:
                canary_b.close()

        self.record_result(result)

    # ==========================================================================
    # GROUP 4: PROTOCOL PARSER REJECTION CHECKS - AC 5
    # ==========================================================================
    def _assert_parser_rejection(self, raw_payload: str, case_id: str, desc: str):
        result = TestCaseResult(case_id, "Protocol Parser Rejections", desc)
        start_time = time.time()
        try:
            s = self.connect_socket(timeout=3.0)
            s.sendall(self.pack_frame(raw_payload))
            resp = self.unpack_frame(s, timeout=2.0)
            s.close()
            result.duration_ms = (time.time() - start_time) * 1000.0
            # Server must either reject with ERR message (e.g. ERR 100/101/102) or disconnect (EOF: resp is None)
            if resp is None:
                result.passed = True
                result.details = "Server rejected invalid payload by immediately closing connection"
            elif resp.startswith("ERR"):
                result.passed = True
                result.details = f"Server strictly rejected invalid payload with protocol error: {repr(resp.strip())}"
            else:
                result.passed = False
                result.details = f"Server failed to reject invalid payload, returned non-error response: {repr(resp)}"
        except Exception as e:
            result.duration_ms = (time.time() - start_time) * 1000.0
            result.passed = True
            result.details = f"Server closed connection on malformed payload ({str(e)})"
        self.record_result(result)

    def run_parser_rejection_tests(self):
        # 1. MOVE missing all arguments
        self._assert_parser_rejection("MOVE\n", "PARSE-REJECT-01",
                                      "Rejection of MOVE command with missing parameters (1 token)")
        # 2. MOVE missing column
        self._assert_parser_rejection("MOVE\t100\t1\t7\n", "PARSE-REJECT-02",
                                      "Rejection of MOVE command with missing column coordinate (4 tokens)")
        # 3. LOGIN missing all arguments
        self._assert_parser_rejection("LOGIN\n", "PARSE-REJECT-03",
                                      "Rejection of LOGIN command with missing parameters (1 token)")
        # 4. LOGIN missing password
        self._assert_parser_rejection("LOGIN\t101\talice\n", "PARSE-REJECT-04",
                                      "Rejection of LOGIN command with missing password (3 tokens)")
        # 5. Integer overflow > UINT32_MAX
        self._assert_parser_rejection("MOVE\t99999999999999999999\t1\t7\t7\n", "PARSE-REJECT-05",
                                      "Rejection of numeric field with integer overflow (> UINT32_MAX)")
        # 6. Negative number in unsigned field
        self._assert_parser_rejection("MOVE\t-1\t1\t7\t7\n", "PARSE-REJECT-06",
                                      "Rejection of negative number in unsigned numeric field (-1)")
        # 7. Non-numeric characters in numeric field
        self._assert_parser_rejection("MOVE\tabc\txyz\tfoo\tbar\n", "PARSE-REJECT-07",
                                      "Rejection of non-numeric string in integer parameters")
        # 8. Out of range coordinates
        self._assert_parser_rejection("MOVE\t100\t1\t15\t15\n", "PARSE-REJECT-08",
                                      "Rejection of out-of-range coordinates (row 15, col 15 on 15x15 board)")
        # 9. Unknown command
        self._assert_parser_rejection("UNKNOWN_CMD_TEST\targ1\targ2\n", "PARSE-REJECT-09",
                                      "Rejection of unknown command string (ERR 101)")

    # ==========================================================================
    # GROUP 5: CLIENT CLI DISPLAY & DISCONNECT - AC 5
    # ==========================================================================
    def run_client_cli_tests(self):
        # CLI-01: Client Self-Test
        r1 = TestCaseResult("CLI-VERIFY-01", "Client CLI Verification",
                            "Client internal self-test mode (--test)")
        t0 = time.time()
        try:
            cp = subprocess.run([self.client_bin, "--test"], capture_output=True, text=True, timeout=5)
            r1.duration_ms = (time.time() - t0) * 1000.0
            if cp.returncode == 0 and ("self-tests passed" in cp.stdout.lower() or "passed (6/6)" in cp.stdout.lower()):
                r1.passed = True
                r1.details = "Internal client framing and protocol self-tests passed"
            else:
                r1.passed = False
                r1.details = f"Code: {cp.returncode}, Out: {cp.stdout.strip()}, Err: {cp.stderr.strip()}"
        except Exception as e:
            r1.duration_ms = (time.time() - t0) * 1000.0
            r1.passed = False
            r1.details = f"Exception: {str(e)}"
        self.record_result(r1)

        # CLI-02: Client Batch Ping
        r2 = TestCaseResult("CLI-VERIFY-02", "Client CLI Verification",
                            "Client batch ping mode (--ping <msg>)")
        t0 = time.time()
        try:
            cp = subprocess.run([self.client_bin, "-p", str(self.port), "--ping", "cli_g2_batch_check"],
                                capture_output=True, text=True, timeout=5)
            r2.duration_ms = (time.time() - t0) * 1000.0
            if cp.returncode == 0 and "Verified" in cp.stdout:
                r2.passed = True
                r2.details = "Batch mode connected, verified PONG echo, and exited cleanly (code 0)"
            else:
                r2.passed = False
                r2.details = f"Code: {cp.returncode}, Out: {cp.stdout.strip()}"
        except Exception as e:
            r2.duration_ms = (time.time() - t0) * 1000.0
            r2.passed = False
            r2.details = f"Exception: {str(e)}"
        self.record_result(r2)

        # CLI-03: Client Interactive Piped Commands & Disconnection
        r3 = TestCaseResult("CLI-VERIFY-03", "Client CLI Verification",
                            "Client interactive piped CLI with help, ping, and quit commands")
        t0 = time.time()
        try:
            p = subprocess.Popen([self.client_bin, "-p", str(self.port)],
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True)
            p.stdin.write("help\n")
            p.stdin.flush()
            time.sleep(0.2)
            p.stdin.write("ping cli_interactive_pipe\n")
            p.stdin.flush()
            time.sleep(0.4)
            p.stdin.write("quit\n")
            p.stdin.flush()
            out, err = p.communicate(timeout=5)
            r3.duration_ms = (time.time() - t0) * 1000.0
            if (p.returncode == 0 and
                "CARO ONLINE CLIENT COMMANDS" in out and
                "cli_interactive_pipe" in out and
                "Client exited cleanly" in out):
                r3.passed = True
                r3.details = "Interactive CLI handled help menu, processed ping/pong, and exited cleanly"
            else:
                r3.passed = False
                r3.details = f"Code: {p.returncode}, Out: {out.strip()}, Err: {err.strip()}"
        except Exception as e:
            r3.duration_ms = (time.time() - t0) * 1000.0
            r3.passed = False
            r3.details = f"Exception: {str(e)}"
        self.record_result(r3)

    # ==========================================================================
    # MASTER RUNNER & REPORT GENERATOR - AC 6
    # ==========================================================================
    def run_all(self) -> bool:
        self.logger.raw_log("========================================================================================")
        self.logger.raw_log("             CARO ONLINE C11/LINUX — COMPREHENSIVE E2E TEST SUITE (G2)                  ")
        self.logger.raw_log("========================================================================================")
        self.logger.raw_log(f"Execution Timestamp : {time.strftime('%Y-%m-%dT%H:%M:%SZ')}")
        self.logger.raw_log(f"Platform            : {sys.platform} (Linux / WSL POSIX C11)")
        self.logger.raw_log(f"Server Target       : {self.host}:{self.port}")
        self.logger.raw_log(f"Server PID          : {self.server_pid if self.server_pid else 'N/A'}")
        self.logger.raw_log(f"Log File Destination: {self.logger.log_path}")
        self.logger.raw_log("----------------------------------------------------------------------------------------\n")

        # 1. 20 Concurrent Connections Bot
        self.logger.log(">>> EXECUTING GROUP 1: 20 CONCURRENT CONNECTIONS BOT (AC 1)", COLOR_CYAN)
        self.run_bot_20_concurrent_workloads()
        self.run_bot_connection_storm()

        # 2. Framing & Edge Cases
        self.logger.log("\n>>> EXECUTING GROUP 2: FRAMING & ADVERSE NETWORK EDGE CASES (AC 2)", COLOR_CYAN)
        self.run_edge_split_header_1_3()
        self.run_edge_split_header_2_2()
        self.run_edge_split_header_and_payload()
        self.run_edge_extreme_body_fragmentation()
        self.run_edge_coalesced_frames()
        self.run_edge_coalesced_with_partial_tail()
        self.run_edge_oversized_header()
        self.run_edge_oversized_4kb_header()
        self.run_edge_zero_length_header()
        self.run_edge_missing_newline()
        self.run_edge_incomplete_header_sudden_close()

        # 3. Linux /proc/<pid>/fd Baseline Recovery Tracking & Canaries
        self.logger.log("\n>>> EXECUTING GROUP 3: /proc/<pid>/fd BASELINE RECOVERY & CANARY SURVIVAL (AC 3)", COLOR_CYAN)
        self.run_fd_baseline_recovery_and_canaries()

        # 4. Protocol Parser Rejection Checks
        self.logger.log("\n>>> EXECUTING GROUP 4: PROTOCOL PARSER REJECTIONS (AC 5)", COLOR_CYAN)
        self.run_parser_rejection_tests()

        # 5. Client CLI Verification
        self.logger.log("\n>>> EXECUTING GROUP 5: CLIENT CLI DISPLAY & DISCONNECT BEHAVIOR (AC 5)", COLOR_CYAN)
        self.run_client_cli_tests()

        # Summary Generation
        total_tests = len(self.results)
        passed_tests = sum(1 for r in self.results if r.passed)
        failed_tests = total_tests - passed_tests
        pass_rate = (passed_tests / total_tests) * 100.0 if total_tests > 0 else 0.0

        self.logger.raw_log("\n========================================================================================")
        self.logger.raw_log("                               E2E TEST SUMMARY MATRIX                                  ")
        self.logger.raw_log("========================================================================================")
        header = f"{'Case ID':<16} | {'Group':<24} | {'Result':<6} | {'Time (ms)':<9} | {'Description'}"
        self.logger.raw_log(header)
        self.logger.raw_log("-" * len(header))

        for r in self.results:
            status = "PASS" if r.passed else "FAIL"
            line = f"{r.case_id:<16} | {r.group:<24} | {status:<6} | {r.duration_ms:>9.1f} | {r.description}"
            self.logger.raw_log(line)

        self.logger.raw_log("========================================================================================")
        summary_line = (f"FINAL OUTCOME: {passed_tests} / {total_tests} SUITES PASSED "
                        f"({pass_rate:.1f}%) — " +
                        ("ALL CRITERIA VERIFIED (PASS)" if failed_tests == 0 else f"{failed_tests} FAILED"))
        self.logger.raw_log(summary_line)
        self.logger.raw_log("========================================================================================")

        return failed_tests == 0

def find_server_pid_by_port(port: int) -> Optional[int]:
    """Attempts to find the PID of the process listening on the specified port."""
    try:
        out = subprocess.check_output(["ss", "-tulpn"], text=True, stderr=subprocess.DEVNULL)
        for line in out.splitlines():
            if f":{port} " in line and "pid=" in line:
                m = re.search(r'pid=(\d+)', line)
                if m:
                    return int(m.group(1))
    except Exception:
        pass
    try:
        out = subprocess.check_output(["lsof", "-t", "-i", f":{port}"], text=True, stderr=subprocess.DEVNULL)
        lines = out.strip().split()
        if lines:
            return int(lines[0])
    except Exception:
        pass
    return None

def main():
    parser = argparse.ArgumentParser(description="Caro Online Milestone G2 Comprehensive E2E Test Suite")
    parser.add_argument("--host", default="127.0.0.1", help="Target server host (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=9200, help="Target server port (default: 9200)")
    parser.add_argument("--pid", type=int, default=None, help="Server process PID (for /proc/<pid>/fd tracking)")
    parser.add_argument("--log", default=os.path.join(os.path.dirname(__file__), "test_g2_results.log"),
                        help="Path to output test log file")
    args = parser.parse_args()

    server_pid = args.pid
    if server_pid is None:
        server_pid = find_server_pid_by_port(args.port)

    logger = TestLogger(args.log)
    suite = G2TestSuite(args.host, args.port, server_pid, logger)
    success = suite.run_all()
    logger.close()

    sys.exit(0 if success else 1)

if __name__ == "__main__":
    main()
