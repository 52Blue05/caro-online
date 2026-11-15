# BÁO CÁO TIẾN ĐỘ TUẦN 1 — NGHIỆM THU MỐC G1
## DỰ ÁN: CỜ CARO TRỰC TUYẾN BẰNG C/LINUX

- **Thời gian thực hiện**: 16/11/2026 – 22/11/2026 (Nghiệm thu: 15/11/2026)
- **Thành viên tham gia**:
  - **Trịnh Ngọc Hoàng Nam (MSSV 20235793)** — Phụ trách Server, mạng TCP, Event loop `poll()`, mô hình Client/Game.
  - **Đào Nhật Hưng (MSSV 20235737)** — Phụ trách Đặc tả ca sử dụng, Giao thức Framing, Client CLI, Kiểm thử tích hợp G1 và Tài liệu.
- **Mục tiêu Tuần 1 (Mốc G1)**: Thiết lập nền tảng kết nối mạng TCP socket non-blocking, đóng gói dữ liệu nhị phân 4-byte header + payload TAB, phục vụ ít nhất 2 client C kết nối đồng thời và trao đổi thông điệp PING/PONG tin cậy, ngắt kết nối an toàn và tái sử dụng tài nguyên.

---

## 1. BẢNG TỔNG HỢP TRẠNG THÁI 8 ĐẦU VIỆC (T1N1 – T1H4)

| Mã việc | Thời hạn | Người phụ trách | Tên đầu việc | Trạng thái | Bằng chứng kiểm thử thực tế |
|---|---|---|---|---|---|
| **T1N1** | 16/11 | Nam (20235793) | Tạo repository và kiểm tra skeleton | **HOÀN THÀNH (100%)** | Repo chuẩn `src/`, `include/`, `tests/`, `docs/`, `Makefile`, `.gitignore`. Build sạch cả trên WSL Ubuntu và Windows native. Commit `66f0b23`. |
| **T1N2** | 17–18/11 | Nam (20235793) | Chốt cấu trúc Client và Game | **HOÀN THÀNH (100%)** | `include/server.h`, `include/game.h`, `src/server/game.c`. 29/29 unit tests bàn cờ 15x15 Gomoku (thắng >=5 quân, overline, hòa). |
| **T1N3** | 19–20/11 | Nam (20235793) | Hoàn thiện khung socket và `poll()` | **HOÀN THÀNH (100%)** | `src/server/server.c` dùng `fcntl` non-blocking, non-blocking `accept()` loop, `poll()` đơn luồng quản lý mảng `pollfd`, không thread riêng. |
| **T1N4** | 21/11 | Nam (20235793) | Ghép đường gửi nhận tối thiểu (Framing & PING/PONG) | **HOÀN THÀNH (100%)** | Tích hợp framing 4-byte big-endian, bộ đệm 64KB rx/tx, xử lý EOF/EINTR/EAGAIN, ngắt kết nối an toàn, bỏ qua SIGPIPE. Commit `8ab2614`. |
| **T1H1** | 16/11 | Hưng (20235737) | Chốt phạm vi và các ca sử dụng | **HOÀN THÀNH (100%)** | `docs/use_cases.md` chi tiết 22 ca sử dụng 6 tuần, luật Caro 15x15 Gomoku, điều kiện thắng `>= 5` quân, phân kỳ G1–G6. |
| **T1H2** | 17–18/11 | Hưng (20235737) | Viết đặc tả giao thức ban đầu & parser dùng chung | **HOÀN THÀNH (100%)** | `docs/protocol.md`, `include/protocol.h`, `src/common/protocol.c`. Bộ unit test `tests/test_protocol.c` đạt 52/52 tests PASS. |
| **T1H3** | 19–20/11 | Hưng (20235737) | Chuẩn bị client CLI | **HOÀN THÀNH (100%)** | `src/client/main.c` dùng `poll()` dồn kênh stdin và socket, hỗ trợ `ping`, `help`, `quit`, batch mode `--ping <msg>`, `--test` (4/4 pass). |
| **T1H4** | 21–22/11 | Hưng (20235737) | Kiểm tra mốc hai client & Nghiệm thu G1 | **HOÀN THÀNH (100%)** | `tests/test_integration_g1.py`, `tests/test_integration_g1.sh`. Đạt 7/7 suites kiểm thử tích hợp. Log lưu tại `tests/test_g1_results.log`. |

