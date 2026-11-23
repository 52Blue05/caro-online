# BÁO CÁO TIẾN ĐỘ TUẦN 3 — NGHIỆM THU MỐC G3
## DỰ ÁN: CỜ CARO TRỰC TUYẾN BẰNG C/LINUX

- **Thời gian thực hiện**: 30/11/2026 – 06/12/2026 (Nghiệm thu thực tế: 23/11/2026)
- **Thành viên tham gia**:
  - **Trịnh Ngọc Hoàng Nam (MSSV 20235793)** — Phụ trách Lưu trữ SQLite (`storage.c`, `schema.sql`), Xác thực tài khoản (`auth.c`), Băm mật khẩu SHA-256 có Salt (`sha256.c`), Hàng đợi luồng ngầm Async DB Worker Queue, Kiểm soát phiên đơn (Single Active Session), Chống dò mật khẩu (Brute-force Throttling), Phòng chống xung đột tái sử dụng FD (`generation` counter).
  - **Đào Nhật Hưng (MSSV 20235737)** — Phụ trách Cơ chế Thách đấu & Lời mời (`CHALLENGE`, `ACCEPT`, `DECLINE`, Timeout 30s), Quản lý ván đấu độc lập trong RAM (`game_slot_t`), Kiểm duyệt nước cờ 6 bước trên Server (Server-Side 6-Step Move Validation), Đồng bộ kết thúc trận (WIN, DRAW, RESIGN, DISCONNECT), Ghi log chi tiết từng trận cờ (`logs/game_<id>.log`), Giao diện dòng lệnh tương tác và bàn cờ ASCII 15x15 (`src/client/main.c`), Bộ kiểm thử 4 tầng độc lập E2E (`tests/test_g3_suite.py`).
- **Mục tiêu Tuần 3 (Mốc G3)**: Cơ sở dữ liệu, Xác thực tài khoản và Quản lý trận cờ thời gian thực
  - Tạo schema SQLite cho `users`, `games`, `moves`; bảo vệ 100% bằng parameterized queries.
  - Đăng ký và đăng nhập bảo mật bằng SHA-256 với 16-byte random salt và hàm so sánh constant-time; bảo vệ luồng sự kiện mạng bằng hàng đợi Async DB Worker và cơ chế tự đánh thức pipe.
  - Cơ chế thách đấu thời gian thực: khóa hai chiều hai người chơi, hết hạn tự động sau 30 giây, từ chối người chơi đang bận.
  - Kiểm duyệt nước đi 6 bước chặt chẽ trên Server; hỗ trợ đầy đủ các trạng thái kết thúc trận đấu (Thắng 5 quân liên tiếp, Hòa, Đầu hàng, Rớt mạng).
  - 4 tài khoản chơi đồng thời 2 trận đấu độc lập trong RAM mà không có bất kỳ hiện tượng rò rỉ sự kiện chéo (cross-game event leakage).
  - Khách hàng CLI có giao diện tương tác trực tiếp và hiển thị bàn cờ 15x15 bằng ký tự ASCII.

---

## 1. BẢNG TỔNG HỢP TRẠNG THÁI 8 ĐẦU VIỆC (T3N1 – T3H4)

