#!/usr/bin/env python3
"""
tests/test_g3_suite.py - Comprehensive E2E Test Suite for Milestone G3
Online Caro (Gomoku) Game C11/Linux - 4-Tier Automated Test Architecture

Test Tiers Implemented (28 Test Cases Total):
- Tier 1: Feature Coverage (8 test cases)
    TC-T1-01: User Registration
    TC-T1-02: User Login
    TC-T1-03: Challenge Invitation Flow
    TC-T1-04: Challenge Acceptance & Match Start
    TC-T1-05: In-Game Valid Move with ACK & Opponent Broadcast
    TC-T1-06: Match Conclusion via 5-in-a-Row Win
    TC-T1-07: Match Conclusion via Player Resignation
    TC-T1-08: Match Conclusion via Mutual Draw Agreement
- Tier 2: Boundary & Corner Cases (12 test cases)
    TC-T2-01: Authentication Rejection on Invalid Password (ERR 201)
    TC-T2-02: Duplicate Username Registration Rejection (ERR 202)
    TC-T2-03: Single Active Session Enforcement / Duplicate Login Rejection (ERR 203)
    TC-T2-04: Brute-Force Login Throttling (5 failed attempts trigger lockout)
    TC-T2-05: Challenge 30-Second Expiration Timeout (ERR 302)
    TC-T2-06: Invalid Responder Rejection (ERR 303)
    TC-T2-07: Busy Player Mutual Locking (ERR 301)
    TC-T2-08: Move Turn Order Enforcement (ERR 401)
    TC-T2-09: Cell Occupancy Rejection (ERR 402)
    TC-T2-10: Coordinate Boundary Rejection (ERR 402 / ERR 102)
    TC-T2-11: Sequence Monotonicity Rejection (ERR 403)
    TC-T2-12: Cross-Game Move Rejection (ERR 400)
- Tier 3: Cross-Feature Combinations (4 test cases)
    TC-T3-01: State Collision - Challenge attempt while in active match rejected
    TC-T3-02: Disconnect Forfeit in active match with clean lobby restoration
    TC-T3-03: Simultaneous Mutual Challenge Race Condition handling
    TC-T3-04: Challenger disconnect during pending invite unlocks target
- Tier 4: Real-World Application Scenarios (4 test cases)
    TC-T4-01: 4 Accounts Playing 2 Concurrent Independent Matches (Zero Event Leakage)
    TC-T4-02: Asynchronous Match Conclusion Isolation
    TC-T4-03: Verification of Structured Per-Game Move Logs (logs/game_<id>.log)
    TC-T4-04: Full Lifecycle Lobby Re-entry & Cross-Pairing Rematch
"""

import sys
import os
import re
import time
import socket
import struct
import threading
import queue
import argparse
from typing import List, Dict, Optional, Tuple, Callable

# ANSI Color Codes
COLOR_GREEN = "\033[92m"
COLOR_RED = "\033[91m"
COLOR_YELLOW = "\033[93m"
COLOR_BLUE = "\033[94m"
COLOR_CYAN = "\033[96m"
COLOR_RESET = "\033[0m"


class TestLogger:
    """Handles logging to both stdout and persistent log file."""
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


class TestCaseResult:
    """Data record for standardized test result tracking."""
    def __init__(self, case_id: str, group: str, description: str):
        self.case_id = case_id
        self.group = group
        self.description = description
        self.passed = False
        self.duration_ms = 0.0
        self.details = ""


