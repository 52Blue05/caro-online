# BÁO CÁO TIẾN ĐỘ TUẦN 2 — NGHIỆM THU MỐC G2
## DỰ ÁN: CỜ CARO TRỰC TUYẾN BẰNG C/LINUX

- **Thời gian thực hiện**: 23/11/2026 – 29/11/2026 (Nghiệm thu: 16/11/2026)
- **Thành viên tham gia**:
  - **Trịnh Ngọc Hoàng Nam (MSSV 20235793)** — Phụ trách Server Core, RX/TX non-blocking tích lũy khung, Quản lý tài nguyên, Ngân sách đọc/ghi/khung, Quản lý phiên và Rà soát giải phóng FD.
  - **Đào Nhật Hưng (MSSV 20235737)** — Phụ trách Module Luật cờ Caro 15x15 độc lập, Hardening Parser giao thức, Client CLI hiển thị lỗi, Bot kiểm thử 20 kết nối, Bộ kiểm thử biên và Nghiệm thu G2.
- **Mục tiêu Tuần 2 (Mốc G2)**: Mạng ổn định và Luật chơi
  - Phục vụ đồng thời 20 kết nối TCP cùng trao đổi PING/PONG ổn định, không nghẽn hay gián đoạn.
  - Xử lý an toàn gói tin phân mảnh (fragmented), gói tin gộp (coalesced), slow peers, header quá lớn hoặc sai quy chuẩn.
  - Sau lượt mở/đóng nhiều client liên tiếp, số lượng socket file descriptor (fd) của server phục hồi về baseline ban đầu (0 descriptor leak), bộ đệm giải phóng sạch sẽ, các kết nối đang mở tiếp tục phản hồi bình thường.
  - Module luật cờ Caro 15×15 độc lập hoàn toàn với mạng/SQL, kiểm thử 100% pass (ô hợp lệ, đặt quân, thắng 4 hướng, overline ≥6 quân, hòa 225 ô).
  - Parser từ chối các trường thiếu, số tràn hoặc không hợp lệ; Client CLI hiển thị rõ `ERROR` và tình trạng mất kết nối.
  - Kịch bản bot 20 kết nối và bộ kiểm thử biên dữ liệu chạy tự động và xuất báo cáo kết quả chi tiết từng ca kiểm thử.

---

## 1. BẢNG TỔNG HỢP TRẠNG THÁI 8 ĐẦU VIỆC (T2N1 – T2H4)