| Mã việc | Thời hạn | Người phụ trách | Tên đầu việc | Trạng thái | Bằng chứng kiểm thử thực tế |
|---|---|---|---|---|---|
| **T3N1** | 30/11–01/12 | Nam (20235793) | Thiết kế Schema và Hợp đồng Lưu trữ SQLite | **HOÀN THÀNH (100%)** | `schema.sql`, `include/storage.h`, `src/server/storage.c`: Bảng `users`, `games`, `moves`; ràng buộc Foreign Key, tọa độ `[0..14]`, số thứ tự `seq`; 113/113 unit tests PASS (`tests/test_storage.c`). |
| **T3N2** | 02/12–03/12 | Nam (20235793) | Triển khai Đăng ký / Đăng nhập với Salted Hash & Async Worker | **HOÀN THÀNH (100%)** | `src/common/sha256.c`, `src/server/auth.c`: Băm SHA-256 + 16-byte random salt, so sánh chuỗi hằng số thời gian; hàng đợi Async DB worker dung lượng 64 request; `self-pipe` đánh thức `poll()` không block I/O; 52/52 auth tests PASS. |
| **T3N3** | 04/12 | Nam (20235793) | Duy trì Phiên Đơn, Chống Brute-force & Bảo vệ Socket FD Reuse | **HOÀN THÀNH (100%)** | Từ chối đăng nhập lặp (`ERR 203`); khóa 30 giây sau 5 lần sai mật khẩu liên tiếp; gắn nhãn `generation` counter trên mỗi slot socket để loại bỏ kết quả DB trả về muộn cho socket đã đóng/tái sử dụng (`TC-T2-01` đến `TC-T2-04` PASS). |
| **T3N4** | 05/12 | Nam (20235793) | Tích hợp Xác thực vào Luồng Chơi & Ghi nhận Trạng thái | **HOÀN THÀNH (100%)** | Chỉ tài khoản xác thực mới được thách đấu; liên kết `user_id` với phiên làm việc; ghi nhận nhật ký hiện diện đăng nhập/đăng xuất `[PRESENCE]`; phát danh sách online cập nhật tự động. |
| **T3H1** | 30/11–01/12 | Hưng (20235737) | Cơ chế Thách đấu và Lời mời chơi thời gian thực | **HOÀN THÀNH (100%)** | `CHALLENGE`, `ACCEPT`, `DECLINE`: Khóa trạng thái `STATE_IN_INVITE` của 2 bên; từ chối người chơi đang bận (`ERR 301`); từ chối người thứ 3 can thiệp (`ERR 303`); quét timeout định kỳ hủy lời mời sau 30 giây (`ERR 302`) (`TC-T1-03`, `TC-T1-04`, `TC-T2-05` đến `TC-T2-07` PASS). |
| **T3H2** | 02/12–03/12 | Hưng (20235737) | Khởi tạo Trận đấu và Thẩm định Nước đi 6 bước trên Server | **HOÀN THÀNH (100%)** | Cấp phát `game_slot_t` trong RAM; phân định X (đi trước) và O; kiểm duyệt 6 bước: (1) Trận active, (2) Quyền người chơi, (3) Lượt đi, (4) Số thứ tự `seq`, (5) Tọa độ `[0..14]`, (6) Ô trống theo luật `game.c`. Phản hồi `MOVE_ACK` và `MOVE_BROADCAST` (`TC-T1-05`, `TC-T2-08` đến `TC-T2-12` PASS). |
| **T3H3** | 04/12 | Hưng (20235737) | Xử lý Kết thúc Trận và Đồng bộ Trạng thái | **HOÀN THÀNH (100%)** | Xử lý thắng (5 quân/overline), hòa (xin hòa được chấp nhận / đầy 225 ô), đầu hàng (`RESIGN`), ngắt kết nối (forfeit); chốt kết quả trong RAM chính xác 1 lần (*idempotency*); ghi nhận điểm số vào SQLite (`TC-T1-06` đến `TC-T1-08`, `TC-T3-02` PASS). |
| **T3H4** | 05/12–06/12 | Hưng (20235737) | Kiểm thử Đa Trận Độc lập, Ghi Log & Nâng cấp Client CLI | **HOÀN THÀNH (100%)** | 4 tài khoản chơi 2 trận song song không rò rỉ sự kiện (`TC-T4-01`); trận 1 xong không ảnh hưởng trận 2 (`TC-T4-02`); tự động sinh file log cấu trúc `logs/game_<id>.log` (`TC-T4-03`); Client CLI với 13 lệnh tương tác và bàn cờ ASCII 15x15 (`bin/caro_client`). |

---

## 2. CHI TIẾT KẾT QUẢ KỸ THUẬT

### 2.1. Phân hệ Lưu trữ SQLite & Xác thực Bảo mật (Hoàng Nam)
1. **Schema SQLite (`schema.sql`)**:
   - `users`: `id` (AUTOINCREMENT), `username` (UNIQUE COLLATE NOCASE), `salt` (CHAR(32)), `password_hash` (CHAR(64)), `points`, `wins`, `draws`, `losses`, `created_at`.
   - `games`: `id`, `player_x_id`, `player_o_id`, `outcome` ('WIN_X', 'WIN_O', 'DRAW', 'ONGOING', 'INTERRUPTED'), `reason` ('FIVE_IN_ROW', 'OVERLINE', 'BOARD_FULL', 'MUTUAL_DRAW', 'RESIGN', 'DISCONNECT', 'SERVER_CRASH'), `winner_id`, `start_time`, `end_time`.
   - `moves`: `game_id`, `seq`, `user_id`, `stone`, `row`, `col`, `created_at`; khóa chính phức hợp `(game_id, seq)` và ràng buộc UNIQUE `(game_id, row, col)`.
   - Khôi phục ván đấu gián đoạn (`storage_recover_crashed_games`): Khi server khởi động lại, tự động chuyển các trận `ONGOING` thành `INTERRUPTED` với lý do `SERVER_CRASH`.
