# KIẾN TRÚC HỆ THỐNG CỜ CARO TRỰC TUYẾN

**Dự án:** Cờ Caro trực tuyến (C/Linux - Non-blocking Socket & Poll)  
**Tác giả:** Trịnh Ngọc Hoàng Nam (20235793) & Đào Nhật Hưng (20235737)  
**Mốc tài liệu:** Tuần 1 — Khung kiến trúc và phân rã module  
**Phiên bản:** 1.0  

---

## 1. TỔNG QUAN KIẾN TRÚC HỆ THỐNG

Hệ thống hoạt động theo mô hình **Client-Server tập trung** (Centralized Client-Server Architecture). Toàn bộ trạng thái ván cờ, dữ liệu phiên người dùng và kết toán điểm được quản lý tập trung và xác thực trên Server nhằm ngăn chặn gian lận (cheat-proof).

```
                      +------------------------------------------+
                      |               CARO SERVER                |
                      |                                          |
                      |  +------------------------------------+  |
                      |  |     Main Thread: Non-blocking      |  |
                      |  |       Event Loop via poll()        |  |
                      |  +------------------+-----------------+  |
                      |                     |                    |
                      |      +--------------+--------------+     |
                      |      |                             |     |
                      |      v                             v     |
                      |  +-------+                  +---------+  |
                      |  |Session|                  |  Game   |  |
                      |  |Manager|                  | Manager |  |
                      |  +-------+                  +----+----+  |
                      |                                  |       |
                      |                                  v       |
                      |                             +---------+  |
                      |                             |  Rules  |  |
                      |                             | Engine  |  |
                      |                             +---------+  |
                      |                     |                    |
                      |       (Async Jobs)  v                    |
                      |  +------------------------------------+  |
                      |  | Worker Thread Pool / Job Queue     |  |
                      |  | (Password Hashing, SQLite Trans)   |  |
                      |  +------------------------------------+  |
                      +------------------^-----------------------+
                                         |
                                         | TCP Stream (Binary Framing)
                                         | [4B length][TAB payload\n]
                                         |
              +--------------------------+--------------------------+
              |                                                     |
              v                                                     v
+-----------------------------+                       +-----------------------------+
|        CLIENT CLI A         |                       |        CLIENT CLI B         |
|  +-----------------------+  |                       |  +-----------------------+  |
|  | poll() [stdin + sock] |  |                       |  | poll() [stdin + sock] |  |
|  +-----------------------+  |                       |  +-----------------------+  |
|  | ANSI Board & Command  |  |                       |  | ANSI Board & Command  |  |
|  | State Machine Engine  |  |                       |  | State Machine Engine  |  |
+-----------------------------+                       +-----------------------------+
```

### 1.1. Các nguyên tắc kiến trúc cốt lõi
1. **Single-threaded Event Loop (Luồng chính đơn phục vụ I/O mạng):**
   - Không tạo tiến trình (process) hoặc luồng (thread) riêng cho từng client hoặc từng ván cờ.
   - Toàn bộ kết nối TCP (listener và client sockets) được đặt ở chế độ không chặn (`O_NONBLOCK`) và theo dõi dồn kênh tập trung thông qua lời gọi hàm hệ thống `poll()`.
2. **Worker bất đồng bộ cho tác vụ chậm (Async Job Workers):**
   - Các thao tác tốn CPU (băm mật khẩu bằng thư viện an toàn) hoặc I/O ổ đĩa (giao dịch đọc/ghi SQLite) được đưa vào hàng đợi công việc (bounded job queue) cho worker xử lý ở chế độ nền.
   - Worker hoàn thành công việc và trả completion message kèm định danh `session_id` / `game_id` về cho event loop qua pipe hoặc cơ chế thông báo nội bộ.
   - **Chỉ có luồng chính (event loop)** mới được quyền trực tiếp cập nhật trạng thái của `Client/Session` và `Game`. Worker tuyệt đối không truy cập hoặc sửa trực tiếp cấu trúc RAM của phiên.
3. **Phân tách hoàn toàn Giao thức với Vận chuyển mạng:**
   - Tuyệt đối không gửi struct C thô (raw struct) qua TCP socket để tránh sai khác padding, endianness giữa các kiến trúc máy.
   - Giao thức dùng khung nhị phân chuẩn hóa: Header 4-byte độ dài theo thứ tự mạng (Network Byte Order - Big Endian), payload dạng văn bản phân tách bằng dấu TAB (`\t`) kết thúc bằng dòng mới (`\n`).
   - Cả Server và Client dùng chung một module xử lý giao thức (`src/common/protocol.c`). Không sao chép hai phiên bản parser khác nhau.