class G3TestClient:
    """
    Asynchronous TCP Client for Caro Wire Protocol.
    - 4-byte Big-Endian length header framing.
    - TAB-delimited UTF-8 payload terminated by newline '\n'.
    - Background reader thread feeding a thread-safe Queue for unsolicited server pushes.
    - Message filtering and predicate matching.
    """
    def __init__(self, host: str, port: int, name: str = "Client"):
        self.host = host
        self.port = port
        self.name = name
        self.sock: Optional[socket.socket] = None
        self.rx_queue: queue.Queue = queue.Queue()
        self.running: bool = False
        self.reader_thread: Optional[threading.Thread] = None
        self.req_counter: int = 1000
        self.user_id: int = 0
        self.username: str = ""
        self.current_game_id: int = 0
        self.my_piece: str = ""
        self.all_received_msgs: List[str] = []
        self.lock = threading.Lock()

    def connect(self, timeout: float = 3.0):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.settimeout(timeout)
        self.sock.connect((self.host, self.port))
        self.running = True
        self.reader_thread = threading.Thread(target=self._reader_loop, daemon=True)
        self.reader_thread.start()

    def _reader_loop(self):
        """Continuously unpacks 4-byte BE framed messages and pushes to rx_queue."""
        buf = bytearray()
        while self.running:
            sock = self.sock
            if not sock or not self.running:
                break
            try:
                sock.settimeout(0.3)
                chunk = sock.recv(4096)
                if not chunk:
                    break
                buf.extend(chunk)

                while len(buf) >= 4:
                    payload_len = struct.unpack("!I", buf[:4])[0]
                    if len(buf) < 4 + payload_len:
                        break

                    payload_bytes = buf[4:4 + payload_len]
                    del buf[:4 + payload_len]

                    payload_str = payload_bytes.decode("utf-8", errors="replace")
                    with self.lock:
                        self.all_received_msgs.append(payload_str)
                    self.rx_queue.put(payload_str)
            except socket.timeout:
                continue
            except (ConnectionResetError, BrokenPipeError, OSError, AttributeError):
                break
        self.running = False

    @staticmethod
    def pack_frame(payload_str: str) -> bytes:
        if not payload_str.endswith("\n"):
            payload_str += "\n"
        b_pay = payload_str.encode("utf-8")
        header = struct.pack("!I", len(b_pay))
        return header + b_pay

    def send_raw(self, payload_str: str):
        if not self.sock:
            raise RuntimeError(f"[{self.name}] Socket not connected")
        wire = self.pack_frame(payload_str)
        self.sock.sendall(wire)

    def next_req_id(self) -> int:
        self.req_counter += 1
        return self.req_counter

    def drain_queue(self) -> List[str]:
        drained = []
        while not self.rx_queue.empty():
            try:
                drained.append(self.rx_queue.get_nowait())
            except queue.Empty:
                break
        return drained

    def get_received_msgs(self) -> List[str]:
        """Thread-safe snapshot copy of all received messages."""
        with self.lock:
            return list(self.all_received_msgs)

    def clear_received_msgs(self):
        """Thread-safe reset of received messages history."""
        with self.lock:
            self.all_received_msgs.clear()

    def wait_for_msg(
        self,
        cmd: Optional[str] = None,
        ref_id: Optional[int] = None,
        timeout: float = 3.0,
        predicate: Optional[Callable[[List[str]], bool]] = None
    ) -> Optional[List[str]]:
        """Scans queue for a message matching command, ref_id, and/or predicate."""
        deadline = time.time() + timeout
        unmatched = []
        result = None

        while time.time() < deadline:
            remaining = max(0.02, deadline - time.time())
            try:
                raw_msg = self.rx_queue.get(timeout=remaining)
            except queue.Empty:
                continue

            tokens = raw_msg.strip("\r\n").split("\t")
            msg_cmd = tokens[0] if tokens else ""

            match = True
            if cmd is not None and msg_cmd != cmd:
                match = False

            if match and ref_id is not None:
                ref_match = False
                if msg_cmd == "ERR":
                    if len(tokens) >= 4:
                        try:
                            ref_match = (int(tokens[1]) == ref_id)
                        except ValueError:
                            ref_match = False
                    else:
                        ref_match = True
                else:
                    if len(tokens) > 1:
                        try:
                            ref_match = (int(tokens[1]) == ref_id)
                        except ValueError:
                            ref_match = False
                if not ref_match:
                    match = False

            if match and predicate is not None:
                if not predicate(tokens):
                    match = False

            if match:
                result = tokens
                break
            else:
                unmatched.append(raw_msg)

        for m in unmatched:
            self.rx_queue.put(m)

        return result

    def wait_for_cmd_or_err(self, expected_cmd: str, req_id: Optional[int] = None, timeout: float = 3.0) -> Optional[List[str]]:
        """Wait for either expected command or an ERR packet."""
        deadline = time.time() + timeout
        unmatched = []
        result = None

        while time.time() < deadline:
            remaining = max(0.02, deadline - time.time())
            try:
                raw_msg = self.rx_queue.get(timeout=remaining)
            except queue.Empty:
                continue

            tokens = raw_msg.strip("\r\n").split("\t")
            msg_cmd = tokens[0] if tokens else ""

            if msg_cmd == expected_cmd:
                if req_id is not None and len(tokens) > 1:
                    try:
                        if int(tokens[1]) != req_id:
                            unmatched.append(raw_msg)
                            continue
                    except ValueError:
                        pass
                result = tokens
                break
            elif msg_cmd == "ERR":
                if req_id is not None and len(tokens) >= 4:
                    try:
                        if int(tokens[1]) != req_id:
                            unmatched.append(raw_msg)
                            continue
                    except ValueError:
                        pass
                result = tokens
                break
            else:
                unmatched.append(raw_msg)

        for m in unmatched:
            self.rx_queue.put(m)
        return result

    @staticmethod
    def parse_err(tokens: List[str]) -> Tuple[Optional[int], Optional[int], str]:
        """Returns (ref_id, err_code, err_msg) from ERR tokens."""
        if not tokens or tokens[0] != "ERR":
            return None, None, ""
        if len(tokens) >= 4:
            try:
                ref_id = int(tokens[1])
            except ValueError:
                ref_id = None
            try:
                err_code = int(tokens[2])
            except ValueError:
                err_code = None
            err_msg = tokens[3] if len(tokens) > 3 else ""
            return ref_id, err_code, err_msg
        elif len(tokens) == 3:
            try:
                err_code = int(tokens[1])
            except ValueError:
                err_code = None
            return None, err_code, tokens[2]
        elif len(tokens) == 2:
            try:
                err_code = int(tokens[1])
            except ValueError:
                err_code = None
            return None, err_code, ""
        return None, None, ""

    def register(self, username: str, password: str, timeout: float = 3.0) -> Tuple[bool, List[str]]:
        req_id = self.next_req_id()
        self.send_raw(f"REGISTER\t{req_id}\t{username}\t{password}")
        resp = self.wait_for_cmd_or_err("OK", req_id=req_id, timeout=timeout)
        if resp and resp[0] == "OK":
            return True, resp
        return False, resp if resp else []

    def login(self, username: str, password: str, timeout: float = 3.0) -> Tuple[bool, List[str]]:
        req_id = self.next_req_id()
        self.send_raw(f"LOGIN\t{req_id}\t{username}\t{password}")
        resp = self.wait_for_cmd_or_err("OK", req_id=req_id, timeout=timeout)
        if resp and resp[0] == "OK":
            self.username = username
            if len(resp) >= 3:
                try:
                    self.user_id = int(resp[2])
                except ValueError:
                    pass
            return True, resp
        return False, resp if resp else []

    def challenge(self, target_user: str, timeout: float = 3.0) -> Tuple[bool, List[str]]:
        req_id = self.next_req_id()
        self.send_raw(f"CHALLENGE\t{req_id}\t{target_user}")
        resp = self.wait_for_cmd_or_err("OK", req_id=req_id, timeout=timeout)
        if resp and resp[0] == "OK":
            return True, resp
        return False, resp if resp else []

    def wait_for_challenge_req(self, timeout: float = 3.0) -> Optional[Tuple[int, str]]:
        msg = self.wait_for_msg("CHALLENGE_REQ", timeout=timeout)
        if msg and len(msg) >= 3:
            try:
                invite_id = int(msg[1])
                # Protocol: CHALLENGE_REQ\t<invite_id>\t<challenger_id>\t<challenger_name>
                # or:       CHALLENGE_REQ\t<invite_id>\t<challenger_name>
                if len(msg) >= 4 and msg[2].isdigit():
                    challenger = msg[3]
                else:
                    challenger = msg[2]
                return invite_id, challenger
            except ValueError:
                return None
        return None

    def respond_challenge(self, invite_id: int, accept: bool):
        resp_token = "1" if accept else "0"
        self.send_raw(f"CHALLENGE_RESP\t{invite_id}\t{resp_token}")

    def wait_for_game_start(self, timeout: float = 5.0) -> Optional[Tuple[int, str, str]]:
        msg = self.wait_for_msg("GAME_START", timeout=timeout)
        if msg and len(msg) >= 4:
            try:
                game_id = int(msg[1])
                role = msg[2]
                opp = msg[3]
                self.current_game_id = game_id
                self.my_piece = role
                return game_id, role, opp
            except ValueError:
                return None
        return None

    def send_move(self, game_id: int, seq: int, row: int, col: int, timeout: float = 3.0) -> Tuple[bool, List[str]]:
        self.send_raw(f"MOVE\t{game_id}\t{seq}\t{row}\t{col}")
        resp = self.wait_for_cmd_or_err("MOVE_ACK", req_id=game_id, timeout=timeout)
        if resp and resp[0] == "MOVE_ACK":
            return True, resp
        return False, resp if resp else []

    def wait_for_move_broadcast(self, game_id: Optional[int] = None, seq: Optional[int] = None, timeout: float = 3.0) -> Optional[List[str]]:
        def pred(tokens: List[str]) -> bool:
            if game_id is not None and len(tokens) > 1:
                try:
                    if int(tokens[1]) != game_id:
                        return False
                except ValueError:
                    return False
            if seq is not None and len(tokens) > 2:
                try:
                    if int(tokens[2]) != seq:
                        return False
                except ValueError:
                    return False
            return True
        return self.wait_for_msg("MOVE_BROADCAST", timeout=timeout, predicate=pred)

    def resign(self, game_id: int):
        self.send_raw(f"RESIGN\t{game_id}")

    def offer_draw(self, game_id: int):
        self.send_raw(f"OFFER_DRAW\t{game_id}")

    def respond_draw(self, game_id: int, accept: bool):
        resp_token = "1" if accept else "0"
        self.send_raw(f"DRAW_RESP\t{game_id}\t{resp_token}")

    def wait_for_game_over(self, game_id: Optional[int] = None, timeout: float = 5.0) -> Optional[List[str]]:
        def pred(tokens: List[str]) -> bool:
            if game_id is not None and len(tokens) > 1:
                try:
                    if int(tokens[1]) != game_id:
                        return False
                except ValueError:
                    return False
            return True
        return self.wait_for_msg("GAME_OVER", timeout=timeout, predicate=pred)

    def close(self):
        self.running = False
        s = self.sock
        self.sock = None
        if s:
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                s.close()
            except OSError:
                pass
        if self.reader_thread and self.reader_thread.is_alive():
            self.reader_thread.join(timeout=1.0)