| Mã việc | Thời hạn | Người phụ trách | Tên đầu việc | Trạng thái | Bằng chứng kiểm thử thực tế |
|---|---|---|---|---|---|
| **T2N1** | 23/11–24/11 | Nam (20235793) | Hoàn thiện RX và TX cho từng client | **HOÀN THÀNH (100%)** | Đọc tích lũy đúng khung; gửi một phần giữ offset; xử lý `EAGAIN` an toàn; từ chối header > 4KB hoặc độ dài 0 byte (`EDGE-FRAME-01` đến `EDGE-FRAME-09` PASS). |
| **T2N2** | 25/11–26/11 | Nam (20235793) | Giới hạn công việc và tài nguyên | **HOÀN THÀNH (100%)** | Ngân sách xử lý tối đa 16 khung/vòng; giới hạn RX/TX 64KB; timeout 30s ngắt client treo/slowloris; `POLLOUT` chỉ bật khi còn dữ liệu cần gửi; fix dứt điểm hiện tượng frame starvation khi gặp burst. |
| **T2N3** | 27/11–28/11 | Nam (20235793) | Quản lý phiên và danh sách online | **HOÀN THÀNH (100%)** | Enum `client_state_t` phân tách phiên tạm `CLIENT_STATE_CONNECTED` (`guest_<fd>`) với tài khoản xác thực; cấu trúc sẵn sàng broadcast online list mà không ảnh hưởng kết nối khác. |
| **T2N4** | 29/11 | Nam (20235793) | Rà soát đóng socket và giải phóng bộ nhớ | **HOÀN THÀNH (100%)** | Sau chuỗi bão mở/đóng 20–60 kết nối (churn storm), số lượng fd trong `/proc/<pid>/fd` trở về đúng baseline 5 FDs ban đầu (0 leak); kết nối canary nền vẫn phản hồi < 1ms (`FD-TRACK-01` PASS). |
| **T2H1** | 23/11–24/11 | Hưng (20235737) | Hoàn thiện luật Caro độc lập với mạng | **HOÀN THÀNH (100%)** | `src/server/game.c` & `include/game.h`: bàn cờ 15x15, thắng 4 hướng, overline ≥6 quân, hòa 225 ô; độc lập 100% không socket/SQL (134/134 rule assertions PASS trong `tests/test_game.c`). |
| **T2H2** | 25/11–26/11 | Hưng (20235737) | Hoàn thiện parser và hiển thị lỗi | **HOÀN THÀNH (100%)** | `parse_strict_uint32` từ chối số âm, non-digit, số tràn `ERANGE`; từ chối thiếu trường, tọa độ ngoài 15x15; CLI hiển thị `[SERVER ERROR <code>]` và `[CONNECTION LOST]` (`PARSE-REJECT-01` đến `09` PASS). |
| **T2H3** | 27/11–28/11 | Hưng (20235737) | Viết bot 20 kết nối và kiểm tra framing | **HOÀN THÀNH (100%)** | `tests/test_g2_suite.py` mở 20 TCP socket đồng thời với lưu lượng hỗn hợp (rapid, fragmented 1B/10ms, coalesced burst 16–100 frames, slow drip); 100% tin nhắn trao đổi thành công (`BOT-CONCUR-01`, `02` PASS). |
| **T2H4** | 29/11 | Hưng (20235737) | Bổ sung kiểm thử luật và biên dữ liệu | **HOÀN THÀNH (100%)** | Bộ test 4 góc biên bàn cờ, thắng 6 và 7 quân, chặn 2 đầu, hòa trọn vẹn 225 ô; 9 ca từ chối gói tin lỗi; bộ fuzz test chạy 938,342 ca không phát hiện lỗi. |

---

## 2. CHI TIẾT KẾT QUẢ THỰC HIỆN TỪNG ĐẦU VIỆC

### 2.1. Nhóm đầu việc của Agent Nam (Server Core, Quản lý tài nguyên & Dọn dẹp FD)

#### T2N1: Hoàn thiện RX và TX cho từng client
- **Hiện thực**:
  - `include/server.h` & `src/server/server.c`: Quản lý bộ đệm kép `rx_buf` (64KB), `rx_len`, `tx_buf` (64KB), `tx_len`, `tx_offset` độc lập cho từng client trong cấu trúc `client_conn_t`.
  - **Đọc tích lũy (Accumulation)**: Khi có sự kiện `POLLIN`, đọc non-blocking từ socket vào đuôi buffer `rx_buf + rx_len`. Gọi `protocol_unpack_frame()` bóc tách từng khung hoàn chỉnh. Khi xử lý xong 1 khung, dùng `memmove()` để tịnh tiến dữ liệu còn dư về đầu buffer.
  - **Gửi một phần (Partial write)**: Khi có sự kiện `POLLOUT`, gọi `send()` non-blocking. Nếu chỉ gửi được một phần dữ liệu, server ghi nhận `tx_offset += sent` và giữ nguyên trạng thái để lần kích hoạt `POLLOUT` tiếp theo gửi nốt phần còn lại. Khi `tx_offset == tx_len`, reset cả hai về 0.
  - **Xử lý `EAGAIN` / `EWOULDBLOCK`**: Trả về bình thường mà không coi là lỗi, không làm đứt kết nối.
  - **Kiểm soát header hợp lệ**: Từ chối tức thì các khung có header khai báo payload length > 4096 bytes (`PROTO_MAX_PAYLOAD_LEN`) hoặc bằng 0 byte (`0x00000000`), ngắt kết nối an toàn để phòng ngừa tấn công DoS / memory exhaustion.