4. **Không giả định ranh giới gói tin trên TCP:**
   - TCP là giao thức hướng luồng (byte-stream), không bảo toàn ranh giới thông điệp. Một lần `recv()` có thể trả về một phần khung (partial frame) hoặc nhiều khung dính liền nhau (coalesced frames). Một lần `send()` có thể chỉ gửi được một phần dữ liệu (partial send).
   - Mỗi client duy trì bộ đệm nhận (RX buffer) và bộ đệm gửi (TX buffer) tích lũy độc lập.

---

## 2. PHÂN RÃ MODULE HỆ THỐNG (M01 - M12)

Dự án được phân chia thành 12 module rõ ràng với trách nhiệm đơn lẻ và phân công minh bạch giữa hai thành viên:

| Mã Module | Tên Module | Người chủ trì | File / Sản phẩm bàn giao | Trách nhiệm chính |
|---|---|---|---|---|
| **M01** | Repository & Build | Hoàng Nam | `Makefile`, `.gitignore`, cấu trúc repo | Quản lý biên dịch, cờ build `-Wall -Wextra`, target build server, client, test, clean |
| **M02** | Protocol & Parser | Nhật Hưng | `include/protocol.h`, `src/common/protocol.c`, `docs/protocol.md` | Định dạng frame nhị phân, đóng gói frame, bóc tách partial/multiple frames, parse token TAB |
| **M03** | Socket & Event Loop | Hoàng Nam | `src/server/net.c`, `src/server/event_loop.c` | Tạo non-blocking socket, lắng nghe `poll()`, tiếp nhận kết nối, quản lý RX/TX buffer, ngắt kết nối an toàn |
| **M04** | Session & Presence | Hoàng Nam | `src/server/session.c`, `include/server.h` | Vòng đời kết nối client, trạng thái phiên (Chưa đăng nhập, Sảnh chờ, Đang mời, Đang chơi), dọn dẹp tài nguyên |
| **M05** | Rules Engine | Nhật Hưng | `src/common/rules.c` (hoặc `src/server/rules.c`), `include/game.h` | Kiểm tra nước đi hợp lệ bàn 15×15, điều kiện thắng 4 hướng (>=5 quân), hòa; hoàn toàn độc lập với mạng/DB |
| **M06** | Challenge & Game Manager | Nhật Hưng | `src/server/challenge.c`, `src/server/game_manager.c` | Quản lý lời mời (timeout 30s), ghép trận, cấp phát ván cờ, điều phối thứ tự nước đi `expected_seq` |
| **M07** | Client CLI | Nhật Hưng | `src/client/main.c`, `src/client/ui.c` | CLI dồn kênh `stdin` và `socket`, vẽ bàn cờ ANSI, nhận lệnh người dùng, hiển thị thông báo server |
| **M08** | Authentication | Hoàng Nam | `src/server/auth.c` | Đăng ký, đăng nhập, băm mật khẩu có salt, chống đăng nhập trùng lặp trên 1 tài khoản |
| **M09** | SQLite Storage | Hoàng Nam | `src/server/storage.c`, `schema.sql`, DB worker | Tạo bảng `users`, `games`, `moves`; giao dịch an toàn; phục hồi trạng thái sau crash |
| **M10** | Leaderboard & Logs | Nhật Hưng | `src/server/stats.c`, `src/client/log_viewer.c` | Tính điểm (Thắng +3, Hòa +1, Thua 0), phân trang xếp hạng, chia khung nhật ký ván đấu |
| **M11** | Testing & Tooling | Nhật Hưng | `tests/`, bot tải 20 client, fault injection scripts | Kịch bản kiểm thử tải, kiểm tra rớt mạng giữa chừng, kiểm thử biên framing, thu thập log nghiệm thu |
| **M12** | Documentation & Defense | Cả hai (Nam kỹ thuật, Hưng sử dụng/slide) | `docs/`, `README.md`, Slide báo cáo | Báo cáo kiến trúc, tài liệu đặc tả, hướng dẫn cài đặt và sử dụng, kịch bản demo |

---

## 3. RANH GIỚI VÀ QUYỀN SỞ HỮU DỮ LIỆU (DATA OWNERSHIP)

### 3.1. Ranh giới dữ liệu giữa Nam và Hưng
- **Nam phụ trách:**
  - Socket mạng, vòng lặp `poll()`, file descriptor lifecycle.
  - Cấu trúc `Session` và bộ đệm nhị phân cấp thấp `rx_buf`, `tx_buf`.
  - Toàn quyền đọc/ghi cơ sở dữ liệu SQLite qua API nội bộ (`storage_save_game`, `storage_update_score`).
- **Hưng phụ trách:**
  - Định nghĩa cấu trúc thông điệp tầng ứng dụng (`protocol_msg_t`).
  - Lõi luật chơi (`CaroBoard`, `rules_check_win`, `rules_is_valid_move`).
  - Logic điều phối ván cờ (`GameSession`, xử lý lệnh `MOVE`, kiểm tra lượt và `expected_seq`).
  - Hưng gọi API lưu trữ do Nam cung cấp khi ván cờ kết thúc, không tự ý can thiệp trực tiếp vào database handle.