---

## 2. CHI TIẾT KẾT QUẢ THỰC HIỆN TỪNG ĐẦU VIỆC

### 2.1. Nhóm đầu việc của Agent Nam (Server Core & Mạng)

#### T1N1: Tạo repository và kiểm tra skeleton
- **Hiện thực**: Khởi tạo Git repo, phân chia cây thư mục chuẩn POSIX: `src/server`, `src/client`, `src/common`, `include`, `tests`, `docs`.
- **Makefile**: Quản lý build đa mục tiêu (`all`, `server`, `client`, `test`, `clean`), cờ biên dịch `-Wall -Wextra -std=c11 -pedantic -O2 -D_POSIX_C_SOURCE=200809L -Iinclude`. Tự động nhận diện Linux/WSL và Windows (`-lws2_32`).
- **Bằng chứng**: Commit `66f0b23`, build thành công `bin/caro_server` và `bin/caro_client`.

#### T1N2: Chốt cấu trúc Client và Game
- **Hiện thực**:
  - `include/server.h`: Cấu trúc `client_conn_t` quản lý socket fd, `client_state_t`, IP/Port, bộ đệm tích lũy 64KB `rx_buf`, bộ đệm gửi `tx_buf`, ID phiên. Hàm ngắt kết nối an toàn `server_disconnect_client()`.
  - `include/game.h`: Bàn cờ `CARO_BOARD_SIZE 15`, `CARO_WIN_STREAK 5`, `cell_state_t` (EMPTY, X, O), `game_t` quản lý người chơi X/O, lượt đi, số thứ tự nước đi, trạng thái và kết quả.
  - `src/server/game.c`: Triển khai genuine logic kiểm tra thắng 4 hướng (ngang, dọc, chéo chính, chéo phụ) với `>= 5` quân liên tiếp, cho phép overline `>= 6` quân, phát hiện hòa khi đầy 225 ô.
- **Bằng chứng**: 29/29 unit tests trong `.agents/teamwork/worker_nam_m2/test_game.c` vượt qua 100%.

#### T1N3: Hoàn thiện khung socket và poll event loop
- **Hiện thực**:
  - Socket listener và client socket cấu hình non-blocking qua `fcntl(fd, F_SETFL, flags | O_NONBLOCK)`.
  - Vòng lặp `accept()` non-blocking tiếp nhận toàn bộ kết nối đang chờ trong backlog tới khi gặp `EAGAIN`/`EWOULDBLOCK`.
  - Vòng lặp `poll()` đơn luồng quản lý mảng `poll_fds` và bảng ánh xạ `client_map`. Cờ `POLLOUT` chỉ kích hoạt khi `tx_len > 0` nhằm triệt tiêu hiện tượng busy-loop.
- **Bằng chứng**: Server chạy ổn định, không chiếm dụng CPU bất thường, tiếp nhận và phục vụ nhiều client cùng lúc mà không tạo luồng riêng.

#### T1N4: Ghép đường gửi nhận tối thiểu (Framing & PING/PONG)
- **Hiện thực**:
  - Tích hợp hàm `protocol_unpack_frame()` từ module chung `src/common/protocol.c`.
  - Xử lý `POLLIN`: đọc non-blocking vào đuôi `rx_buf`, bóc tách khung hoàn chỉnh, trượt buffer bằng `memmove()`.
  - Xử lý thông điệp: nhận `CMD_PING` -> sinh `CMD_PONG` với chuỗi echo tương ứng và đưa vào hàng đợi `tx_buf` qua `server_queue_send()`.
  - Xử lý `POLLOUT`: gửi non-blocking dữ liệu với cờ `MSG_NOSIGNAL` kết hợp `signal(SIGPIPE, SIG_IGN)` để chống crash server khi client ngắt kết nối đột ngột.
  - Ngắt kết nối: khi nhận `EOF` (`recv() == 0`) hoặc lỗi kết nối, giải phóng slot ngay lập tức.
- **Bằng chứng**: Commit `8ab2614`, 8/8 test suites trong `test_server_integration.py` pass 100%.

---

### 2.2. Nhóm đầu việc của Agent Hưng (Giao thức, Client & Nghiệm thu)