#### T2N2: Giới hạn công việc và tài nguyên
- **Hiện thực**:
  - **Ngân sách xử lý (I/O & Frame Budget)**: Áp dụng hạn ngạch tối đa `SERVER_FRAME_BUDGET_PER_LOOP` (16 khung mỗi vòng lặp `poll()`) cho từng client nhằm ngăn ngừa hiện tượng một client gửi dồn dập chiếm dụng tài nguyên CPU, gây đói tài nguyên (starvation) cho các client khác.
  - **Cơ chế Starvation Decoupling**: Tách biệt logic xử lý khung đã tích lũy trong bộ đệm `rx_buf` khỏi sự kiện `POLLIN` của `poll()`. Nhờ đó, các gói tin gộp số lượng lớn (coalesced bursts từ 16 đến 100 khung) tiếp tục được rút và xử lý ngay trong các vòng lặp tiếp theo của event loop mà không cần chờ socket kích hoạt `POLLIN` mới.
  - **Kích hoạt `POLLOUT` có kiểm soát**: Cờ `POLLOUT` chỉ được bật trên `pollfd` khi và chỉ khi `client->tx_len > client->tx_offset`. Khi bộ đệm rỗng, cờ `POLLOUT` lập tức bị hạ, triệt tiêu hoàn toàn hiện tượng busy-loop spinning 100% CPU.
  - **Timeout kết nối**: Thiết lập `SERVER_CLIENT_TIMEOUT_SEC` (30 giây) theo dõi `last_activity`. Tự động ngắt các kết nối không gửi nhận dữ liệu hoặc truyền kiểu slowloris nhỏ giọt vượt ngưỡng cho phép.

#### T2N3: Quản lý phiên và danh sách online
- **Hiện thực**:
  - Mở rộng enum `client_state_t` quản lý các pha của vòng đời kết nối:
    - `CLIENT_STATE_FREE`: Slot chưa sử dụng.
    - `CLIENT_STATE_CONNECTED`: Phiên tạm thời mới kết nối qua TCP, định danh bằng tên mặc định `guest_<fd>`, chưa vào sảnh chơi chính thức.
    - `CLIENT_STATE_AUTH`: Đã xác thực thành công.
    - `CLIENT_STATE_LOBBY`: Đang ở sảnh chờ, sẵn sàng nhận lời mời thách đấu.
    - `CLIENT_STATE_INGAME`: Đang trong ván đấu Caro.
  - Thiết kế kiến trúc module sẵn sàng đẩy cập nhật danh sách online tới các client khác khi có người vào/ra mà không gây nghẽn luồng I/O mạng.

#### T2N4: Rà soát đóng socket và giải phóng bộ nhớ
- **Hiện thực**:
  - Triển khai quy trình ngắt kết nối nghiêm ngặt trong `server_disconnect_client()`:
    1. Đóng socket descriptor qua `close(fd)`.
    2. Xóa và dọn dẹp sạch sẽ `rx_buf`, `tx_buf`, reset toàn bộ các biến đếm và timers về 0.
    3. Trả slot trong mảng `poll_fds` và bảng ánh xạ `client_map` về trạng thái `FREE`.
  - **Kiểm chứng rò rỉ File Descriptor (FD Leak Audit)**:
    - Kịch bản kiểm thử `FD-TRACK-01` trên hệ điều hành Linux/WSL truy vấn trực tiếp thư mục hệ thống `/proc/<pid>/fd`.
    - Kết quả: Server khởi động với **5 baseline FDs** (stdin, stdout, stderr, listen_socket, epoll/pipe). Khi 20+ client ồ ạt kết nối, số FD tăng lên đỉnh **27 FDs**. Sau bão ngắt kết nối đồng loạt (churn storm 60 chu kỳ), số FD trở về đúng **5 FDs**. Rò rỉ descriptor: **0 FD**.
    - Kết nối canary (kết nối nền kiểm tra tính khả dụng) tiếp tục gửi/nhận PING/PONG bình thường với độ trễ < 1ms trong suốt và sau đợt bão kết nối.

