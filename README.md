# Cờ Caro Trực Tuyến bằng C/Linux (Caro Online)

Dự án môn học Lập trình mạng - Nhóm 8.  
Hệ thống trò chơi cờ Caro (Gomoku) trực tuyến theo mô hình Client-Server phục vụ nhiều client kết nối đồng thời trên nền tảng POSIX C11/Linux socket không chặn và `poll()`.

---

## 1. Thông Tin Thành Viên và Phân Công

| Thành viên | MSSV | Vai trò & Trách nhiệm chính |
|---|---|---|
| **Trịnh Ngọc Hoàng Nam** | **20235793** | Quản lý repo, build system (Makefile), Socket I/O non-blocking, Event loop `poll()`, Quản lý phiên/kết nối, Luật cờ Caro Gomoku 15x15, Xác thực tài khoản, Tích hợp SQLite. |
| **Đào Nhật Hưng** | **20235737** | Đặc tả giao thức ứng dụng, Framing & Parser (`protocol.c`), CLI Client dồn kênh `poll()`, Kiểm thử tích hợp G1, Tài liệu tiến độ Tuần 1, Ghép cặp/thách đấu, Bot kiểm thử. |

---

## 2. Cấu Trúc Thư Mục Dự Án

```text
caro-online/
├── Makefile                    # Hệ thống build tự động chuẩn POSIX C11 (GCC)
├── .gitignore                  # Bỏ qua binary, object, db, logs, agent state
├── README.md                   # Tài liệu tổng quan và hướng dẫn build/chạy/test
├── bin/                        # File thực thi sau khi biên dịch (caro_server, caro_client, test_protocol)
├── build/                      # Intermediate object files (*.o)
├── include/                    # Header files công khai
│   ├── protocol.h              # Giao thức framing nhị phân 4 byte, enum lệnh và struct thông điệp
│   ├── server.h                # Cấu trúc server, client context và API socket non-blocking
│   └── game.h                  # Bàn cờ 15x15 Gomoku, trạng thái ô, luật thắng >= 5 quân & overline
├── src/                        # Mã nguồn C
│   ├── common/                 # Module dùng chung
│   │   └── protocol.c          # Đóng gói và giải mã khung dữ liệu nhị phân / phân tách TAB
│   ├── server/                 # Module Server
│   │   ├── main.c              # Server CLI entry point và xử lý tín hiệu SIGINT/SIGTERM
│   │   ├── server.c            # Event loop poll() non-blocking, bộ đệm RX/TX và dispatch PING/PONG
│   │   └── game.c              # Thuật toán kiểm tra thắng thua Caro Gomoku 15x15 genuine
│   └── client/                 # Module Client CLI
│       └── main.c              # Client CLI đa kênh poll() giữa stdin và socket, hỗ trợ ping/help/quit
├── tests/                      # Kiểm thử đơn vị và tích hợp
│   ├── test_protocol.c         # Bộ 52 unit tests kiểm tra giao thức & framing
│   ├── test_integration_g1.py  # Kịch bản kiểm thử tích hợp 2 client đồng thời & nghiệm thu G1
│   ├── test_integration_g1.sh  # Script shell chạy trọn gói kiểm thử nghiệm thu G1
│   └── test_g1_results.log     # Nhật ký kết quả kiểm thử thực tế mốc G1
└── docs/                       # Tài liệu thiết kế kiến trúc, ca sử dụng, giao thức, tiến độ
    ├── architecture.md         # Kiến trúc hệ thống và luồng dữ liệu
    ├── protocol.md             # Đặc tả giao thức tầng ứng dụng
    ├── use_cases.md            # Đặc tả 22 ca sử dụng toàn dự án
    └── progress_week1.md       # Báo cáo tiến độ và bằng chứng kiểm thử 8 checklist Tuần 1
```

---

## 3. Yêu Cầu Môi Trường

- **Hệ điều hành**: Linux (Ubuntu 20.04/22.04/24.04, Debian) hoặc Windows với WSL (Khuyến nghị WSL Ubuntu) hoặc MinGW-w64 (MSYS2 UCRT64).
- **Trình biên dịch**: GCC (hỗ trợ C11, cờ `-Wall -Wextra -std=c11 -pedantic -O2 -D_POSIX_C_SOURCE=200809L`).
- **Công cụ build**: GNU Make (>= 4.0).
- **Python**: Python 3.8+ (dành cho kịch bản kiểm thử tích hợp tự động).

---

## 4. Hướng Dẫn Biên Dịch

```bash
# Biên dịch toàn bộ các thành phần và chạy unit test protocol:
make all

# Chỉ biên dịch Server:
make server

# Chỉ biên dịch Client:
make client

# Chạy riêng unit test protocol:
make test

# Dọn dẹp thư mục build/ và bin/:
make clean
```
*Lưu ý: Toàn bộ mã nguồn biên dịch sạch 100% không cảnh báo (zero warnings) với `-Wall -Wextra -std=c11 -pedantic`.*

---