#### T1H1: Chốt phạm vi và các ca sử dụng
- **Hiện thực**: Soạn thảo tài liệu đặc tả `docs/use_cases.md` mô tả chi tiết 22 ca sử dụng qua 6 tuần (UC01–UC22), xác định rõ ranh giới chức năng của Tuần 1 (kết nối mạng, framing, PING/PONG) và các tuần sau (xác thực, SQLite, lobby, luật cờ hoàn chỉnh, bot kiểm thử).
- **Quy tắc bàn cờ**: Chốt kích thước 15x15, thắng khi có ít nhất 5 quân liên tiếp, chấp nhận overline, không chặn 2 đầu.

#### T1H2: Viết đặc tả giao thức ban đầu & Parser dùng chung
- **Hiện thực**:
  - Soạn thảo `docs/protocol.md`: Quy định header cố định 4 byte (big-endian / network byte order, `uint32_t`) mang chiều dài payload; payload phân tách bằng TAB (`\t`) và kết thúc bằng `\n`.
  - Header `include/protocol.h`: Định nghĩa `protocol_msg_t`, enum lệnh (`CMD_PING`, `CMD_PONG`, `CMD_ERR`, `CMD_MOVE`, v.v.), enum mã lỗi (`ERR_UNKNOWN_CMD`, `ERR_BAD_FRAME`, v.v.).
  - Thư viện `src/common/protocol.c`: Hiện thực `protocol_pack_frame()`, `protocol_unpack_frame()` hỗ trợ partial/coalesced frames, `protocol_tokenize()`, `protocol_parse_msg()`.
- **Bằng chứng**: Bộ kiểm thử `tests/test_protocol.c` với 52/52 bài kiểm thử tự động đạt 100% PASS. Sửa triệt để cảnh báo `__func__` và `memcpy` an toàn đạt 0 warning với `-Wall -Wextra -std=c11 -pedantic`.

#### T1H3: Chuẩn bị client CLI
- **Hiện thực**:
  - `src/client/main.c`: Thiết lập client CLI C/Linux kết nối TCP tới server (`-h <host>`, `-p <port>`).
  - Dồn kênh phi chặn bằng `poll()`: Theo dõi đồng thời bàn phím `stdin` (fd 0) và socket TCP.
  - Khi người dùng đang chờ nhập lệnh hoặc prompt `caro> ` hiển thị, nếu server gửi gói tin đến, client vẫn đọc, unpack và hiển thị `[SERVER] PONG: <msg>` ngay lập tức mà không bị nghẽn bởi `fgets`.
  - Hỗ trợ các lệnh tương tác: `ping <message>`, `help`, `quit` / `exit`.
  - Đóng gói input bàn phím bằng `read(STDIN_FILENO, ...)` vào buffer dòng lệnh riêng, loại bỏ xung đột buffer libc với `poll()`.
  - Cờ tự động hóa: `--ping <msg>` phục vụ batch mode/scripting tự động, kiểm tra echo PONG và trả mã thoát `0` (thành công) hoặc `1` (thất bại).
  - Cờ tự kiểm thử nội bộ: `--test` kiểm tra 4 bài test logic packing/unpacking/parsing, thoát mã 0.
- **Bằng chứng**: Biên dịch 0 warning, tự kiểm thử `--test` đạt 4/4 PASS; batch mode `--ping` và interactive pipe hoạt động hoàn hảo.