### 3.2. Cấu trúc vòng đời dữ liệu phiên (Session Lifecycle)
```
[TCP CONNECT]
      |
      v
(STATE_CONNECTED / UNREGISTERED)
      |
      +---> [LOGIN / REGISTER via M08 Auth]
      |
      v
(STATE_LOBBY_AVAILABLE) <------------------------------------+
      |                                                      |
      +---> [Gửi / Nhận Lời mời thách đấu]                  |
      |                                                      |
      v                                                      |
(STATE_INVITING / STATE_INVITED)                             |
      |                                                      |
      +---> [Từ chối / Hết hạn 30s] -------------------------+
      |
      +---> [Chấp nhận: GAME_START]
      |
      v
(STATE_IN_GAME)
      |
      +---> [Diễn biến nước đi: MOVE luân phiên]
      |
      +---> [Kết thúc: Thắng / Thua / Hòa / Đầu hàng / Disconnect]
      |
      v
(STATE_GAME_OVER_PENDING_COMMIT)
      |
      +---> [SQLite DB Worker hoàn thành COMMIT]
      |
      v
(STATE_LOBBY_AVAILABLE)
      |
      +---> [DISCONNECT / EOF]
      |
      v
(STATE_CLOSED / CLEANUP)
```

---

## 4. LUỒNG DỮ LIỆU ĐIỂN HÌNH (DATA FLOW)

### 4.1. Luồng tiếp nhận và xử lý Frame trên Server
```
1. [Network Stream] -> recv(fd, rx_buf + rx_len, available_space)
2. rx_len += bytes_read
3. Lặp:
     a. Kiểm tra nếu rx_len < 4 bytes -> DỪNG (chờ POLLIN tiếp tục).
     b. Đọc uint32_t payload_len từ 4 bytes đầu (ntohl).
     c. Kiểm tra nếu payload_len > MAX_PAYLOAD_SIZE -> Đóng kết nối (bảo vệ chống flood/tấn công cạn kiệt bộ nhớ).
     d. Kiểm tra nếu rx_len < 4 + payload_len -> DỪNG (chờ POLLIN tiếp tục nhận đủ khung).
     e. Đã đủ 1 khung hoàn chỉnh:
        - Trích xuất payload (độ dài payload_len).
        - memmove dịch chuyển dữ liệu dư trong rx_buf về đầu buffer, cập nhật rx_len.
        - Gọi parser giao thức: protocol_parse_payload(payload, &msg).
        - Chuyển giao msg cho bộ định tuyến lệnh (Command Dispatcher).
```

### 4.2. Luồng gửi dữ liệu (TX Pipeline)
```
1. Module nghiệp vụ muốn gửi thông điệp msg tới client.
2. Gọi hàm protocol_pack_frame(&msg, out_buf, &out_len).
3. Đưa out_buf vào đuôi tx_buf của client.
4. Thử gọi send(fd, tx_buf, tx_len, MSG_NOSIGNAL).
5. Nếu gửi được n bytes (n < tx_len):
     - Dịch chuyển tx_buf (tx_len -= n).
     - BẬT cờ POLLOUT cho socket trong pollfd.
6. Nếu n == tx_len:
     - Gửi xong trọn vẹn, TẮT cờ POLLOUT.
7. Khi event loop báo sự kiện POLLOUT: tiếp tục gửi phần còn lại trong tx_buf.
```

---

## 5. RÀ SOÁT TÀI NGUYÊN VÀ CÔ LẬP SỰ CỐ

1. **Bộ đệm cố định có kiểm soát:**
   - Mỗi kết nối client được cấp phát tĩnh một bộ đệm RX và TX tối đa (ví dụ 8KB - 16KB).
   - Nếu client gửi liên tục mà không tuân thủ framing hoặc vượt quá giới hạn khung, Server đóng ngay kết nối để tránh cạn kiệt bộ nhớ RAM.
2. **Xử lý tín hiệu SIGPIPE:**
   - Toàn bộ các thao tác `send()` sử dụng cờ `MSG_NOSIGNAL` (hoặc chặn `SIGPIPE` bằng `signal(SIGPIPE, SIG_IGN)` khi khởi động Server) để tránh việc Server bị hệ điều hành tắt đột ngột khi gửi vào socket đối phương đã đóng.
3. **Cô lập lỗi ván đấu:**
   - Một client bị crash hoặc gửi lệnh sai không làm ảnh hưởng đến ván cờ của các cặp đấu khác.
   - Trận đấu bị hủy hoặc kết thúc bất thường sẽ giải phóng slot ván cờ và trả đối thủ về sảnh chờ an toàn.