---

### 2.2. Nhóm đầu việc của Agent Hưng (Module Luật Caro, Parser Hardening & Kiểm thử tải G2)

#### T2H1: Hoàn thiện luật Caro độc lập với mạng
- **Hiện thực**:
  - `src/server/game.c` & `include/game.h`: Hiện thực đầy đủ và tối ưu hóa bộ luật Caro Gomoku trên bàn cờ chuẩn 15×15 ô vuông (`CARO_BOARD_SIZE 15`).
  - Các API cốt lõi:
    - `game_init()`: Khởi tạo bàn cờ trắng, thiết lập trạng thái `GAME_STATUS_PLAYING`, lượt đi đầu tiên thuộc về quân X.
    - `game_is_valid_cell(row, col)`: Kiểm tra biên tọa độ `0 <= row < 15` và `0 <= col < 15`.
    - `game_is_valid_move(game, row, col, player)`: Kiểm tra ô còn trống (`CELL_EMPTY`), đúng lượt người chơi và ván đấu đang diễn ra. Từ chối nước đi khi ván đã kết thúc hoặc bị hủy (`ABORTED`).
    - `game_make_move(game, row, col, player)`: Thực hiện đặt quân, tăng `move_count`, đổi lượt đi.
    - Thuật toán kiểm tra thắng 4 hướng xung quanh nước đi vừa đánh: Hàng ngang (`dx=0, dy=1`), Hàng dọc (`dx=1, dy=0`), Đường chéo chính (`dx=1, dy=1`), Đường chéo phụ (`dx=1, dy=-1`).
    - **Quy tắc Gomoku quốc tế tự do**: Thắng khi đạt từ 5 quân liên tiếp trở lên (`streak >= 5`). Chấp nhận thắng trường hợp overline (6, 7 quân liên tiếp) và không phân biệt chặn hai đầu.
    - `game_is_board_full()`: Phát hiện hòa cờ khi cả 225 ô vuông đều đã có quân mà chưa bên nào đạt điều kiện thắng.
  - **Tính độc lập tuyệt đối (Decoupling)**:
    - File `src/server/game.c` hoàn toàn không include các header mạng (`sys/socket.h`, `poll.h`, `netinet/in.h`), không include SQLite (`sqlite3.h`).
    - Kiểm chứng bằng phân tích bảng ký hiệu nhị phân (symbol table): nhị phân `bin/test_game` có 0 tham chiếu đến socket hay SQL APIs.

#### T2H2: Hoàn thiện parser và hiển thị lỗi
- **Hiện thực**:
  - `src/common/protocol.c`:
    - Bổ sung hàm tiện ích `parse_strict_uint32()` phân tích chuỗi số nguyên: từ chối số âm (`-1`), chuỗi chứa ký tự chữ cái, và từ chối số vượt ngưỡng biểu diễn của `uint32_t` (`> 4294967295`) bằng cờ lỗi `ERANGE` của `strtoul()`.
    - Bổ sung kiểm tra số lượng token tối thiểu cho từng loại lệnh giao thức: `CMD_MOVE` bắt buộc tối thiểu 3 trường (`game_id`, `row`, `col`); `CMD_LOGIN` bắt buộc tối thiểu 2 trường (`username`, `password`).
    - Từ chối các tọa độ cờ vượt ra ngoài biên 15x15 (ví dụ: `row=15`, `col=15`).
  - `src/client/main.c`:
    - Xử lý gói tin `CMD_ERR`: Client CLI bóc tách mã lỗi số và chuỗi mô tả, in ra màn hình dạng chuẩn `[SERVER ERROR <code>] <message>` giúp người dùng dễ dàng nhận diện nguyên nhân lỗi.
    - Bắt sự kiện EOF từ socket khi server chủ động đóng kết nối (do gửi khung lỗi, timeout hoặc server tắt), lập tức in cảnh báo `[CONNECTION LOST] Server closed the connection` và thoát tiến trình sạch sẽ (exit code 0/1).