2. **Mật mã & Xác thực (`sha256.c`, `auth.c`)**:
   - Tự hiện thực thuật toán băm SHA-256 thuần C11 theo chuẩn FIPS 180-4, không phụ thuộc OpenSSL.
   - Sinh chuỗi muối (salt) ngẫu nhiên 16 bytes (32 ký tự hex) từ `/dev/urandom`.
   - So sánh hash bằng thuật toán so sánh chuỗi hằng số thời gian (*constant-time equality check*) `auth_verify_password()` để chống tấn công kênh kề (*timing attack*).
3. **Async DB Worker Queue & Phòng thủ Socket FD**:
   - Luồng ngầm độc lập chạy vòng lặp xử lý truy vấn SQLite, giúp luồng mạng chính (`poll()`) không bao giờ bị nghẽn đĩa.
   - Giao tiếp hai chiều an toàn: Hàng đợi vòng tròn có giới hạn 64 phần tử (`pthread_mutex_t`, `pthread_cond_t`), thông báo hoàn tất về luồng chính thông qua `self-pipe` không đồng bộ.
   - Chống race condition tái sử dụng FD: Mỗi kết nối được cấp một số thứ tự sinh tăng đơn điệu `generation`. Khi worker DB hoàn thành, nếu `generation` trong kết quả không khớp với `generation` hiện tại của socket slot, kết quả sẽ bị hủy bỏ an toàn.
   - Khóa brute-force: Bảng `auth_throttle_table` tự động khóa 30 giây đối với IP/tài khoản sai liên tiếp 5 lần.

### 2.2. Phân hệ Quản lý Trận đấu, Nước cờ & Client CLI (Nhật Hưng)
1. **Cơ chế Thách đấu & Trận cờ trong RAM (`server.c`, `server.h`)**:
   - Quản lý đồng thời tối đa 32 ván cờ song song (`CARO_MAX_GAMES`).
   - Lời mời thách đấu có ID tăng dần, khóa trạng thái cả người gửi và người nhận vào `STATE_IN_INVITE`.
   - Vòng lặp `poll()` kiểm tra timeout định kỳ: nếu quá 30 giây không phản hồi, lời mời tự hủy, hoàn trả trạng thái về sảnh chờ (`STATE_IN_LOBBY`) và gửi thông báo lỗi `ERR 302`.
2. **Thẩm định Nước đi 6 bước chặt chẽ (Server-Side Move Validation)**:
   - Bước 1: Trận đấu tồn tại và đang ở trạng thái `active`.
   - Bước 2: Người gửi gói tin phải là một trong hai người chơi (Player X hoặc Player O) của ván cờ.
   - Bước 3: Đúng lượt đánh (Player X đánh lượt cờ lẻ `seq` 1, 3, 5...; Player O đánh lượt chẵn 2, 4, 6...).
   - Bước 4: Số thứ tự nước đi `seq` của gói tin gửi lên phải đúng bằng `game->current_seq + 1`.
   - Bước 5: Tọa độ hàng và cột phải nằm trong đoạn $[0..14]$ của bàn cờ $15 \times 15$.
   - Bước 6: Ô cờ chưa bị chiếm đóng và thỏa mãn các quy tắc cờ Caro thuần C (`game_place_stone`).
   - Nếu vi phạm bất kỳ bước nào: Server từ chối ngay lập tức với mã lỗi tương ứng (`ERR 400`, `ERR 401`, `ERR 402`, `ERR 403`).
3. **Nhật ký Trận đấu Tự động (`logs/game_<id>.log`)**:
   - Khởi tạo file log khi trận bắt đầu: ghi thông tin 2 đối thủ, User ID, loại quân cờ.
   - Mỗi nước đi hợp lệ: ghi nhận `MOVE <seq> <stone> <user_id> (<username>) at row=<r>, col=<c>`.
   - Khi trận kết thúc: ghi nhận lý do thắng/thua/hòa, ID người thắng cuộc và tổng số nước đã đánh.
4. **Giao diện Dòng lệnh Tương tác Client (`src/client/main.c`)**:
   - Sử dụng `poll()` để ghép kênh đồng thời giữa bàn phím (`stdin`) và socket mạng; không bao giờ bị đơ giao diện khi chờ server.
   - Tích hợp 13 câu lệnh dòng lệnh: `REGISTER`, `LOGIN`, `ONLINE`, `CHALLENGE`, `ACCEPT`, `DECLINE`, `MOVE`, `DRAW`, `ACCEPT_DRAW`, `DECLINE_DRAW`, `RESIGN`, `QUIT`, `HELP`.
   - Hàm `client_board_render()` vẽ trực quan bàn cờ Caro $15 \times 15$ dạng ASCII với chỉ số hàng/cột, đánh dấu rõ ràng quân `X`, `O` và ô trống `.`.