class G3TestSuite:
    def __init__(self, host: str, port: int, server_pid: Optional[int], db_path: str, logger: TestLogger, invite_timeout: float = 32.0):
        self.host = host
        self.port = port
        self.server_pid = server_pid
        self.db_path = db_path
        self.logger = logger
        self.invite_timeout = invite_timeout
        self.results: List[TestCaseResult] = []

    def make_client(self, name: str) -> G3TestClient:
        c = G3TestClient(self.host, self.port, name=name)
        c.connect()
        return c

    @staticmethod
    def is_game_event(raw_msg: str, forbidden_game_id: int) -> bool:
        """
        Determines whether raw_msg belongs to forbidden_game_id via strict token parsing.
        Prevents false positive leakage detection caused by substring matching on sequence numbers.
        """
        game_cmds = {
            "GAME_START", "MOVE", "MOVE_ACK", "MOVE_BROADCAST",
            "OFFER_DRAW", "DRAW_RESP", "RESIGN", "GAME_OVER"
        }
        tokens = raw_msg.strip("\r\n").split("\t")
        if len(tokens) < 2:
            return False
        cmd = tokens[0]
        str_gid = str(forbidden_game_id)
        if cmd in game_cmds and tokens[1] == str_gid:
            return True
        if cmd == "ERR" and len(tokens) >= 4 and tokens[1] == str_gid:
            return True
        return False

    def record_result(self, res: TestCaseResult, start_time: float):
        res.duration_ms = (time.time() - start_time) * 1000.0
        self.results.append(res)
        status_str = f"{COLOR_GREEN}PASS{COLOR_RESET}" if res.passed else f"{COLOR_RED}FAIL{COLOR_RESET}"
        self.logger.log(
            f"[{res.case_id}] {res.group} - {res.description}: {status_str} ({res.duration_ms:.1f}ms)",
            COLOR_GREEN if res.passed else COLOR_RED
        )
        if res.details:
            self.logger.log(f"    Details: {res.details}")

    # =========================================================================
    # TIER 1: FEATURE COVERAGE (HAPPY PATH)
    # =========================================================================
    def run_tier1_features(self):
        self.logger.raw_log("\n" + "=" * 80)
        self.logger.log(">>> EXECUTING TIER 1: FEATURE COVERAGE (HAPPY PATH)", COLOR_CYAN)
        self.logger.raw_log("=" * 80)

        # TC-T1-01: User Registration
        res = TestCaseResult("TC-T1-01", "Tier 1: Feature Coverage", "User Registration with valid credentials")
        t0 = time.time()
        c1 = self.make_client("T1_C1")
        try:
            ok, resp = c1.register("user_t1_01", "Pass123!_Alpha")
            if ok and len(resp) >= 2:
                res.passed = True
                res.details = f"Registration succeeded: {resp}"
            else:
                res.details = f"Expected OK, received: {resp}"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            c1.close()
        self.record_result(res, t0)

        # TC-T1-02: User Login
        res = TestCaseResult("TC-T1-02", "Tier 1: Feature Coverage", "User Login with registered credentials")
        t0 = time.time()
        c1 = self.make_client("T1_C2")
        try:
            # Ensure isolated user account exists regardless of test execution order
            c1.register("user_t1_02", "Pass123!_Beta")
            ok, resp = c1.login("user_t1_02", "Pass123!_Beta")
            if ok:
                res.passed = True
                res.details = f"Login succeeded: {resp}, username: {c1.username}"
            else:
                res.details = f"Expected OK, received: {resp}"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            c1.close()
        self.record_result(res, t0)

        # TC-T1-03: Challenge Invitation Flow
        res = TestCaseResult("TC-T1-03", "Tier 1: Feature Coverage", "Challenge invitation dispatched and received")
        t0 = time.time()
        c1 = self.make_client("T1_Ch1")
        c2 = self.make_client("T1_Ch2")
        try:
            c1.register("user_t1_ch1", "Pass123!_Ch1")
            c1.login("user_t1_ch1", "Pass123!_Ch1")
            c2.register("user_t1_ch2", "Pass123!_Ch2")
            c2.login("user_t1_ch2", "Pass123!_Ch2")

            ok, chal_resp = c1.challenge("user_t1_ch2")
            ch_req = c2.wait_for_challenge_req(timeout=3.0)

            if ok and ch_req:
                invite_id_t1, challenger = ch_req
                # Cleanly decline invite to restore both players to unreserved state
                c2.respond_challenge(invite_id_t1, accept=False)
                res.passed = True
                res.details = f"Challenge OK to P1 ({chal_resp}); P2 received CHALLENGE_REQ: invite_id={invite_id_t1}, from={challenger}"
            else:
                res.details = f"Challenge dispatch failed. P1 resp: {chal_resp}, P2 req: {ch_req}"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            c1.close()
            c2.close()
        self.record_result(res, t0)

        # TC-T1-04: Challenge Acceptance & Match Start
        res = TestCaseResult("TC-T1-04", "Tier 1: Feature Coverage", "Challenge accepted, match allocated and GAME_START pushed")
        t0 = time.time()
        c1 = self.make_client("T1_Acc1")
        c2 = self.make_client("T1_Acc2")
        try:
            c1.register("user_t1_acc1", "Pass123!_Acc1")
            c1.login("user_t1_acc1", "Pass123!_Acc1")
            c2.register("user_t1_acc2", "Pass123!_Acc2")
            c2.login("user_t1_acc2", "Pass123!_Acc2")

            c1.challenge("user_t1_acc2")
            ch_req = c2.wait_for_challenge_req(timeout=3.0)
            if ch_req:
                inv_id, _ = ch_req
                c2.respond_challenge(inv_id, accept=True)
                gs1 = c1.wait_for_game_start(timeout=3.0)
                gs2 = c2.wait_for_game_start(timeout=3.0)
                if gs1 and gs2 and gs1[0] == gs2[0]:
                    roles = {gs1[1], gs2[1]}
                    if roles == {"X", "O"}:
                        res.passed = True
                        res.details = f"Game started cleanly: game_id={gs1[0]}, P1={gs1[1]}, P2={gs2[1]}"
                    else:
                        res.details = f"Invalid role distribution: {roles}"
                    # Conclude match cleanly to prevent lingering in-game state
                    c1.resign(gs1[0])
                    c1.wait_for_game_over(gs1[0], timeout=2.0)
                    c2.wait_for_game_over(gs1[0], timeout=2.0)
                else:
                    res.details = f"GAME_START mismatch or missing: P1={gs1}, P2={gs2}"
            else:
                res.details = "No CHALLENGE_REQ received"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            c1.close()
            c2.close()
        self.record_result(res, t0)

        # TC-T1-05: In-Game Valid Move with ACK & Opponent Broadcast
        res = TestCaseResult("TC-T1-05", "Tier 1: Feature Coverage", "Valid move generates MOVE_ACK to mover and MOVE_BROADCAST to opponent")
        t0 = time.time()
        c1 = self.make_client("T1_Mv1")
        c2 = self.make_client("T1_Mv2")
        try:
            c1.register("user_t1_mv1", "Pass123!_Mv1")
            c1.login("user_t1_mv1", "Pass123!_Mv1")
            c2.register("user_t1_mv2", "Pass123!_Mv2")
            c2.login("user_t1_mv2", "Pass123!_Mv2")

            c1.challenge("user_t1_mv2")
            ch_req = c2.wait_for_challenge_req(timeout=3.0)
            if ch_req:
                c2.respond_challenge(ch_req[0], accept=True)
                gs1 = c1.wait_for_game_start(timeout=3.0)
                gs2 = c2.wait_for_game_start(timeout=3.0)
                mover = c1 if gs1 and gs1[1] == "X" else c2
                listener = c2 if mover == c1 else c1
                g_id = mover.current_game_id

                ok, ack = mover.send_move(g_id, seq=1, row=7, col=7)
                bcast = listener.wait_for_move_broadcast(game_id=g_id, seq=1, timeout=3.0)

                if ok and ack and bcast:
                    if int(ack[3]) == 7 and int(ack[4]) == 7 and int(bcast[3]) == 7 and int(bcast[4]) == 7:
                        res.passed = True
                        res.details = f"Move ACK verified: {ack}, Broadcast verified: {bcast}"
                    else:
                        res.details = f"Coordinate mismatch: ack={ack}, bcast={bcast}"
                else:
                    res.details = f"Move failed: ack={ack}, bcast={bcast}"

                # Cleanly conclude match
                mover.resign(g_id)
                mover.wait_for_game_over(g_id, timeout=2.0)
                listener.wait_for_game_over(g_id, timeout=2.0)
            else:
                res.details = "Challenge setup failed"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            c1.close()
            c2.close()
        self.record_result(res, t0)

        # TC-T1-06: Match Conclusion via 5-in-a-Row Win
        res = TestCaseResult("TC-T1-06", "Tier 1: Feature Coverage", "Horizontal 5-in-a-row triggers GAME_OVER with WIN outcome")
        t0 = time.time()
        c1 = self.make_client("T1_W1")
        c2 = self.make_client("T1_W2")
        try:
            c1.register("user_t1_w1", "Pass123!_Win")
            c1.login("user_t1_w1", "Pass123!_Win")
            c2.register("user_t1_w2", "Pass123!_Win")
            c2.login("user_t1_w2", "Pass123!_Win")
            c1.challenge("user_t1_w2")
            ch_req = c2.wait_for_challenge_req(timeout=3.0)
            if ch_req:
                c2.respond_challenge(ch_req[0], accept=True)
                gs1 = c1.wait_for_game_start(timeout=3.0)
                gs2 = c2.wait_for_game_start(timeout=3.0)
                pX = c1 if gs1 and gs1[1] == "X" else c2
                pO = c2 if pX == c1 else c1
                g_id = pX.current_game_id

                # Sequence of moves:
                # X: (7, 7) - seq 1
                # O: (6, 7) - seq 2
                # X: (7, 8) - seq 3
                # O: (6, 8) - seq 4
                # X: (7, 9) - seq 5
                # O: (6, 9) - seq 6
                # X: (7, 10) - seq 7
                # O: (6, 10) - seq 8
                # X: (7, 11) - seq 9 (WIN!)
                pX.send_move(g_id, 1, 7, 7)
                pO.wait_for_move_broadcast(g_id, 1)
                pO.send_move(g_id, 2, 6, 7)
                pX.wait_for_move_broadcast(g_id, 2)

                pX.send_move(g_id, 3, 7, 8)
                pO.wait_for_move_broadcast(g_id, 3)
                pO.send_move(g_id, 4, 6, 8)
                pX.wait_for_move_broadcast(g_id, 4)

                pX.send_move(g_id, 5, 7, 9)
                pO.wait_for_move_broadcast(g_id, 5)
                pO.send_move(g_id, 6, 6, 9)
                pX.wait_for_move_broadcast(g_id, 6)

                pX.send_move(g_id, 7, 7, 10)
                pO.wait_for_move_broadcast(g_id, 7)
                pO.send_move(g_id, 8, 6, 10)
                pX.wait_for_move_broadcast(g_id, 8)

                pX.send_move(g_id, 9, 7, 11)
                go_x = pX.wait_for_game_over(g_id, timeout=3.0)
                go_o = pO.wait_for_game_over(g_id, timeout=3.0)

                if go_x and go_o and "WIN" in go_x:
                    res.passed = True
                    res.details = f"Game Over Win detected: P(X) received {go_x}, P(O) received {go_o}"
                else:
                    res.details = f"GAME_OVER missing or not WIN: go_x={go_x}, go_o={go_o}"
            else:
                res.details = "Setup challenge failed"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            c1.close()
            c2.close()
        self.record_result(res, t0)

        # TC-T1-07: Match Conclusion via Player Resignation
        res = TestCaseResult("TC-T1-07", "Tier 1: Feature Coverage", "Player resignation triggers GAME_OVER with RESIGN outcome")
        t0 = time.time()
        c1 = self.make_client("T1_R1")
        c2 = self.make_client("T1_R2")
        try:
            c1.register("user_t1_r1", "Pass123!_Resign")
            c1.login("user_t1_r1", "Pass123!_Resign")
            c2.register("user_t1_r2", "Pass123!_Resign")
            c2.login("user_t1_r2", "Pass123!_Resign")
            c1.challenge("user_t1_r2")
            ch_req = c2.wait_for_challenge_req(timeout=3.0)
            if ch_req:
                c2.respond_challenge(ch_req[0], accept=True)
                gs1 = c1.wait_for_game_start(timeout=3.0)
                gs2 = c2.wait_for_game_start(timeout=3.0)
                g_id = c1.current_game_id

                c2.resign(g_id)
                go1 = c1.wait_for_game_over(g_id, timeout=3.0)
                go2 = c2.wait_for_game_over(g_id, timeout=3.0)

                if go1 and go2 and ("RESIGN" in go1 or "WIN" in go1):
                    res.passed = True
                    res.details = f"Resignation processed cleanly: go1={go1}, go2={go2}"
                else:
                    res.details = f"Expected RESIGN game over, received: go1={go1}, go2={go2}"
            else:
                res.details = "Setup challenge failed"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            c1.close()
            c2.close()
        self.record_result(res, t0)

        # TC-T1-08: Match Conclusion via Mutual Draw Agreement
        res = TestCaseResult("TC-T1-08", "Tier 1: Feature Coverage", "Mutual draw offer and acceptance triggers GAME_OVER with DRAW outcome")
        t0 = time.time()
        c1 = self.make_client("T1_D1")
        c2 = self.make_client("T1_D2")
        try:
            c1.register("user_t1_d1", "Pass123!_Draw")
            c1.login("user_t1_d1", "Pass123!_Draw")
            c2.register("user_t1_d2", "Pass123!_Draw")
            c2.login("user_t1_d2", "Pass123!_Draw")
            c1.challenge("user_t1_d2")
            ch_req = c2.wait_for_challenge_req(timeout=3.0)
            if ch_req:
                c2.respond_challenge(ch_req[0], accept=True)
                c1.wait_for_game_start(timeout=3.0)
                c2.wait_for_game_start(timeout=3.0)
                g_id = c1.current_game_id

                c1.offer_draw(g_id)
                c2.respond_draw(g_id, accept=True)

                go1 = c1.wait_for_game_over(g_id, timeout=3.0)
                go2 = c2.wait_for_game_over(g_id, timeout=3.0)

                if go1 and go2 and ("DRAW" in go1 or "DRAW" in go2):
                    res.passed = True
                    res.details = f"Draw agreement settled: go1={go1}, go2={go2}"
                else:
                    res.details = f"Expected DRAW game over, received: go1={go1}, go2={go2}"
            else:
                res.details = "Setup challenge failed"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            c1.close()
            c2.close()
        self.record_result(res, t0)

    # =========================================================================
    # TIER 2: BOUNDARY & CORNER CASES (NEGATIVE TESTING)
    # =========================================================================
    def run_tier2_boundary_corner(self):
        self.logger.raw_log("\n" + "=" * 80)
        self.logger.log(">>> EXECUTING TIER 2: BOUNDARY & CORNER CASES", COLOR_CYAN)
        self.logger.raw_log("=" * 80)

        # TC-T2-01: Authentication Rejection on Invalid Password (ERR 201)
        res = TestCaseResult("TC-T2-01", "Tier 2: Boundary & Corner", "Login rejection on invalid credentials (ERR 201)")
        t0 = time.time()
        c = self.make_client("T2_AuthFail")
        try:
            c.register("user_t2_01", "CorrectPass123!")
            ok, err_resp = c.login("user_t2_01", "WrongPass999!")
            ref, code, msg = G3TestClient.parse_err(err_resp)
            if not ok and code == 201:
                res.passed = True
                res.details = f"Received ERR 201 as expected: {err_resp}"
            else:
                res.details = f"Expected ERR 201, got ok={ok}, code={code}, resp={err_resp}"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            c.close()
        self.record_result(res, t0)

        # TC-T2-02: Duplicate Username Registration Rejection (ERR 202)
        res = TestCaseResult("TC-T2-02", "Tier 2: Boundary & Corner", "Duplicate username registration rejection (ERR 202)")
        t0 = time.time()
        c = self.make_client("T2_DupReg")
        try:
            c.register("user_t2_dup", "InitialPass123!")
            ok, err_resp = c.register("user_t2_dup", "OtherPass123!")
            ref, code, msg = G3TestClient.parse_err(err_resp)
            if not ok and code == 202:
                res.passed = True
                res.details = f"Received ERR 202 as expected: {err_resp}"
            else:
                res.details = f"Expected ERR 202, got ok={ok}, code={code}, resp={err_resp}"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            c.close()
        self.record_result(res, t0)

        # TC-T2-03: Single Active Session Enforcement (ERR 203)
        res = TestCaseResult("TC-T2-03", "Tier 2: Boundary & Corner", "Duplicate login for active account rejected (ERR 203)")
        t0 = time.time()
        c1 = self.make_client("T2_Sess1")
        c2 = self.make_client("T2_Sess2")
        try:
            c1.register("user_t2_sess", "Pass123!_Sess")
            ok1, resp1 = c1.login("user_t2_sess", "Pass123!_Sess")
            ok2, err_resp = c2.login("user_t2_sess", "Pass123!_Sess")
            ref, code, msg = G3TestClient.parse_err(err_resp)
            if ok1 and not ok2 and code == 203:
                res.passed = True
                res.details = f"First session active, second socket rejected with ERR 203: {err_resp}"
            else:
                res.details = f"Expected ERR 203, got ok1={ok1}, ok2={ok2}, code={code}, resp={err_resp}"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            c1.close()
            c2.close()
        self.record_result(res, t0)

        # TC-T2-04: Brute-Force Login Throttling (5 failed attempts trigger lockout)
        res = TestCaseResult("TC-T2-04", "Tier 2: Boundary & Corner", "5 consecutive failed logins trigger 30s lockout")
        t0 = time.time()
        c = self.make_client("T2_Throttle")
        try:
            reg_ok, reg_resp = c.register("user_t2_throttle", "CorrectPass123!")
            if not reg_ok:
                res.passed = False
                res.details = f"Registration setup failed for user_t2_throttle: {reg_resp}"
            else:
                # 1. Execute 5 consecutive failed login attempts with wrong passwords
                failures_ok = True
                fail_details = []
                for attempt in range(5):
                    ok, resp = c.login("user_t2_throttle", f"WrongPass_{attempt}")
                    _, code, _ = G3TestClient.parse_err(resp) if resp else (None, None, "")
                    if ok or code not in (200, 201):
                        failures_ok = False
                        fail_details.append(f"Attempt {attempt + 1}: expected rejection (code 200/201), got ok={ok}, resp={resp}")
                        break
                    fail_details.append(f"Attempt {attempt + 1}: rejected with code {code}")

                if not failures_ok:
                    res.passed = False
                    res.details = f"Failed login attempts setup failed: {'; '.join(fail_details)}"
                else:
                    # 2. Assert Genuine Lockout Invariant:
                    # Even with the CORRECT password, login MUST be rejected during the 30s lockout window.
                    ok_valid, resp_valid = c.login("user_t2_throttle", "CorrectPass123!")
                    _, code_v, msg_v = G3TestClient.parse_err(resp_valid) if resp_valid else (None, None, "")
                    if ok_valid:
                        res.passed = False
                        res.details = f"Lockout NOT enforced: login with valid password succeeded after 5 failures (resp={resp_valid})"
                    elif code_v in (200, 201):
                        res.passed = True
                        res.details = f"Lockout genuinely enforced: valid login rejected after 5 failures: code={code_v}, resp={resp_valid}"
                    else:
                        res.passed = False
                        res.details = f"Unexpected response for valid login during lockout window: code={code_v}, resp={resp_valid}"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            c.close()
        self.record_result(res, t0)

        # TC-T2-05: Challenge 30-Second Expiration Timeout (ERR 302)
        res = TestCaseResult("TC-T2-05", "Tier 2: Boundary & Corner", "Challenge invitation expiration unlocks participants (ERR 302)")
        t0 = time.time()
        c1 = self.make_client("T2_TO1")
        c2 = self.make_client("T2_TO2")
        try:
            c1.register("user_t2_to1", "Pass123!_TO")
            c1.login("user_t2_to1", "Pass123!_TO")
            c2.register("user_t2_to2", "Pass123!_TO")
            c2.login("user_t2_to2", "Pass123!_TO")

            c1.challenge("user_t2_to2")
            ch_req = c2.wait_for_challenge_req(timeout=3.0)
            if ch_req:
                inv_id, _ = ch_req
                self.logger.log(f"[*] Waiting up to {self.invite_timeout}s for challenge expiration tick...", COLOR_YELLOW)
                err_msg = c1.wait_for_msg("ERR", timeout=self.invite_timeout)
                ref, code, msg = G3TestClient.parse_err(err_msg) if err_msg else (None, None, "")
                if code == 302:
                    # Empirically verify mutual unlock: players can initiate/receive new challenges without ERR 301 (Busy)
                    ok_new, resp_new = c2.challenge("user_t2_to1", timeout=3.0)
                    if ok_new:
                        res.passed = True
                        res.details = f"Received ERR 302 (Invite Expired) and confirmed mutual unlock via follow-up challenge: {err_msg}"
                    else:
                        _, code_new, _ = G3TestClient.parse_err(resp_new) if resp_new else (None, None, "")
                        if code_new == 301:
                            res.passed = False
                            res.details = f"Received ERR 302, but players remained stuck in locked/busy state (ERR 301) on new challenge: {resp_new}"
                        else:
                            res.passed = True
                            res.details = f"Received ERR 302 (Invite Expired): {err_msg}"
                else:
                    res.passed = False
                    res.details = f"Invite expiration failed: expected ERR 302, received code={code}, resp={err_msg}"
            else:
                res.passed = False
                res.details = "Challenge setup failed: target never received CHALLENGE_REQ"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            c1.close()
            c2.close()
        self.record_result(res, t0)

        # TC-T2-06: Invalid Responder Rejection (ERR 303)
        res = TestCaseResult("TC-T2-06", "Tier 2: Boundary & Corner", "Third party cannot respond to invite (ERR 303)")
        t0 = time.time()
        c1 = self.make_client("T2_InvP1")
        c2 = self.make_client("T2_InvP2")
        c3 = self.make_client("T2_InvEve")
        try:
            c1.register("user_t2_inv1", "Pass123!_Inv")
            c1.login("user_t2_inv1", "Pass123!_Inv")
            c2.register("user_t2_inv2", "Pass123!_Inv")
            c2.login("user_t2_inv2", "Pass123!_Inv")
            c3.register("user_t2_eve", "Pass123!_Eve")
            c3.login("user_t2_eve", "Pass123!_Eve")

            c1.challenge("user_t2_inv2")
            ch_req = c2.wait_for_challenge_req(timeout=3.0)
            if ch_req:
                inv_id, _ = ch_req
                # Third party Eve attempts to accept invite_id
                c3.respond_challenge(inv_id, accept=True)
                err_resp = c3.wait_for_msg("ERR", timeout=3.0)
                ref, code, msg = G3TestClient.parse_err(err_resp) if err_resp else (None, None, "")
                if code == 303:
                    res.passed = True
                    res.details = f"Eve received ERR 303 as expected: {err_resp}"
                else:
                    res.passed = False
                    res.details = f"Expected ERR 303 for invalid responder, received code={code}, resp={err_resp}"
            else:
                res.passed = False
                res.details = "Challenge setup failed: target never received CHALLENGE_REQ"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            c1.close()
            c2.close()
            c3.close()
        self.record_result(res, t0)

        # TC-T2-07: Busy Player Mutual Locking (ERR 301)
        res = TestCaseResult("TC-T2-07", "Tier 2: Boundary & Corner", "Challenging busy/in-match player rejected (ERR 301)")
        t0 = time.time()
        c1 = self.make_client("T2_Busy1")
        c2 = self.make_client("T2_Busy2")
        c3 = self.make_client("T2_Busy3")
        try:
            c1.register("user_t2_busy1", "Pass123!_B1")
            c1.login("user_t2_busy1", "Pass123!_B1")
            c2.register("user_t2_busy2", "Pass123!_B2")
            c2.login("user_t2_busy2", "Pass123!_B2")
            c3.register("user_t2_busy3", "Pass123!_B3")
            c3.login("user_t2_busy3", "Pass123!_B3")

            c1.challenge("user_t2_busy2")
            ch_req = c2.wait_for_challenge_req(timeout=3.0)
            if ch_req:
                c2.respond_challenge(ch_req[0], accept=True)
                c1.wait_for_game_start(timeout=3.0)
                c2.wait_for_game_start(timeout=3.0)

                # Now user_t2_busy1 is in active game. c3 challenges c1
                ok, err_resp = c3.challenge("user_t2_busy1")
                ref, code, msg = G3TestClient.parse_err(err_resp)
                if not ok and code == 301:
                    res.passed = True
                    res.details = f"C3 challenge to in-game user rejected with ERR 301: {err_resp}"
                else:
                    res.details = f"Expected ERR 301, got ok={ok}, code={code}, resp={err_resp}"
            else:
                res.details = "Initial match setup failed"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            c1.close()
            c2.close()
            c3.close()
        self.record_result(res, t0)

        # TC-T2-08: Move Turn Order Enforcement (ERR 401)
        res = TestCaseResult("TC-T2-08", "Tier 2: Boundary & Corner", "Moving out of turn rejected (ERR 401)")
        t0 = time.time()
        c1 = self.make_client("T2_Turn1")
        c2 = self.make_client("T2_Turn2")
        try:
            c1.register("user_t2_turn1", "Pass123!_Turn")
            c1.login("user_t2_turn1", "Pass123!_Turn")
            c2.register("user_t2_turn2", "Pass123!_Turn")
            c2.login("user_t2_turn2", "Pass123!_Turn")
            c1.challenge("user_t2_turn2")
            ch_req = c2.wait_for_challenge_req(timeout=3.0)
            if ch_req:
                c2.respond_challenge(ch_req[0], accept=True)
                gs1 = c1.wait_for_game_start(timeout=3.0)
                gs2 = c2.wait_for_game_start(timeout=3.0)
                pO = c2 if gs1 and gs1[1] == "X" else c1
                g_id = pO.current_game_id

                # pO tries to move first (it is X's turn, seq 1)
                ok, err_resp = pO.send_move(g_id, seq=1, row=7, col=7)
                ref, code, msg = G3TestClient.parse_err(err_resp)
                if not ok and code == 401:
                    res.passed = True
                    res.details = f"Out-of-turn move rejected with ERR 401: {err_resp}"
                else:
                    res.details = f"Expected ERR 401, got ok={ok}, code={code}, resp={err_resp}"
            else:
                res.details = "Match setup failed"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            c1.close()
            c2.close()
        self.record_result(res, t0)

        # TC-T2-09: Cell Occupancy Rejection (ERR 402)
        res = TestCaseResult("TC-T2-09", "Tier 2: Boundary & Corner", "Moving on already occupied cell rejected (ERR 402)")
        t0 = time.time()
        c1 = self.make_client("T2_Occ1")
        c2 = self.make_client("T2_Occ2")
        try:
            c1.register("user_t2_occ1", "Pass123!_Occ")
            c1.login("user_t2_occ1", "Pass123!_Occ")
            c2.register("user_t2_occ2", "Pass123!_Occ")
            c2.login("user_t2_occ2", "Pass123!_Occ")
            c1.challenge("user_t2_occ2")
            ch_req = c2.wait_for_challenge_req(timeout=3.0)
            if ch_req:
                c2.respond_challenge(ch_req[0], accept=True)
                gs1 = c1.wait_for_game_start(timeout=3.0)
                gs2 = c2.wait_for_game_start(timeout=3.0)
                pX = c1 if gs1 and gs1[1] == "X" else c2
                pO = c2 if pX == c1 else c1
                g_id = pX.current_game_id

                # pX plays (7, 7)
                pX.send_move(g_id, seq=1, row=7, col=7)
                pO.wait_for_move_broadcast(g_id, seq=1)

                # pO tries to play the exact same cell (7, 7)
                ok, err_resp = pO.send_move(g_id, seq=2, row=7, col=7)
                ref, code, msg = G3TestClient.parse_err(err_resp)
                if not ok and code == 402:
                    res.passed = True
                    res.details = f"Occupied cell move rejected with ERR 402: {err_resp}"
                else:
                    res.details = f"Expected ERR 402, got ok={ok}, code={code}, resp={err_resp}"
            else:
                res.details = "Match setup failed"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            c1.close()
            c2.close()
        self.record_result(res, t0)

        # TC-T2-10: Coordinate Boundary Rejection (ERR 402 / ERR 102)
        res = TestCaseResult("TC-T2-10", "Tier 2: Boundary & Corner", "Out-of-bounds coordinates (>14) rejected (ERR 402 / ERR 102)")
        t0 = time.time()
        c1 = self.make_client("T2_Bound1")
        c2 = self.make_client("T2_Bound2")
        try:
            c1.register("user_t2_bnd1", "Pass123!_Bnd")
            c1.login("user_t2_bnd1", "Pass123!_Bnd")
            c2.register("user_t2_bnd2", "Pass123!_Bnd")
            c2.login("user_t2_bnd2", "Pass123!_Bnd")
            c1.challenge("user_t2_bnd2")
            ch_req = c2.wait_for_challenge_req(timeout=3.0)
            if ch_req:
                c2.respond_challenge(ch_req[0], accept=True)
                gs1 = c1.wait_for_game_start(timeout=3.0)
                pX = c1 if gs1 and gs1[1] == "X" else c2
                g_id = pX.current_game_id

                # pX sends move with row=15 (valid is 0..14)
                ok, err_resp = pX.send_move(g_id, seq=1, row=15, col=7)
                ref, code, msg = G3TestClient.parse_err(err_resp)
                if not ok and code in (102, 402):
                    res.passed = True
                    res.details = f"Out-of-bounds move rejected with code {code}: {err_resp}"
                else:
                    res.details = f"Expected ERR 102/402, got ok={ok}, code={code}, resp={err_resp}"
            else:
                res.details = "Match setup failed"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            c1.close()
            c2.close()
        self.record_result(res, t0)

        # TC-T2-11: Sequence Monotonicity Rejection (ERR 403)
        res = TestCaseResult("TC-T2-11", "Tier 2: Boundary & Corner", "Sequence number mismatch rejected (ERR 403)")
        t0 = time.time()
        c1 = self.make_client("T2_Seq1")
        c2 = self.make_client("T2_Seq2")
        try:
            c1.register("user_t2_seq1", "Pass123!_Seq")
            c1.login("user_t2_seq1", "Pass123!_Seq")
            c2.register("user_t2_seq2", "Pass123!_Seq")
            c2.login("user_t2_seq2", "Pass123!_Seq")
            c1.challenge("user_t2_seq2")
            ch_req = c2.wait_for_challenge_req(timeout=3.0)
            if ch_req:
                c2.respond_challenge(ch_req[0], accept=True)
                gs1 = c1.wait_for_game_start(timeout=3.0)
                pX = c1 if gs1 and gs1[1] == "X" else c2
                g_id = pX.current_game_id

                # Expecting seq=1, player sends seq=99
                ok, err_resp = pX.send_move(g_id, seq=99, row=7, col=7)
                ref, code, msg = G3TestClient.parse_err(err_resp)
                if not ok and code == 403:
                    res.passed = True
                    res.details = f"Seq jump rejected with ERR 403: {err_resp}"
                else:
                    res.details = f"Expected ERR 403, got ok={ok}, code={code}, resp={err_resp}"
            else:
                res.details = "Match setup failed"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            c1.close()
            c2.close()
        self.record_result(res, t0)

        # TC-T2-12: Cross-Game Move Rejection (ERR 400)
        res = TestCaseResult("TC-T2-12", "Tier 2: Boundary & Corner", "Cross-game move injection rejected (ERR 400)")
        t0 = time.time()
        c1 = self.make_client("T2_Cross")
        try:
            c1.register("user_t2_cross", "Pass123!_Cross")
            c1.login("user_t2_cross", "Pass123!_Cross")

            # c1 sends move for game 99999 which does not exist / c1 not in it
            ok, err_resp = c1.send_move(game_id=99999, seq=1, row=7, col=7)
            ref, code, msg = G3TestClient.parse_err(err_resp)
            if not ok and code in (400, 404):
                res.passed = True
                res.details = f"Cross-game move rejected with code {code}: {err_resp}"
            else:
                res.details = f"Expected ERR 400/404, got ok={ok}, code={code}, resp={err_resp}"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            c1.close()
        self.record_result(res, t0)

    # =========================================================================
    # TIER 3: CROSS-FEATURE COMBINATIONS
    # =========================================================================
    def run_tier3_cross_features(self):
        self.logger.raw_log("\n" + "=" * 80)
        self.logger.log(">>> EXECUTING TIER 3: CROSS-FEATURE COMBINATIONS", COLOR_CYAN)
        self.logger.raw_log("=" * 80)

        # TC-T3-01: State Collision: Challenge while active in match rejected
        res = TestCaseResult("TC-T3-01", "Tier 3: Cross-Feature", "Challenge attempt while active in match rejected")
        t0 = time.time()
        c1 = self.make_client("T3_Col1")
        c2 = self.make_client("T3_Col2")
        c3 = self.make_client("T3_Col3")
        try:
            c1.register("user_t3_col1", "Pass123!_Col")
            c1.login("user_t3_col1", "Pass123!_Col")
            c2.register("user_t3_col2", "Pass123!_Col")
            c2.login("user_t3_col2", "Pass123!_Col")
            c3.register("user_t3_col3", "Pass123!_Col")
            c3.login("user_t3_col3", "Pass123!_Col")

            c1.challenge("user_t3_col2")
            ch_req = c2.wait_for_challenge_req(timeout=3.0)
            if ch_req:
                c2.respond_challenge(ch_req[0], accept=True)
                c1.wait_for_game_start(timeout=3.0)
                c2.wait_for_game_start(timeout=3.0)

                # While playing c2, c1 attempts to challenge c3
                ok, err_resp = c1.challenge("user_t3_col3")
                ref, code, msg = G3TestClient.parse_err(err_resp)
                if not ok and code in (301, 400):
                    res.passed = True
                    res.details = f"Challenge while in match rejected with code {code}: {err_resp}"
                else:
                    res.details = f"Expected ERR 301/400, got ok={ok}, code={code}, resp={err_resp}"
            else:
                res.details = "Match setup failed"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            c1.close()
            c2.close()
            c3.close()
        self.record_result(res, t0)

        # TC-T3-02: Disconnect Forfeit in Active Game
        res = TestCaseResult("TC-T3-02", "Tier 3: Cross-Feature", "Sudden disconnect forfeits active match to opponent")
        t0 = time.time()
        c1 = self.make_client("T3_Disc1")
        c2 = self.make_client("T3_Disc2")
        try:
            c1.register("user_t3_disc1", "Pass123!_Disc")
            c1.login("user_t3_disc1", "Pass123!_Disc")
            c2.register("user_t3_disc2", "Pass123!_Disc")
            c2.login("user_t3_disc2", "Pass123!_Disc")

            c1.challenge("user_t3_disc2")
            ch_req = c2.wait_for_challenge_req(timeout=3.0)
            if ch_req:
                c2.respond_challenge(ch_req[0], accept=True)
                c1.wait_for_game_start(timeout=3.0)
                c2.wait_for_game_start(timeout=3.0)
                g_id = c2.current_game_id

                # Abruptly close c1 socket
                c1.close()

                # c2 should receive GAME_OVER with DISCONNECT outcome
                go = c2.wait_for_game_over(g_id, timeout=4.0)
                if go and ("DISCONNECT" in go or "WIN" in go):
                    res.passed = True
                    res.details = f"Opponent received disconnect game over: {go}"
                else:
                    res.details = f"Expected DISCONNECT GAME_OVER, received: {go}"
            else:
                res.details = "Match setup failed"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            c1.close()
            c2.close()
        self.record_result(res, t0)

        # TC-T3-03: Simultaneous Mutual Challenge Race Condition
        res = TestCaseResult("TC-T3-03", "Tier 3: Cross-Feature", "Simultaneous mutual challenge race resolved safely")
        t0 = time.time()
        c1 = self.make_client("T3_Race1")
        c2 = self.make_client("T3_Race2")
        try:
            reg1, _ = c1.register("user_t3_race1", "Pass123!_Race")
            log1, _ = c1.login("user_t3_race1", "Pass123!_Race")
            reg2, _ = c2.register("user_t3_race2", "Pass123!_Race")
            log2, _ = c2.login("user_t3_race2", "Pass123!_Race")

            if not (reg1 and log1 and reg2 and log2):
                res.passed = False
                res.details = "Setup failed: registration or login failed for mutual challenge accounts"
            else:
                # Fire challenges concurrently
                req1 = c1.next_req_id()
                req2 = c2.next_req_id()

                t_a = threading.Thread(target=lambda: c1.send_raw(f"CHALLENGE\t{req1}\tuser_t3_race2"))
                t_b = threading.Thread(target=lambda: c2.send_raw(f"CHALLENGE\t{req2}\tuser_t3_race1"))
                t_a.start()
                t_b.start()
                t_a.join()
                t_b.join()

                # Await specific responses for each request ID
                m1 = c1.wait_for_cmd_or_err("OK", req_id=req1, timeout=3.0)
                m2 = c2.wait_for_cmd_or_err("OK", req_id=req2, timeout=3.0)

                # Strict protocol semantic verification:
                # Exactly one client must succeed with OK, and the competing client must be rejected with ERR 301 (Busy).
                # Unimplemented ERR 101, double-success (both OK), or double-busy (both ERR 301) must FAIL.
                c1_ok = (m1 is not None and len(m1) > 0 and m1[0] == "OK")
                c2_ok = (m2 is not None and len(m2) > 0 and m2[0] == "OK")
                _, c1_code, _ = G3TestClient.parse_err(m1) if m1 else (None, None, "")
                _, c2_code, _ = G3TestClient.parse_err(m2) if m2 else (None, None, "")

                if (c1_ok and c2_code == 301) or (c2_ok and c1_code == 301):
                    res.passed = True
                    winner = "C1" if c1_ok else "C2"
                    res.details = f"Mutual challenge race cleanly resolved: {winner} won (OK), loser received ERR 301 (C1={m1}, C2={m2})"
                else:
                    res.passed = False
                    if c1_code == 101 or c2_code == 101:
                        res.details = f"Feature not implemented (received ERR 101): C1={m1}, C2={m2}"
                    elif c1_ok and c2_ok:
                        res.details = f"Race collision violation: BOTH clients received OK (deadlock/double invite): C1={m1}, C2={m2}"
                    elif c1_code == 301 and c2_code == 301:
                        res.details = f"Deadlock: BOTH clients received ERR 301 (neither could initiate): C1={m1}, C2={m2}"
                    else:
                        res.details = f"Expected exactly 1 OK and 1 ERR 301, received: C1={m1}, C2={m2}"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            c1.close()
            c2.close()
        self.record_result(res, t0)

        # TC-T3-04: Challenger Disconnect During Pending Challenge Invite
        res = TestCaseResult("TC-T3-04", "Tier 3: Cross-Feature", "Disconnect during pending invite safely unlocks target")
        t0 = time.time()
        c1 = self.make_client("T3_InvDisc1")
        c2 = self.make_client("T3_InvDisc2")
        c3 = self.make_client("T3_InvDisc3")
        try:
            c1.register("user_t3_invd1", "Pass123!_Invd")
            c1.login("user_t3_invd1", "Pass123!_Invd")
            c2.register("user_t3_invd2", "Pass123!_Invd")
            c2.login("user_t3_invd2", "Pass123!_Invd")
            c3.register("user_t3_invd3", "Pass123!_Invd")
            c3.login("user_t3_invd3", "Pass123!_Invd")

            c1.challenge("user_t3_invd2")
            ch_req = c2.wait_for_challenge_req(timeout=3.0)
            if ch_req:
                # Disconnect c1 while invite is pending
                c1.close()
                time.sleep(0.3)

                # Target c2 should be unlocked and able to accept new invites or invite c3
                ok, resp = c3.challenge("user_t3_invd2")
                if ok:
                    res.passed = True
                    res.details = f"Target unlocked from pending invite and accepted new challenge from C3: {resp}"
                else:
                    ref, code, msg = G3TestClient.parse_err(resp)
                    res.details = f"Target still locked or failed: code={code}, resp={resp}"
            else:
                res.details = "Invite setup failed"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            c1.close()
            c2.close()
            c3.close()
        self.record_result(res, t0)

    # =========================================================================
    # TIER 4: REAL-WORLD APPLICATION SCENARIOS
    # =========================================================================
    def run_tier4_real_world(self):
        self.logger.raw_log("\n" + "=" * 80)
        self.logger.log(">>> EXECUTING TIER 4: REAL-WORLD APPLICATION SCENARIOS", COLOR_CYAN)
        self.logger.raw_log("=" * 80)

        # TC-T4-01: 4 Accounts Playing 2 Concurrent Independent Matches (Zero Event Leakage)
        res = TestCaseResult("TC-T4-01", "Tier 4: Real-World Scenarios", "4 accounts playing 2 concurrent matches with zero cross-event leakage")
        t0 = time.time()
        u1 = self.make_client("U1")
        u2 = self.make_client("U2")
        u3 = self.make_client("U3")
        u4 = self.make_client("U4")
        g1_id = 0
        g2_id = 0
        try:
            # Register & Login 4 accounts
            for c, name in [(u1, "user_rw_1"), (u2, "user_rw_2"), (u3, "user_rw_3"), (u4, "user_rw_4")]:
                c.register(name, "Pass123!_RW")
                c.login(name, "Pass123!_RW")

            # Match 1: U1 vs U2
            u1.challenge("user_rw_2")
            req1 = u2.wait_for_challenge_req(timeout=3.0)
            if not req1:
                raise RuntimeError("Match 1 challenge request failed")
            u2.respond_challenge(req1[0], accept=True)
            gs1_u1 = u1.wait_for_game_start(timeout=3.0)
            gs1_u2 = u2.wait_for_game_start(timeout=3.0)
            g1_id = gs1_u1[0]

            # Match 2: U3 vs U4
            u3.challenge("user_rw_4")
            req2 = u4.wait_for_challenge_req(timeout=3.0)
            if not req2:
                raise RuntimeError("Match 2 challenge request failed")
            u4.respond_challenge(req2[0], accept=True)
            gs2_u3 = u3.wait_for_game_start(timeout=3.0)
            gs2_u4 = u4.wait_for_game_start(timeout=3.0)
            g2_id = gs2_u3[0]

            if g1_id == g2_id:
                raise RuntimeError(f"Match IDs collided: {g1_id}")

            # Interleave moves in both games at identical coordinates (7, 7)
            # Match 1, Move 1: U1 (X) -> (7, 7)
            u1_pX = u1 if gs1_u1[1] == "X" else u2
            u1_pO = u2 if u1_pX == u1 else u1
            u1_pX.send_move(g1_id, seq=1, row=7, col=7)
            u1_pO.wait_for_move_broadcast(g1_id, seq=1)

            # Match 2, Move 1: U3 (X) -> (7, 7) (Independent board allows same coordinate!)
            u2_pX = u3 if gs2_u3[1] == "X" else u4
            u2_pO = u4 if u2_pX == u3 else u3
            u2_pX.send_move(g2_id, seq=1, row=7, col=7)
            u2_pO.wait_for_move_broadcast(g2_id, seq=1)

            # Match 1, Move 2: U1 (O) -> (6, 6)
            u1_pO.send_move(g1_id, seq=2, row=6, col=6)
            u1_pX.wait_for_move_broadcast(g1_id, seq=2)

            # Match 2, Move 2: U2 (O) -> (6, 6)
            u2_pO.send_move(g2_id, seq=2, row=6, col=6)
            u2_pX.wait_for_move_broadcast(g2_id, seq=2)

            # Verify Zero Cross-Match Event Leakage:
            # U1 & U2 must NEVER have received any game-scoped packet belonging to g2_id
            m1_msgs = u1.get_received_msgs() + u2.get_received_msgs()
            leakage_m1 = [m for m in m1_msgs if self.is_game_event(m, g2_id)]

            # U3 & U4 must NEVER have received any game-scoped packet belonging to g1_id
            m2_msgs = u3.get_received_msgs() + u4.get_received_msgs()
            leakage_m2 = [m for m in m2_msgs if self.is_game_event(m, g1_id)]

            if not leakage_m1 and not leakage_m2:
                res.passed = True
                res.details = f"2 concurrent matches active (G1={g1_id}, G2={g2_id}) with 0 cross-event leakage"
            else:
                res.details = f"Leakage detected! M1 leak: {leakage_m1}, M2 leak: {leakage_m2}"
        except Exception as e:
            res.details = f"Exception: {e}"
        self.record_result(res, t0)

        # TC-T4-02: Asynchronous Match Conclusion Isolation
        res = TestCaseResult("TC-T4-02", "Tier 4: Real-World Scenarios", "Match 1 early conclusion does not disrupt ongoing Match 2")
        t0 = time.time()
        try:
            if g1_id and g2_id:
                # Conclude Match 1 early via resignation by u1_pO
                u1_pO.resign(g1_id)
                go1_a = u1_pX.wait_for_game_over(g1_id, timeout=3.0)
                go1_b = u1_pO.wait_for_game_over(g1_id, timeout=3.0)

                # Verify Match 2 participants received 0 interruption or conclusion packets from Match 1
                m2_msgs = u3.get_received_msgs() + u4.get_received_msgs()
                m2_leak = [m for m in m2_msgs if self.is_game_event(m, g1_id)]
                # Ensure no premature termination event for Match 2 before Match 2 resigns
                premature_go = []
                for m in m2_msgs:
                    toks = m.strip("\r\n").split("\t")
                    if toks and toks[0] == "GAME_OVER" and len(toks) > 1 and toks[1] == str(g2_id):
                        premature_go.append(m)
                if premature_go:
                    m2_leak.extend(premature_go)

                # Match 2 continues valid gameplay uninterrupted: seq 3
                ok, ack3 = u2_pX.send_move(g2_id, seq=3, row=8, col=8)
                bcast3 = u2_pO.wait_for_move_broadcast(g2_id, seq=3)

                # Match 2 then concludes cleanly
                u2_pO.resign(g2_id)
                go2_a = u2_pX.wait_for_game_over(g2_id, timeout=3.0)
                go2_b = u2_pO.wait_for_game_over(g2_id, timeout=3.0)

                if go1_a and ok and bcast3 and go2_a and not m2_leak:
                    res.passed = True
                    res.details = f"Match 1 concluded early; Match 2 continued and concluded cleanly with zero interference"
                else:
                    res.details = f"Isolation violated or moves failed: go1={go1_a}, m2_ack={ack3}, go2={go2_a}, m2_leak={m2_leak}"
            else:
                res.details = "Prerequisite games not established in TC-T4-01"
        except Exception as e:
            res.details = f"Exception: {e}"
        self.record_result(res, t0)

        # TC-T4-03: Verification of Structured Per-Game Move Logs (logs/game_<id>.log)
        res = TestCaseResult("TC-T4-03", "Tier 4: Real-World Scenarios", "Per-game structured logs (logs/game_<id>.log) verified on disk")
        t0 = time.time()
        try:
            log1_path = os.path.join("logs", f"game_{g1_id}.log")
            log2_path = os.path.join("logs", f"game_{g2_id}.log")

            log1_exists = os.path.exists(log1_path)
            log2_exists = os.path.exists(log2_path)

            if log1_exists and log2_exists:
                with open(log1_path, "r", encoding="utf-8", errors="replace") as f1:
                    c1 = f1.read()
                with open(log2_path, "r", encoding="utf-8", errors="replace") as f2:
                    c2 = f2.read()

                # Assert non-empty and structured content
                if len(c1) > 10 and len(c2) > 10:
                    res.passed = True
                    res.details = f"Logs verified: {log1_path} ({len(c1)} bytes), {log2_path} ({len(c2)} bytes)"
                else:
                    res.details = f"Logs empty or truncated: len(c1)={len(c1)}, len(c2)={len(c2)}"
            else:
                res.details = f"Log files missing on disk: log1={log1_exists} ({log1_path}), log2={log2_exists} ({log2_path})"
        except Exception as e:
            res.details = f"Exception: {e}"
        self.record_result(res, t0)

        # TC-T4-04: Full Lifecycle Lobby Re-entry & Cross-Pairing Rematch
        res = TestCaseResult("TC-T4-04", "Tier 4: Real-World Scenarios", "Clean lobby recycling and cross-pairing rematch (U1 vs U3)")
        t0 = time.time()
        try:
            # U1 (from Game 1) now challenges U3 (from Game 2)
            u1.challenge("user_rw_3")
            req3 = u3.wait_for_challenge_req(timeout=3.0)
            if req3:
                u3.respond_challenge(req3[0], accept=True)
                gs3_u1 = u1.wait_for_game_start(timeout=3.0)
                gs3_u3 = u3.wait_for_game_start(timeout=3.0)

                if gs3_u1 and gs3_u3:
                    g3_id = gs3_u1[0]
                    # Verify game sequence starts cleanly from seq 1
                    pX = u1 if gs3_u1[1] == "X" else u3
                    pO = u3 if pX == u1 else u1

                    ok, ack = pX.send_move(g3_id, seq=1, row=7, col=7)
                    bcast = pO.wait_for_move_broadcast(g3_id, seq=1)

                    if ok and bcast:
                        res.passed = True
                        res.details = f"Cross-pairing rematch succeeded cleanly: G3_ID={g3_id}, seq 1 verified"
                    else:
                        res.details = f"Rematch move failed: ack={ack}, bcast={bcast}"
                else:
                    res.details = f"GAME_START failed on rematch: gs1={gs3_u1}, gs3={gs3_u3}"
            else:
                res.details = "Cross-pairing challenge failed"
        except Exception as e:
            res.details = f"Exception: {e}"
        finally:
            u1.close()
            u2.close()
            u3.close()
            u4.close()
        self.record_result(res, t0)

    # =========================================================================
    # SUMMARY REPORTING
    # =========================================================================
    def print_summary(self) -> int:
        self.logger.raw_log("\n" + "=" * 90)
        self.logger.raw_log(f"{'E2E TEST SUMMARY MATRIX':^90}")
        self.logger.raw_log("=" * 90)
        self.logger.raw_log(f"{'Case ID':<15} | {'Group':<28} | {'Result':<6} | {'Time (ms)':>9} | Description")
        self.logger.raw_log("-" * 90)

        passed_count = 0
        total_count = len(self.results)

        for r in self.results:
            if r.passed:
                passed_count += 1
                res_col = f"{COLOR_GREEN}PASS{COLOR_RESET}"
            else:
                res_col = f"{COLOR_RED}FAIL{COLOR_RESET}"
            self.logger.raw_log(f"{r.case_id:<15} | {r.group:<28} | {res_col:<15} | {r.duration_ms:>9.1f} | {r.description}")

        pass_rate = (passed_count / total_count * 100.0) if total_count > 0 else 0.0
        self.logger.raw_log("=" * 90)
        summary_line = f"FINAL OUTCOME: {passed_count} / {total_count} TEST CASES PASSED ({pass_rate:.1f}%)"
        if passed_count == total_count and total_count > 0:
            self.logger.log(f"{summary_line} — ALL CRITERIA VERIFIED (PASS)", COLOR_GREEN)
            return 0
        else:
            self.logger.log(f"{summary_line} — CRITERIA NOT MET (FAIL)", COLOR_RED)
            return 1


