# Cờ Caro Trực Tuyến bằng C/Linux (Caro Online)

Dự án môn học Lập trình mạng - Nhóm 8.  
Hệ thống trò chơi cờ Caro (Gomoku) trực tuyến theo mô hình Client-Server phục vụ nhiều client kết nối đồng thời trên nền tảng POSIX C11/Linux socket không chặn và `poll()`.

---

## 1. Thông Tin Thành Viên và Phân Công

| Thành viên | MSSV | Vai trò & Trách nhiệm chính |
|---|---|---|
| **Trịnh Ngọc Hoàng Nam** | **20235793** | Quản lý repo, build system (Makefile), Socket I/O non-blocking, Event loop `poll()`, Quản lý phiên/kết nối, Xác thực tài khoản, Tích hợp SQLite. |
| **Đào Nhật Hưng** | **20235737** | Đặc tả giao thức ứng dụng, Framing & Parser (`protocol.c`), CLI Client, Luật cờ Caro (15x15, thắng >= 5 quân), Ghép cặp/thách đấu, Leaderboard/Log, Bot kiểm thử. |

---

## 2. Cấu Trúc Thư Mục Dự Án

```text
caro-online/
├── Makefile                # Hệ thống build tự động chuẩn POSIX C11
├── .gitignore              # Bỏ qua binary, object, db, logs, agent state
├── README.md               # Tài liệu tổng quan và hướng dẫn build/chạy
├── bin/                    # Thư mục chứa file thực thi sau khi biên dịch
├── build/                  # Thư mục chứa intermediate object files (*.o)
├── include/                # Header files công khai
│   ├── protocol.h          # Định nghĩa giao thức, framing và struct thông điệp
│   ├── server.h            # Cấu trúc server, client context và API socket
│   └── game.h              # Cấu trúc bàn cờ 15x15, trạng thái ván và nước đi
├── src/                    # Mã nguồn C
│   ├── common/             # Module dùng chung (protocol framing/parser)
│   │   └── protocol.c
│   ├── server/             # Module Server (listener, event loop, sessions)
│   │   ├── main.c
│   │   └── server.c
│   └── client/             # Module Client CLI
│       └── main.c
├── tests/                  # Kiểm thử đơn vị và tích hợp
│   └── test_protocol.c     # Suite 52 test kiểm tra giao thức & framing
└── docs/                   # Tài liệu thiết kế kiến trúc, ca sử dụng, giao thức
    ├── architecture.md     # Kiến trúc hệ thống và luồng dữ liệu
    ├── protocol.md         # Đặc tả giao thức chi tiết
    └── use_cases.md        # Danh sách ca sử dụng toàn dự án
```

---

## 3. Yêu Cầu Môi Trường

- **Hệ điều hành**: Linux (Ubuntu 20.04/22.04/24.04, Debian) hoặc Windows với WSL (Khuyến nghị WSL Ubuntu) hoặc MinGW-w64 (MSYS2 UCRT64).
- **Trình biên dịch**: GCC (hỗ trợ C11, cờ `-Wall -Wextra -std=c11 -pedantic -O2 -D_POSIX_C_SOURCE=200809L`).
- **Công cụ build**: GNU Make (>= 4.0).

---

## 4. Hướng Dẫn Biên Dịch và Kiểm Thử

### Các mục tiêu trong Makefile:

- **Biên dịch toàn bộ và chạy unit tests:**
  ```bash
  make all
  ```
- **Chỉ biên dịch Server:**
  ```bash
  make server
  ```
  File thực thi sinh ra tại: `bin/caro_server` (hoặc `bin/caro_server.exe` trên Windows).

- **Chỉ biên dịch Client:**
  ```bash
  make client
  ```
  File thực thi sinh ra tại: `bin/caro_client` (hoặc `bin/caro_client.exe` trên Windows).

- **Chạy kiểm thử đơn vị:**
  ```bash
  make test
  ```

- **Dọn dẹp mã nhị phân và file trung gian:**
  ```bash
  make clean
  ```

---

## 5. Hướng Dẫn Chạy Thử Nghiệm

### Chạy Server:
```bash
# Mặc định lắng nghe trên 0.0.0.0:8888
./bin/caro_server

# Hoặc chỉ định IP và cổng:
./bin/caro_server -h 0.0.0.0 -p 8888

# Kiểm tra nhanh skeleton (khởi tạo, lắng nghe non-blocking rồi giải phóng):
./bin/caro_server --test
```

### Chạy Client:
```bash
# Kết nối đến server mặc định 127.0.0.1:8888
./bin/caro_client -h 127.0.0.1 -p 8888

# Kiểm tra nhanh skeleton:
./bin/caro_client --test
```

---

## 6. Kế Hoạch Triển Khai Tuần 1 (16/11 - 22/11/2026)

- [x] **T1N1**: Tạo repository, cấu trúc thư mục, Makefile chuẩn, .gitignore, skeleton server/client.
- [x] **T1H1**: Chốt phạm vi dự án và danh sách ca sử dụng (`docs/use_cases.md`).
- [x] **T1H2**: Viết đặc tả giao thức và parser framing 4 byte (`protocol.h`, `protocol.c`, test 52/52 pass).
- [ ] **T1N2**: Chốt cấu trúc Client và Game (`include/server.h`, `include/game.h`).
- [ ] **T1H3**: Chuẩn bị CLI Client không chờ nhập độc quyền (`src/client/`).
- [ ] **T1N3**: Hoàn thiện socket non-blocking và event loop `poll()` phục vụ nhiều client.
- [ ] **T1N4**: Ghép đường truyền gửi nhận PING/PONG tối thiểu.
- [ ] **T1H4**: Kiểm chứng mốc kết nối hai client đồng thời (Nghiệm thu G1).