---

## 3. BẢNG TỔNG HỢP KẾT QUẢ KIỂM THỬ ĐỘC LẬP (E2E TEST SUITE)

Bộ kịch bản kiểm thử [tests/test_g3_suite.py](file:///d:/HUST/TH_LTM/prj/tests/test_g3_suite.py) (28 test cases) được chạy độc lập trên môi trường Linux/WSL:

| Nhóm Kiểm Thử | Mã Test | Tên Ca Kiểm Thử | Kết Quả | Thời Gian |
|---|:---:|---|:---:|:---:|
| **Tier 1: Chức Năng Cơ Bản** | `TC-T1-01` | Đăng ký tài khoản hợp lệ | **PASS** | 6.7ms |
| | `TC-T1-02` | Đăng nhập tài khoản và gắn phiên làm việc | **PASS** | 6.7ms |
| | `TC-T1-03` | Gửi và nhận lời mời thách đấu (`CHALLENGE`) | **PASS** | 54.3ms |
| | `TC-T1-04` | Chấp nhận thách đấu, cấp phát bàn cờ, phát `GAME_START` | **PASS** | 115.0ms |
| | `TC-T1-05` | Nước đi hợp lệ trả về `MOVE_ACK` và `MOVE_BROADCAST` | **PASS** | 195.3ms |
| | `TC-T1-06` | Thắng 5 quân liên tiếp kết thúc trận với `WIN` | **PASS** | 341.6ms |
| | `TC-T1-07` | Đầu hàng (`RESIGN`) xử thua ngay lập tức | **PASS** | 83.9ms |
| | `TC-T1-08` | Đề nghị hòa và đồng ý hòa (`DRAW`) kết thúc trận `DRAW` | **PASS** | 82.4ms |
| **Tier 2: Ca Biên & Phòng Thủ** | `TC-T2-01` | Từ chối đăng nhập khi sai thông tin (`ERR 201`) | **PASS** | 12.6ms |
| | `TC-T2-02` | Từ chối đăng ký tên người dùng đã tồn tại (`ERR 202`) | **PASS** | 6.9ms |
| | `TC-T2-03` | Chặn đăng nhập trùng lặp cùng 1 tài khoản (`ERR 203`) | **PASS** | 22.0ms |
| | `TC-T2-04` | Khóa 30 giây khi gõ sai mật khẩu 5 lần liên tiếp | **PASS** | 20.0ms |
| | `TC-T2-05` | Hết hạn lời mời sau timeout 30s giải phóng người chơi (`ERR 302`) | **PASS** | 926.1ms |
| | `TC-T2-06` | Từ chối người thứ ba trả lời lời mời (`ERR 303`) | **PASS** | 89.0ms |
| | `TC-T2-07` | Từ chối thách đấu người đang bận / đang trong trận (`ERR 301`) | **PASS** | 83.9ms |
| | `TC-T2-08` | Từ chối nước đi sai lượt (`ERR 401`) | **PASS** | 117.6ms |
| | `TC-T2-09` | Từ chối đánh đè lên ô đã có quân (`ERR 402`) | **PASS** | 154.6ms |
| | `TC-T2-10` | Từ chối tọa độ ngoài bàn cờ $> 14$ (`ERR 402` / `ERR 102`) | **PASS** | 139.3ms |
| | `TC-T2-11` | Từ chối số thứ tự nước đi không khớp (`ERR 403`) | **PASS** | 112.7ms |
| | `TC-T2-12` | Từ chối can thiệp nước đi vào trận của người khác (`ERR 400`) | **PASS** | 17.3ms |
| **Tier 3: Tương Tác Phức Hợp** | `TC-T3-01` | Từ chối thách đấu khi đang trong ván cờ active | **PASS** | 77.1ms |
| | `TC-T3-02` | Ngắt kết nối đột ngột: xử đối thủ thắng trận do forfeit | **PASS** | 125.8ms |
| | `TC-T3-03` | Hai người cùng gửi thách đấu nhau được xử lý an toàn | **PASS** | 64.0ms |
| | `TC-T3-04` | Người mời ngắt kết nối: tự động giải phóng người được mời | **PASS** | 367.5ms |
| **Tier 4: Kịch Bản Thực Tế** | `TC-T4-01` | **4 tài khoản chơi 2 ván cờ song song độc lập hoàn toàn** | **PASS** | 199.4ms |
| | `TC-T4-02` | Trận 1 kết thúc sớm không gây gián đoạn Trận 2 | **PASS** | 63.5ms |
| | `TC-T4-03` | Xác nhận file log từng trận có cấu trúc trên ổ đĩa (`logs/game_<id>.log`) | **PASS** | 16.0ms |
| | `TC-T4-04` | Thu hồi slot bàn cờ và mời tái đấu chéo cặp đấu thành công | **PASS** | 62.8ms |

### 📊 Thống kê Kiểm thử Toàn Dự án:
- **C Unit Tests**: **408 / 408 tests PASS (100%)**
  - Protocol (`bin/test_protocol`): 109 tests
  - Rules (`bin/test_game`): 134 tests
  - Storage SQLite (`bin/test_storage`): 113 tests
  - Auth & Crypto SHA-256 (`bin/test_auth`): 52 tests
- **Client Self-Tests** (`./bin/caro_client --test`): **9 / 9 tests PASS (100%)**
- **Milestone G2 Concurrency & Framing** (`tests/test_g2.sh`): **26 / 26 suites PASS (100%)**
- **Milestone G3 4-Tier Automated E2E** (`tests/test_g3.sh`): **28 / 28 test cases PASS (100%)**
- **Cảnh báo trình biên dịch (Compiler Warnings)**: **0 warnings** với cờ `-Wall -Wextra -std=c11 -pedantic`.
- **Rò rỉ tài nguyên (Resource / FD Leaks)**: **0 leaks**, toàn bộ socket và file descriptor đều trở về baseline sau khi test hoàn tất.

---

## 4. HƯỚNG DẪN THỰC THI & CHẠY THỬ NGHIỆM

### 4.1. Biên dịch và Chạy Kiểm thử Tự động
Trên môi trường Linux / WSL:
```bash
# 1. Biên dịch và chạy toàn bộ 408 unit tests:
make clean && make all && make test

# 2. Chạy tự kiểm thử Client:
./bin/caro_client --test

# 3. Chạy bài kiểm tra tải đồng thời Tuần 2:
bash tests/test_g2.sh

# 4. Chạy toàn bộ 28 kịch bản kiểm thử E2E Tuần 3:
bash tests/test_g3.sh --invite-timeout 1.0
```

Trên môi trường Windows (PowerShell / Command Prompt):
```bat
build.bat
run_test.bat
```

### 4.2. Chơi thử Trực tiếp giữa 2 Client
Mở 3 cửa sổ terminal riêng biệt:

- **Terminal 1 (Khởi động Server)**:
  ```bash
  ./bin/caro_server -p 8888
  ```

- **Terminal 2 (Người chơi 1 - Alice)**:
  ```bash
  ./bin/caro_client 127.0.0.1 8888
  # Trong client CLI:
  REGISTER alice pass123
  LOGIN alice pass123
  ONLINE
  CHALLENGE bob
  ```

- **Terminal 3 (Người chơi 2 - Bob)**:
  ```bash
  ./bin/caro_client 127.0.0.1 8888
  # Trong client CLI:
  REGISTER bob pass456
  LOGIN bob pass456
  # Nhận được thông báo thách đấu có invite_id, ví dụ invite_id=1:
  ACCEPT 1
  ```

- **Tiến hành đánh cờ**:
  - Alice (quân X, đánh trước): `MOVE 7 7`
  - Bob (quân O, đánh sau): `MOVE 7 8`
  - Bàn cờ ASCII sẽ tự động cập nhật ngay trên terminal của cả hai người chơi sau mỗi nước đánh!

---

## 5. KẾT LUẬN & ĐÁNH GIÁ
Nhiệm vụ **Tuần 3 (Mốc G3)** đã được 2 thành viên **Trịnh Ngọc Hoàng Nam** và **Đào Nhật Hưng** hoàn thành xuất sắc, đáp ứng 100% các tiêu chí nghiệm thu đề ra. Kiến trúc hệ thống phân tầng rõ ràng (Storage tách rời qua Worker Queue, Engine luật cờ độc lập, Server Multiplexing không block, Client tương tác trực quan) bảo đảm tính ổn định, bảo mật và sẵn sàng mở rộng cho các tính năng Xếp hạng (ELO) và Ghép trận tự động của Tuần 4.