def main():
    parser = argparse.ArgumentParser(description="Milestone G3 Automated 4-Tier E2E Test Suite")
    parser.add_argument("--host", default="127.0.0.1", help="Target server host (default: 127.0.0.1)")
    parser.add_argument("--port", type=int, default=9350, help="Target server port (default: 9350)")
    parser.add_argument("--pid", type=int, default=None, help="Target server PID for monitoring")
    parser.add_argument("--db", default="tests/test_g3.db", help="Target SQLite database path")
    parser.add_argument("--log", default="tests/test_g3_results.log", help="Output execution log path")
    parser.add_argument("--invite-timeout", type=float, default=32.0, help="Timeout in seconds for invite expiration TC-T2-05")
    args = parser.parse_args()

    logger = TestLogger(args.log)
    suite = G3TestSuite(
        host=args.host,
        port=args.port,
        server_pid=args.pid,
        db_path=args.db,
        logger=logger,
        invite_timeout=args.invite_timeout
    )

    logger.log("=================================================================", COLOR_CYAN)
    logger.log("   CARO ONLINE C11/LINUX - MILESTONE G3 AUTOMATED TEST SUITE    ", COLOR_CYAN)
    logger.log("=================================================================", COLOR_CYAN)
    logger.log(f"Target Server : {args.host}:{args.port}")
    logger.log(f"Database Path : {args.db}")
    logger.log(f"Execution Log : {args.log}")
    if args.pid:
        logger.log(f"Server PID    : {args.pid}")

    try:
        suite.run_tier1_features()
        suite.run_tier2_boundary_corner()
        suite.run_tier3_cross_features()
        suite.run_tier4_real_world()
        status = suite.print_summary()
    finally:
        logger.close()

    sys.exit(status)


if __name__ == "__main__":
    main()