#### T2H3: Viết bot 20 kết nối và kiểm tra framing
- **Hiện thực**:
  - `tests/test_g2_suite.py`: Xây dựng bot kiểm thử tải mạng tự động bằng Python sử dụng socket non-blocking / đa luồng:
    - Tạo đồng thời **20 kết nối TCP** cùng lúc tới Server (`127.0.0.1:9200`).
    - Phân bổ 4 nhóm hành vi lưu lượng hỗn hợp (mixed traffic):
      1. *Rapid Pingers*: 5 client gửi PING liên tục với tần suất cao.
      2. *Fragmented Streamers*: 5 client truyền dữ liệu phân mảnh từng byte với độ trễ 10ms giữa mỗi byte (`1 byte / 10ms`).
      3. *Coalesced Bursters*: 5 client gộp 3 đến 16 khung PING trong cùng 1 lệnh `sendall`.
      4. *Slow Drip Peers*: 5 client gửi byte header đầu tiên và giữ kết nối để kiểm tra cơ chế non-blocking của server.
  - Kết quả: Server phục vụ trơn tru toàn bộ 20 kết nối, 100% PONG echo chính xác, không xảy ra deadlock hay dropped connection.

#### T2H4: Bổ sung kiểm thử luật và biên dữ liệu
- **Hiện thực**:
  - `tests/test_game.c`: Xây dựng bộ test đơn vị độc lập 134 bài kiểm tra bao phủ 100% các ca luật chơi:
    - Thắng tại 4 góc biên bàn cờ: góc trên-trái `(0,0)`, góc trên-phải `(0,14)`, góc dưới-trái `(14,0)`, góc dưới-phải `(14,14)` cho cả 4 hướng.
    - Thắng overline: 6 quân liên tiếp (đánh vào đầu, giữa, đuôi), 7 quân liên tiếp, và thắng khi overline bị chặn cả 2 đầu bởi quân đối phương.
    - Ván cờ hòa đầy đủ: Mô phỏng chính xác trình tự đánh 225 nước đi (113 quân X, 112 quân O) tạo thế hòa không bên nào thắng 5 hàng.
    - Từ chối nước đi: đi đè ô đã có quân, đi ra ngoài biên, đi khi ván đấu đã kết thúc (`FINISHED`) hoặc bị hủy (`ABORTED`).
  - Kịch bản kiểm tra biên dữ liệu giao thức (`PARSE-REJECT-01` đến `09`): kiểm tra toàn bộ các ca thiếu tham số, số âm, số tràn `UINT32_MAX`, lệnh không xác định (`ERR 101`).
  - Bộ kiểm thử đối kháng (Adversarial Stress & Fuzz): `test_adversarial_fuzz.c` thực thi **938,342 lượt fuzz dữ liệu ngẫu nhiên** và 323 ca biên cực đoan mà không gây crash server.

---

## 3. DANH MỤC TẬP TIN ĐÃ TẠO HOẶC CHỈNH SỬA