#### T1H4: Kiểm tra mốc hai client & Nghiệm thu G1
- **Hiện thực**:
  - Xây dựng 2 kịch bản kiểm thử tích hợp tự động:
    - `tests/test_integration_g1.py`: Kịch bản Python toàn diện kiểm tra 7 hạng mục chuyên sâu.
    - `tests/test_integration_g1.sh`: Shell script thực thi kiểm thử tích hợp tự động trong môi trường bash/Linux.
  - Nội dung 7 bài kiểm thử tích hợp:
    1. **Hai Client C đồng thời**: Chạy đồng thời 2 tiến trình `caro_client` (`--ping alice` và `--ping bob`) trao đổi PING/PONG song song.
    2. **Hai Socket bền vững đồng thời**: Kết nối 2 socket TCP trao đổi dữ liệu chéo nhau không bị nghẽn.
    3. **Khả năng phục hồi khi mất kết nối**: Đóng đột ngột kết nối của Client Alice (EOF), kiểm tra Client Bob tiếp tục gửi/nhận PING/PONG bình thường (server không crash, event loop không đứt).
    4. **Tái sử dụng Slot & Client 3 (Charlie)**: Kết nối Client Charlie mới, xác nhận server dọn dẹp slot cũ và tiếp nhận client mới thành công.
    5. **Môi trường mạng khắc nghiệt - Phân mảnh (Fragmentation)**: Truyền từng byte một (1 byte / 20ms) cho cả 4-byte header và payload, xác nhận accumulator buffer của server tích lũy và ghép khung chính xác.
    6. **Gói tin gộp (Coalesced Frames)**: Gửi 2 khung gói tin hoàn chỉnh trong cùng 1 lần ghi socket (`sendall`), xác nhận server bóc tách lần lượt cả 2 khung và trả về đủ 2 PONG.
    7. **Tương tác CLI qua luồng nhập chuẩn**: Mô phỏng tương tác người dùng qua ống dẫn, kiểm tra in menu trợ giúp, gửi PING, in PONG từ server và thoát an toàn.
- **Bằng chứng**: 7/7 test suites đạt 100% PASS; toàn bộ nhật ký ghi lại tại `tests/test_g1_results.log`.

---

## 3. DANH MỤC TẬP TIN ĐÃ TẠO HOẶC CHỈNH SỬA

### Mã nguồn chính và Header:
- `include/protocol.h`: Định nghĩa giao thức framing và API đóng gói/giải mã (Hưng phụ trách).
- `src/common/protocol.c`: Hiện thực parser giao thức dùng chung 100% không warning (Hưng phụ trách).
- `src/client/main.c`: Hiện thực Client CLI dồn kênh `poll()` cho stdin và socket (Hưng phụ trách).
- `include/server.h`: Định nghĩa cấu trúc kết nối, trạng thái và event loop server (Nam phụ trách).
- `include/game.h`: Định nghĩa cấu trúc bàn cờ và luật Gomoku 15x15 (Nam phụ trách).
- `src/server/server.c`: Hiện thực socket non-blocking, event loop `poll()`, Framing RX/TX (Nam phụ trách).
- `src/server/main.c`: Entry point của Server CLI và xử lý tín hiệu (Nam phụ trách).
- `src/server/game.c`: Thuật toán kiểm tra thắng thua Caro Gomoku 15x15 (Nam phụ trách).
- `Makefile`: Cấu hình biên dịch tự động hóa C11 chuẩn POSIX (Nam phụ trách).

### Kiểm thử và Công cụ:
- `tests/test_protocol.c`: Bộ 52 unit tests cho giao thức framing (Hưng phụ trách).
- `tests/test_integration_g1.py`: Kịch bản kiểm thử tích hợp 2 client & nghiệm thu G1 (Hưng phụ trách).
- `tests/test_integration_g1.sh`: Kịch bản shell tự động hóa nghiệm thu G1 (Hưng phụ trách).
- `tests/test_g1_results.log`: Nhật ký ghi nhận kết quả kiểm thử thực tế (Hưng phụ trách).
- `.agents/teamwork/worker_nam_m2/test_game.c`: Bộ 29 unit tests cho luật cờ Caro (Nam phụ trách).
- `.agents/teamwork/worker_nam_m2/test_server_integration.py`: Kịch bản tích hợp server riêng của Nam (Nam phụ trách).

### Tài liệu:
- `docs/use_cases.md`: Đặc tả 22 ca sử dụng và quy tắc nghiệp vụ 6 tuần (Hưng phụ trách).
- `docs/architecture.md`: Thiết kế kiến trúc tổng thể, event loop và ranh giới module (Hưng & Nam).
- `docs/protocol.md`: Đặc tả chi tiết giao thức nhị phân và định dạng thông điệp (Hưng phụ trách).
- `docs/progress_week1.md`: Báo cáo tiến độ và bằng chứng nghiệm thu Tuần 1 (Hưng phụ trách).
- `README.md`: Hướng dẫn biên dịch, chạy demo 2 client và chạy kiểm thử (Hưng phụ trách).

---

## 4. HƯỚNG DẪN TÁI HIỆN VÀ CHẠY THỬ NGHIỆM