## 5. Hướng Dẫn Chạy Demo Hai Client Đồng Thời (Mốc G1)

Để kiểm chứng hai client kết nối và trao đổi thông điệp PING/PONG độc lập với Server, mở 3 cửa sổ terminal riêng biệt:

### Bước 1: Khởi động Server (Terminal 1)
```bash
./bin/caro_server -p 8888
```
*Server sẽ khởi tạo socket non-blocking trên cổng 8888 và bước vào vòng lặp `poll()`.*

### Bước 2: Khởi động Client 1 — Alice (Terminal 2)
```bash
./bin/caro_client -h 127.0.0.1 -p 8888
caro> help
caro> ping Hello_from_Alice
# Màn hình hiển thị: [SERVER] PONG: Hello_from_Alice
caro> 
```

### Bước 3: Khởi động Client 2 — Bob (Terminal 3)
```bash
./bin/caro_client -h 127.0.0.1 -p 8888
caro> ping Hello_from_Bob
# Màn hình hiển thị: [SERVER] PONG: Hello_from_Bob
caro> quit
```

### Bước 4: Kiểm tra tính độc lập và phục hồi
- Khi Bob gõ `quit`, Server ngắt kết nối cho Bob an toàn và giải phóng tài nguyên.
- Quay lại Terminal 2 (Alice), gõ tiếp lệnh:
  ```bash
  caro> ping Alice_still_active
  # Màn hình tiếp tục hiển thị: [SERVER] PONG: Alice_still_active
  ```
- Kết nối một Client 3 mới vào để kiểm chứng Server tái sử dụng slot kết nối cũ mà không cần khởi động lại.

### Các chế độ chạy tiện ích của Client CLI:
```bash
# 1. Chạy tự kiểm thử nội bộ Client:
./bin/caro_client --test

# 2. Batch mode (gửi PING, chờ PONG, kiểm tra khớp echo, thoát mã 0/1):
./bin/caro_client -p 8888 --ping "my_ping_message"

# 3. Xem hướng dẫn lệnh CLI:
./bin/caro_client --help
```

---

## 6. Hướng Dẫn Chạy Kiểm Thử Tự Động Nghiệm Thu G1

Dự án cung cấp bộ kịch bản kiểm thử tích hợp tự động toàn diện kiểm tra 7 hạng mục chuyên sâu:
1. Hai tiến trình Client C chạy đồng thời chế độ batch mode.
2. Hai socket TCP bền vững gửi/nhận PING/PONG song song.
3. Client 1 ngắt kết nối đột ngột (EOF), Client 2 tiếp tục hoạt động bình thường.
4. Client 3 kết nối vào tái sử dụng slot thành công.
5. Mạng phân mảnh: truyền từng byte một (1 byte / 20ms) cho cả header 4 byte và payload, server tự ghép khung chính xác.
6. Gói tin gộp: 2 khung trong 1 lần ghi socket, server bóc tách lần lượt cả 2 khung.
7. Tương tác CLI qua stdin pipe và shutdown server sạch sẽ qua tín hiệu.

### Lệnh chạy kiểm thử tích hợp:
```bash
# Cách 1: Chạy bằng script Shell (tự động build, khởi động server, chạy test):
./tests/test_integration_g1.sh

# Cách 2: Chạy trực tiếp qua Python 3:
python3 tests/test_integration_g1.py
```
*Kết quả chi tiết được lưu trữ tại `tests/test_g1_results.log`.*

---

## 7. Báo Cáo Tiến Độ Tuần 1 (Đạt Mốc Nghiệm Thu G1)

Toàn bộ 8/8 đầu việc theo kế hoạch Tuần 1 đã hoàn thành xuất sắc 100%:
- [x] **T1N1**: Khởi tạo repository, Makefile chuẩn C11, skeleton server/client (`66f0b23`).
- [x] **T1N2**: Chốt cấu trúc `client_conn_t`, `game_t` và thuật toán luật Caro Gomoku 15x15 (29/29 tests pass).
- [x] **T1N3**: Hoàn thiện khung socket non-blocking và event loop `poll()` đơn luồng.
- [x] **T1N4**: Tích hợp framing 4-byte big-endian, bộ đệm RX/TX, xử lý PING/PONG và ngắt kết nối an toàn (`8ab2614`).
- [x] **T1H1**: Đặc tả 22 ca sử dụng dự án, chốt luật Caro 15x15 thắng >= 5 quân (`docs/use_cases.md`).
- [x] **T1H2**: Đặc tả giao thức và parser dùng chung `protocol.c` (52/52 tests pass, 0 warning).
- [x] **T1H3**: Hiện thực Client CLI dồn kênh `poll()` cho stdin và socket, hỗ trợ ping/help/quit và batch mode.
- [x] **T1H4**: Kiểm chứng mốc hai client đồng thời và nghiệm thu G1 (7/7 suites pass, log tại `tests/test_g1_results.log`).

Chi tiết xem tại tài liệu nghiệm thu: [`docs/progress_week1.md`](docs/progress_week1.md).