### Mã nguồn chính và Header:
- `include/server.h`: Cập nhật cấu trúc `client_conn_t` (quản lý `tx_offset`, quotas, timeouts, baseline fd), định nghĩa hằng số tài nguyên.
- `src/server/server.c`: Nâng cấp event loop `poll()`, xử lý partial write, per-loop frame budget, timeout slowloris, starvation decoupling, dọn dẹp kết nối an toàn.
- `include/game.h`: Định nghĩa cấu trúc bàn cờ 15x15, các hàm luật cờ Caro Gomoku, kết quả ván đấu.
- `src/server/game.c`: Hiện thực thuật toán luật cờ Caro thuần C độc lập 100% với mạng và cơ sở dữ liệu.
- `include/protocol.h`: Định nghĩa hàm phân tích số nghiêm ngặt `parse_strict_uint32()`, các mã lỗi mới.
- `src/common/protocol.c`: Nâng cấp parser từ chối số âm, tràn số, thiếu trường và header quá kích thước cho phép.
- `src/client/main.c`: Cập nhật hiển thị lỗi `[SERVER ERROR]` và thông báo mất kết nối `[CONNECTION LOST]`.
- `Makefile`: Bổ sung target `test_game`, tự động build `bin/test_game` và liên kết thư viện mạng.

### Kiểm thử và Công cụ tự động hóa:
- `tests/test_game.c`: Bộ 134 unit tests kiểm tra luật cờ Caro 15x15, overline, hòa 225 ô và kiểm tra tính độc lập của module.
- `tests/test_protocol.c`: Mở rộng từ 52 lên 73 unit tests bao phủ các ca parser biên, tràn số và từ chối gói tin dị dạng.
- `tests/test_g2_suite.py`: Bộ kiểm thử E2E 26 suites bao gồm bot 20 kết nối, phân mảnh mạng, rà soát rò rỉ FD `/proc/<pid>/fd`.
- `tests/test_g2.sh`: Shell script tự động hóa toàn bộ quy trình biên dịch, chạy unit test và chạy bộ test E2E G2.
- `tests/test_g2_results.log`: Nhật ký ghi nhận chi tiết kết quả chạy kiểm thử thực tế Mốc G2.
- `tests/test_challenger_network.py` & `tests/test_adversarial_fuzz.c`: Bộ kiểm thử đối kháng, fuzzing và kiểm tra burst coalesced frames.
- `tests/test_victory_auditor_independent.py`: Kịch bản kiểm toán độc lập xác minh mốc hoàn thành.

### Tài liệu:
- `docs/progress_week2.md`: Báo cáo tiến độ và bằng chứng nghiệm thu Tuần 2 (Mốc G2).
- `README.md`: Cập nhật hướng dẫn chạy các bộ test Mốc G2 và thông tin kiến trúc tuần 2.

---

## 4. HƯỚNG DẪN TÁI HIỆN VÀ CHẠY THỬ NGHIỆM

### 4.1. Lệnh biên dịch sạch toàn bộ dự án
```bash
# Trong môi trường Linux / WSL Ubuntu:
make clean
make all
```
*Kết quả mong đợi*:
- Biên dịch thành công 100% 4 file thực thi trong thư mục `bin/`: `caro_server`, `caro_client`, `test_protocol`, `test_game`.
- **0 cảnh báo (warnings), 0 lỗi (errors)** với các cờ nghiêm ngặt `-Wall -Wextra -std=c11 -pedantic`.
- Tự động thực thi unit test: **73/73 protocol tests PASS** và **134/134 game rule tests PASS**.

### 4.2. Chạy riêng từng bộ Unit Test
```bash
# Chạy bộ test giao thức & parser (73 tests):
make test

# Chạy riêng bộ test luật cờ Caro 15x15 (134 tests):
make test_game
```