### 4.1. Lệnh biên dịch sạch
```bash
# Trong môi trường Linux / WSL Ubuntu:
make clean
make all
```
*Kết quả mong đợi*: Biên dịch thành công 100% `bin/caro_server`, `bin/caro_client`, `bin/test_protocol`, **0 cảnh báo, 0 lỗi**. 52/52 protocol unit tests tự động chạy và hiển thị `RESULTS: 52 / 52 tests passed`.

### 4.2. Chạy Demo 2 Client kết nối Server đồng thời
Mở 3 cửa sổ terminal riêng biệt:

**Terminal 1 — Khởi động Server:**
```bash
./bin/caro_server -p 8888
```

**Terminal 2 — Khởi động Client 1 (Alice):**
```bash
./bin/caro_client -h 127.0.0.1 -p 8888
caro> help
caro> ping Hello_from_Alice
# Nhận phản hồi: [SERVER] PONG: Hello_from_Alice
```

**Terminal 3 — Khởi động Client 2 (Bob):**
```bash
./bin/caro_client -h 127.0.0.1 -p 8888
caro> ping Hello_from_Bob
# Nhận phản hồi: [SERVER] PONG: Hello_from_Bob
caro> quit
```
*Quan sát*: Terminal 1 (Server) ghi nhận Alice và Bob kết nối, xử lý PING/PONG độc lập. Khi Bob gõ `quit`, Server ngắt kết nối an toàn cho Bob, giải phóng slot. Terminal 2 (Alice) tiếp tục hoạt động gửi `ping test_again` mà không hề bị gián đoạn.

### 4.3. Chạy toàn bộ kiểm thử tích hợp tự động (Nghiệm thu G1)
```bash
# Chạy bộ test tích hợp Python:
python3 tests/test_integration_g1.py

# Hoặc chạy qua kịch bản Shell:
./tests/test_integration_g1.sh
```
*Kết quả mong đợi*: Toàn bộ 7/7 test suites vượt qua (`FINAL INTEGRATION RESULTS: 7 / 7 SUITES PASSED`), file `tests/test_g1_results.log` được cập nhật đầy đủ.

---

## 5. ĐÁNH GIÁ NGHIỆM THU MỐC G1 VÀ KẾ HOẠCH TUẦN 2

### 5.1. Đánh giá Nghiệm thu Mốc G1
- **Đạt tiêu chuẩn mốc G1**: Nền tảng kết nối mạng TCP socket non-blocking, event loop `poll()`, framing nhị phân 4 byte và trao đổi thông điệp PING/PONG giữa 2 client đồng thời đã hoàn thành genuine 100%, không sử dụng mã giả, vượt qua toàn bộ các ca kiểm thử phân mảnh và mất kết nối.
- **Ranh giới thực tế**: Chưa bao gồm tính năng đăng nhập SQLite và chơi ván cờ trực tiếp (các chức năng này thuộc phạm vi Tuần 2 và Tuần 3 theo đúng phân công kế hoạch).

### 5.2. Kế hoạch công việc tiếp theo (Tuần 2)
1. **Agent Nam**:
   - T2N1: Thiết kế cơ sở dữ liệu SQLite (`users`, `matches`, `moves`).
   - T2N2: Tích hợp thư viện SQLite3 vào Server, hàm đăng ký (`CMD_REGISTER`) và đăng nhập (`CMD_LOGIN`) kiểm tra mật khẩu.
   - T2N3: Quản lý danh sách người chơi online trong lobby (`CMD_GET_ONLINE`, `CMD_ONLINE_LIST`).
   - T2N4: Xử lý lời mời thách đấu (`CMD_CHALLENGE`, `CMD_CHALLENGE_RESP`) và ghép cặp ván đấu.
2. **Agent Hưng**:
   - T2H1: Cập nhật giao thức tầng ứng dụng cho luồng xác thực và thách đấu.
   - T2H2: Xây dựng menu giao diện dòng lệnh Client: màn hình Đăng nhập/Đăng ký, màn hình Lobby xem danh sách người chơi.
   - T2H3: Giao diện gửi và nhận lời mời thách đấu trên Client CLI.
   - T2H4: Bộ test tự động kiểm tra xác thực (đăng nhập đúng/sai mật khẩu, trùng tài khoản, chặn đăng nhập 2 nơi) và chuẩn bị cho mốc Nghiệm thu G2.