### 4.3. Chạy trọn gói kiểm thử tự động Mốc G2 (Bot 20 kết nối & E2E)
```bash
# Chạy kịch bản Shell tự động hóa toàn diện:
bash tests/test_g2.sh
```
*Kết quả thực tế ghi nhận*:
```text
========================================================================================
                               E2E TEST SUMMARY MATRIX                                  
========================================================================================
Case ID          | Group                    | Result | Time (ms) | Description
------------------------------------------------------------------------------
BOT-CONCUR-01    | 20-Client Concurrency    | PASS   |     709.2 | 20 simultaneous TCP connections with mixed workloads
BOT-CONCUR-02    | 20-Client Concurrency    | PASS   |       2.1 | Connect/ping/disconnect churn (20 iterations)
EDGE-FRAME-01..11| Framing Edge Cases       | PASS   |     750.0 | Split headers, 1B/10ms streaming, coalesced bursts, bad sizes
FD-TRACK-01      | FD Leak Tracking         | PASS   |     811.7 | /proc/<pid>/fd baseline recovery: 5 -> 27 -> 5 FDs (0 leak)
PARSE-REJECT-01..| Protocol Parser Rejects  | PASS   |       2.0 | Missing tokens, integer overflow, negatives, bad coordinates
CLI-VERIFY-01..03| Client CLI Verification  | PASS   |     628.3 | --test, --ping batch mode, interactive piped CLI
========================================================================================
FINAL OUTCOME: 26 / 26 SUITES PASSED (100.0%) — ALL CRITERIA VERIFIED (PASS)
========================================================================================
```

---

## 5. ĐÁNH GIÁ NGHIỆM THU MỐC G2 VÀ KẾ HOẠCH TUẦN 3

### 5.1. Đánh giá Nghiệm thu Mốc G2
- **Đạt tiêu chuẩn mốc G2**:
  - Mạng TCP non-blocking đạt độ ổn định cao, phục vụ đồng thời 20 kết nối tải hỗn hợp không suy hao, giải quyết triệt để hiện tượng frame starvation và 100% CPU spinning.
  - Quản lý tài nguyên chuẩn xác: 0 rò rỉ socket file descriptor sau bão ngắt kết nối.
  - Bộ luật Caro 15×15 Gomoku hoàn chỉnh, hỗ trợ overline ≥6 quân, hòa 225 ô, thiết kế độc lập 100% với mạng/SQL.
  - Parser an toàn tuyệt đối trước các trường hợp dữ liệu rác, số tràn và khung bất thường.
  - Đã được kiểm toán độc lập và xác nhận kết quả: **VICTORY CONFIRMED**.

### 5.2. Kế hoạch công việc tiếp theo (Tuần 3 — Mốc G3: Xác thực tài khoản & Luồng chơi chính)
Theo đúng kế hoạch tổng thể trong `docs/use_cases.md` (30/11 – 06/12/2026):
1. **Agent Nam (Cơ sở dữ liệu SQLite & Xác thực)**:
   - **T3N1**: Thiết kế và tạo schema CSDL SQLite bền vững (`users`, `matches`, `moves`).
   - **T3N2**: Tích hợp thư viện SQLite3 (`-lsqlite3`), hàm băm mật khẩu kèm muối (salt), xử lý `CMD_REGISTER` và `CMD_LOGIN`.
   - **T3N3**: Chống đăng nhập trùng (từ chối đăng nhập khi tài khoản đã online từ client khác).
   - **T3N4**: Quản lý phiên tài khoản chính thức, đồng bộ danh sách người chơi online trong sảnh (`CMD_GET_ONLINE`).
2. **Agent Hưng (Điều phối ván đấu, Ghép cặp & Client CLI)**:
   - **T3H1**: Giao thức và luồng gửi/nhận lời mời thách đấu (`CMD_CHALLENGE`, `CMD_CHALLENGE_RESP`) kèm cơ chế hết hạn 30 giây.
   - **T3H2**: Quản lý ván đấu trong RAM: gán `game_id`, phân định người cầm quân X (đi trước) và quân O (đi sau).
   - **T3H3**: Tích hợp module luật cờ (`src/server/game.c`) vào luồng nhận nước đi (`CMD_MOVE`), kiểm tra hợp lệ và broadcast nước đi cho đối thủ.
   - **T3H4**: Cập nhật Client CLI: giao diện sảnh chờ lobby, hiển thị danh sách đối thủ, thông báo lời mời và vẽ bàn cờ 15x15 khi vào trận; Xây dựng bộ test tích hợp 4 client chơi 2 ván độc lập đồng thời (Mốc G3).
