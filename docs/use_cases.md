# ĐẶC TẢ CA SỬ DỤNG VÀ PHẠM VI DỰ ÁN CỜ CARO TRỰC TUYẾN

**Dự án:** Cờ Caro trực tuyến đa người chơi (C/Linux - Non-blocking Socket & Poll)  
**Nhóm:** Nhóm 8  
**Thành viên:**  
- Trịnh Ngọc Hoàng Nam (MSSV: 20235793) — Phụ trách mạng, event loop, phiên, xác thực, SQLite, build  
- Đào Nhật Hưng (MSSV: 20235737) — Phụ trách giao thức ứng dụng, client CLI, luật Caro, điều phối ván/lời mời, xếp hạng/log, kiểm thử  
**Mốc tài liệu:** T1H1 (Tuần 1 — 16/11/2026)  
**Phiên bản:** 1.0  

---

## 1. TỔNG QUAN HỆ THỐNG VÀ PHẠM VI DỰ ÁN

### 1.1. Mục tiêu toàn dự án (6 tuần)
Hệ thống cờ Caro trực tuyến được xây dựng theo kiến trúc Client-Server sử dụng ngôn ngữ C trên nền tảng POSIX/Linux.
Server chịu trách nhiệm điều phối mạng tập trung:
- Phục vụ đồng thời ít nhất **20 client kết nối** và tổ chức **10 ván đấu độc lập** cùng thời điểm.
- Xử lý I/O dồn kênh bằng `poll()` với socket không chặn (non-blocking sockets) trên một luồng chính duy nhất (single-threaded event loop), kết hợp hàng đợi worker bất đồng bộ cho các tác vụ nặng (băm mật khẩu, đọc/ghi SQLite).
- Quản lý phiên, danh sách online, ghép trận, điều phối nước đi, kiểm tra tính hợp lệ và phân định thắng thua theo luật Caro chuẩn.
- Lưu trữ bền vững tài khoản, mật khẩu (băm kèm salt), thống kê điểm số (Elo/điểm tích lũy) và toàn bộ nhật ký từng nước đi của các ván đấu vào cơ sở dữ liệu SQLite.

Client là ứng dụng giao diện dòng lệnh (CLI):
- Kết nối tới Server qua giao thức TCP.
- Theo dõi đồng thời cả luồng dữ liệu từ Server và bàn phím người dùng (`stdin`) bằng non-blocking I/O / dồn kênh, không để thao tác nhập liệu của người dùng làm chặn việc tiếp nhận thông báo/sự kiện từ Server.
- Vẽ bàn cờ trực quan, hiển thị thông báo trạng thái, hỗ trợ luồng chơi đầy đủ.

### 1.2. Phân định phạm vi 6 tuần theo kế hoạch
Phạm vi dự án được chia theo các mốc nghiệm thu tuần nghiêm ngặt (G1 đến G6):

| Tuần | Thời gian | Mốc | Phạm vi triển khai | Ngoài phạm vi (Out of Scope) |
|---|---|---|---|---|
| **Tuần 1** | 16/11 – 22/11/2026 | **G1** | **Nền tảng mạng và giao thức tối thiểu:** Khởi tạo repository, Makefile; Server non-blocking + `poll()`; Client CLI dồn kênh stdin/socket; Thống nhất giao thức nhị phân framing 4-byte big-endian + TAB delimiter; Trao đổi PING/PONG giữa 2 client đồng thời; Xử lý mất kết nối cơ bản. | Chưa có tài khoản SQLite, chưa có luật Caro đầy đủ, chưa có danh sách online hay ghép ván. |
| **Tuần 2** | 23/11 – 29/11/2026 | **G2** | **Mạng ổn định và bộ luật Caro độc lập:** Hoàn thiện bộ đệm RX/TX tích lũy khung, xử lý partial frame/multiple frames; Triển khai module luật Caro 15×15 độc lập với mạng; Quản lý phiên tạm thời; Thử nghiệm tải 20 kết nối PING đồng thời. | Chưa có xác thực DB, chưa ghép trận. |
| **Tuần 3** | 30/11 – 06/12/2026 | **G3** | **Xác thực tài khoản và luồng chơi chính:** Schema SQLite (users/games/moves); Đăng ký/đăng nhập (băm mật khẩu có salt); Quản lý lời mời/thách đấu (hết hạn 30s); Khởi tạo ván và xử lý nước đi (MOVE) trong RAM; Kiểm thử 4 client chơi 2 ván độc lập. | Dữ liệu ván còn ở RAM, chưa kết toán bền vững vào DB. |
| **Tuần 4** | 07/12 – 13/12/2026 | **G4** | **Lưu trữ SQLite giao dịch và hoàn thiện tính năng:** Hàng đợi worker SQLite; Kết toán điểm giao dịch (Thắng +3, Hòa +1, Thua 0); Đồng bộ trạng thái chờ COMMIT; Xếp hạng và chia log ván; Kiểm thử 10 client / 5 trận trên bản đầy đủ. | Đóng băng tính năng mới từ sau ngày 13/12. |
| **Tuần 5** | 14/12 – 20/12/2026 | **G5** | **Kiểm thử tải cao và xử lý sự cố:** Kiểm chứng 20 client / 10 trận đồng thời trong ≥ 30 phút; Thử nghiệm tiêm lỗi (khung lỗi, peer chậm, rớt mạng giữa ván, tranh chấp trạng thái); Kiểm tra rò rỉ bộ nhớ bằng Valgrind/ASan; Tối ưu tài nguyên. | Không bổ sung thêm tính năng lớn. |
| **Tuần 6** | 21/12 – 27/12/2026 | **G6** | **Đóng gói và bảo vệ:** Build sạch trên máy mới; Hoàn thiện tài liệu kiến trúc, báo cáo kỹ thuật, slide; Diễn tập bảo vệ và demo các ca kiểm thử. | Hoàn thiện tài liệu và kiểm thử hồi quy. |

### 1.3. Các tính năng nằm ngoài phạm vi toàn dự án (Out of Scope)
Để tập trung vào chất lượng lõi và độ ổn định mạng theo yêu cầu kỹ thuật:
- AI đối thủ / chơi với máy: Ngoài phạm vi.
- Phòng chat tự do giữa các người chơi: Ngoài phạm vi (chỉ trao đổi thông điệp trò chơi và thông báo hệ thống).
- Giao diện đồ họa (GUI/Web): Ngoài phạm vi (chỉ dùng CLI dòng lệnh thuần túy).
- Khán giả xem ván đấu (Spectator mode): Ngoài phạm vi.
- Khôi phục ván đấu sau khi đứt kết nối mạng (Reconnect resumption): Nếu người chơi mất kết nối quá thời gian chờ, ván đấu xử thua/hủy theo quy tắc timeout đã định.

---

## 2. CHỐT BỘ LUẬT CỜ CARO (GOMOKU 15×15)

### 2.1. Kích thước bàn cờ và hệ tọa độ
- **Kích thước bàn cờ:** Cố định **15 × 15 ô vuông** (chuẩn Gomoku thi đấu quốc tế).
- **Hệ tọa độ giao thức:**
  - Trục ngang (X / Cột): Đánh số từ `0` đến `14` (từ trái qua phải).
  - Trục dọc (Y / Hàng): Đánh số từ `0` đến `14` (từ trên xuống dưới).
- **Hiển thị trên CLI người dùng:**
  - Cột: Các chữ cái `A` đến `O` (tương ứng `0` đến `14`).
  - Hàng: Các số từ `1` đến `15` (tương ứng `0` đến `14`).
  - Giao diện CLI hỗ trợ nhập theo dạng tọa độ cờ (`H8`, `A1`, `O15`) hoặc số (`7,7`).

### 2.2. Quân cờ và thứ tự đi
- Hai loại quân cờ:
  - Quân **X** (Người chơi 1 - Bên thách đấu hoặc đi trước).
  - Quân **O** (Người chơi 2 - Bên nhận lời thách đấu hoặc đi sau).
- **Nguyên tắc đi quân:**
  - Hai người chơi luân phiên đi từng nước (turn-based).
  - Người cầm quân **X** luôn đi trước (Nước thứ nhất: `turn = 1`, `seq = 1`).
  - Nước đi chỉ được đặt vào ô còn trống (`EMPTY`). Không được đặt đè lên ô đã có quân hoặc ô nằm ngoài biên bàn cờ (`0 <= x, y <= 14`).
  - Khi đã đặt quân, nước đi là cố định và không được phép rút lại (không hoàn cờ).

### 2.3. Điều kiện thắng cuộc (Win Condition)
- Một bên giành chiến thắng ngay lập tức khi tạo được một chuỗi **ít nhất 5 quân liên tiếp cùng màu** (`>= 5`) theo một trong 4 hướng:
  1. **Hàng ngang (Horizontal):** Các ô `(x, y), (x+1, y), (x+2, y), (x+3, y), (x+4, y)`.
  2. **Hàng dọc (Vertical):** Các ô `(x, y), (x, y+1), (x, y+2), (x, y+3), (x, y+4)`.
  3. **Đường chéo chính (Diagonal - Xuống phải):** Các ô `(x, y), (x+1, y+1), (x+2, y+2), (x+3, y+3), (x+4, y+4)`.
  4. **Đường chéo phụ (Anti-diagonal - Lên phải):** Các ô `(x, y), (x+1, y-1), (x+2, y-2), (x+3, y-3), (x+4, y-4)`.
- **Quy tắc Overline (Trường hợp >= 6 quân):**
  - Căn cứ theo tiêu chí kiểm thử của dự án (Checklist T2H4: *"Test thắng ngang/dọc/hai chéo, ≥6 quân"*): Chuỗi liên tiếp từ 5 quân trở lên (kể cả 6 quân liên tiếp trở lên do đối thủ sơ ý không chặn) **được công nhận thắng**.
- **Quy tắc chặn hai đầu (Open-ended rule):**
  - Dự án **KHÔNG áp dụng luật chặn 2 đầu** (tức là chuỗi 5 quân liên tiếp dù bị chặn một đầu hoặc cả hai đầu vẫn được tính là thắng, miễn là đạt đủ 5 quân cùng màu thẳng hàng). Điều này đảm bảo tính xác định, đơn giản và phù hợp với chuẩn gomoku tự do (Freestyle Gomoku) trên hệ thống phân tán.

### 2.4. Điều kiện hòa (Draw Condition)
Ván đấu kết thúc với kết quả HÒA khi:
1. **Hết ô cờ:** Toàn bộ 225 ô trên bàn cờ (`15 × 15 = 225`) đã được đi kín quân mà không bên nào đạt điều kiện thắng.
2. **Cầu hòa:** Một bên gửi yêu cầu xin hòa và đối thủ chấp thuận thông qua giao thức (hoặc cả hai bên đồng thuận kết thúc).

### 2.5. Đầu hàng và Mất kết nối (Surrender & Disconnection)
- **Đầu hàng (Surrender / Resign):** Người chơi có thể gửi thông điệp xin đầu hàng bất cứ lúc nào trong ván đấu. Đối thủ ngay lập tức được xử THẮNG, người đầu hàng bị xử THUA.
- **Mất kết nối / Bỏ cuộc (Disconnection Timeout):**
  - Nếu một client bị đứt kết nối socket hoặc không gửi phản hồi trong thời gian quy định (ví dụ 60 giây khi đang trong ván), Server sẽ xử bên đó bị THUA do bỏ cuộc (Forfeit/Abandon), bên còn lại được xử THẮNG.

---

## 3. DANH SÁCH TỔNG HỢP CÁC CA SỬ DỤNG (USE CASES)

Hệ thống bao gồm 7 nhóm ca sử dụng chính trải dài trong 6 tuần:

```
+-----------------------------------------------------------------------------------+
|                            HỆ THỐNG CỜ CARO TRỰC TUYẾN                           |
+-----------------------------------------------------------------------------------+
|  [Nhóm 1: Kết nối mạng & Giám sát kênh (Network Baseline)]                        |
|    - UC-01: Thiết lập kết nối TCP & Handshake PING/PONG                           |
|    - UC-02: Ngắt kết nối và giải phóng tài nguyên phiên                           |
|                                                                                   |
|  [Nhóm 2: Quản lý tài khoản & Xác thực (Account & Authentication)]                 |
|    - UC-03: Đăng ký tài khoản mới (Register)                                      |
|    - UC-04: Đăng nhập hệ thống (Login)                                            |
|    - UC-05: Đăng xuất (Logout)                                                    |
|                                                                                   |
|  [Nhóm 3: Hiện diện & Sảnh chờ (Lobby & Online Presence)]                         |
|    - UC-06: Xem danh sách người chơi trực tuyến (Online List)                     |
|    - UC-07: Cập nhật trạng thái người chơi (Available / In-Game / Busy)           |
|                                                                                   |
|  [Nhóm 4: Thách đấu & Thiết lập trận (Matchmaking & Challenge)]                   |
|    - UC-08: Gửi lời mời thách đấu (Challenge Request)                             |
|    - UC-09: Tiếp nhận và phản hồi thách đấu (Accept / Decline / Timeout)          |
|    - UC-10: Hủy lời mời thách đấu                                                 |
|                                                                                   |
|  [Nhóm 5: Diễn biến ván đấu (Gameplay Management)]                                |
|    - UC-11: Khởi tạo ván cờ (Game Start)                                         |
|    - UC-12: Đi quân theo lượt (Make Move)                                         |
|    - UC-13: Đồng bộ nước đi và kiểm tra tính hợp lệ                               |
|    - UC-14: Xin đầu hàng (Resign)                                                 |
|    - UC-15: Đề nghị hòa và phản hồi cầu hòa (Offer Draw)                          |
|    - UC-16: Phân định kết thúc ván (Game End: Win/Draw/Abandon)                   |
|                                                                                   |
|  [Nhóm 6: Lưu trữ, Xếp hạng & Nhật ký (Storage & Statistics)]                     |
|    - UC-17: Cập nhật điểm số và kết toán giao dịch (Transactional Scoring)        |
|    - UC-18: Xem bảng xếp hạng (Leaderboard)                                       |
|    - UC-19: Lưu và tải nhật ký ván đấu (Game Log & Replay)                        |
|                                                                                   |
|  [Nhóm 7: Xử lý sự cố & Biên mạng (Fault Tolerance & Edge Cases)]                 |
|    - UC-20: Xử lý khung dữ liệu cắt nhỏ / ghép mảnh (Partial/Coalesced Framing)    |
|    - UC-21: Xử lý rớt mạng bất thường trong ván đấu                              |
|    - UC-22: Xử lý client chậm hoặc gửi tin quá tải (Slowloris & Flooding)         |
+-----------------------------------------------------------------------------------+
```

---

## 4. ĐẶC TẢ CHI TIẾT CÁC CA SỬ DỤNG TIÊU BIỂU

### UC-01: Thiết lập kết nối TCP & Handshake PING/PONG (Trọng tâm Tuần 1)
- **Mã:** UC-01
- **Tác nhân:** Client CLI, Server Daemon
- **Mục đích:** Client kết nối tới cổng TCP của Server, gửi khung PING và nhận phản hồi PONG với payload nguyên vẹn để kiểm tra độ trễ và tính toàn vẹn của kênh truyền.
- **Tiền điều kiện:** Server đang lắng nghe trên cổng cấu hình (mặc định: `8888`).
- **Luồng chính:**
  1. Client khởi tạo socket, kết nối tới Server IP:Port.
  2. Server chấp nhận kết nối qua `accept()`, đưa socket vào chế độ không chặn (`O_NONBLOCK`) và đăng ký sự kiện `POLLIN` vào mảng `pollfd`.
  3. Client đóng gói frame PING: `[4-byte len = 10] PING\t12345\n` gửi qua mạng.
  4. Server đọc đủ 4 byte length, đọc tiếp 10 byte payload, tách command `PING` và token `12345`.
  5. Server phản hồi frame PONG: `[4-byte len = 10] PONG\t12345\n`.
  6. Client nhận frame PONG, bóc tách và in thông báo xác nhận kết nối thành công.
- **Luồng ngoại lệ:**
  - Socket gửi ngắt quãng: Server/Client đọc tích lũy vào buffer cho đến khi đủ frame, không crash hoặc drop dữ liệu.
  - Client đóng kết nối: Server phát hiện `recv() == 0`, dọn dẹp tài nguyên client.

### UC-02: Ngắt kết nối và giải phóng tài nguyên phiên (Trọng tâm Tuần 1 & Tuần 2)
- **Mã:** UC-02
- **Tác nhân:** Client CLI, Server Daemon
- **Mục đích:** Xử lý việc client chủ động thoát hoặc rớt mạng đột ngột mà không gây ảnh hưởng tới các kết nối khác trên Server.
- **Luồng chính:**
  1. Client đóng ứng dụng (gửi EOF hoặc tín hiệu đóng socket).
  2. Vòng lặp `poll()` của Server nhận sự kiện `POLLIN` với `recv() == 0` (hoặc `POLLHUP` / `POLLERR`).
  3. Server đóng file descriptor (`close(fd)`), đặt lại slot trạng thái trong danh sách client về rỗng (`UNUSED`).
  4. Giải phóng bộ đệm RX/TX chưa xử lý (nếu có).
  5. Nếu client đang ở trong ván đấu (từ Tuần 3), thông báo cho đối thủ và xử lý kết thúc ván do bỏ cuộc.
- **Hậu điều kiện:** Số lượng fd mở giảm về đúng giá trị ban đầu, không rò rỉ bộ nhớ hoặc tài nguyên hệ điều hành.

### UC-03: Đăng ký tài khoản (Tuần 3)
- **Mã:** UC-03
- **Tác nhân:** Người dùng, Client CLI, Auth Worker, SQLite DB
- **Mục đích:** Tạo tài khoản mới có tên đăng nhập và mật khẩu an toàn.
- **Luồng chính:**
  1. Người dùng nhập tên tài khoản (3-20 ký tự) và mật khẩu tại CLI.
  2. Client gửi: `REGISTER\t<req_id>\t<username>\t<password>\n`.
  3. Server tiếp nhận, kiểm tra định dạng hợp lệ.
  4. Đẩy tác vụ băm mật khẩu (kèm salt ngẫu nhiên) và lưu vào SQLite qua hàng đợi DB worker bất đồng bộ.
  5. DB worker hoàn thành, đẩy kết quả về event loop.
  6. Server trả lời: `OK\t<req_id>\tREGISTER_SUCCESS\n`.
- **Luồng ngoại lệ:**
  - Tên tài khoản đã tồn tại: Trả lời `ERR\t<req_id>\tERR_USER_EXISTS\tTen da ton tai\n`.

### UC-04: Đăng nhập hệ thống (Tuần 3)
- **Mã:** UC-04
- **Tác nhân:** Người dùng, Client CLI, Server
- **Mục đích:** Xác thực danh tính người dùng và chuyển trạng thái phiên sang `AUTHENTICATED`.
- **Luồng chính:**
  1. Người dùng nhập username và password.
  2. Client gửi: `LOGIN\t<req_id>\t<username>\t<password>\n`.
  3. Server xác thực thông tin tài khoản qua DB worker.
  4. Kiểm tra tài khoản không bị đăng nhập trùng lặp (nếu tài khoản đang online ở phiên khác thì từ chối).
  5. Cập nhật `session.user_id`, chuyển trạng thái sang `LOBBY_AVAILABLE`.
  6. Server trả về: `OK\t<req_id>\tLOGIN_SUCCESS\t<user_id>\t<score>\n`.
  7. Server phát thông báo cập nhật danh sách online tới các client khác.

### UC-08 & UC-09: Thách đấu và Phản hồi thách đấu (Tuần 3)
- **Mã:** UC-08, UC-09
- **Tác nhân:** Người thách đấu (Player A), Người nhận lời (Player B), Server
- **Mục đích:** Thiết lập trận đấu giữa hai người chơi tự nguyện.
- **Luồng chính:**
  1. Player A gửi: `CHALLENGE\t<req_id>\t<target_user_id>\n`.
  2. Server kiểm tra Player B đang ở trạng thái `AVAILABLE`.
  3. Server chuyển tạm thời trạng thái cả hai sang `INVITING`/`INVITED`, khởi động bộ đếm 30 giây.
  4. Server đẩy lời mời tới B: `CHALLENGE_REQ\t<invite_id>\t<A_user_id>\t<A_username>\n`.
  5. Player B gửi phản hồi chấp nhận: `CHALLENGE_RESP\t<invite_id>\tACCEPT\n`.
  6. Server khởi tạo Game mới (`game_id`), phân định A cầm quân `X` (đi trước), B cầm quân `O` (đi sau).
  7. Server gửi `GAME_START\t<game_id>\tX\t<B_username>\n` tới A và `GAME_START\t<game_id>\tO\t<A_username>\n` tới B.
- **Luồng ngoại lệ:**
  - B từ chối: B gửi `CHALLENGE_RESP\t<invite_id>\tDECLINE\n`. Server hoàn trả trạng thái `AVAILABLE` cho cả hai, thông báo từ chối cho A.
  - Quá 30 giây không trả lời: Hết hạn, tự động hủy lời mời.
  - B đang bận chơi với người khác: Báo lỗi ngay cho A.

### UC-12 & UC-13: Đi quân theo lượt và Đồng bộ nước đi (Tuần 3 & Tuần 4)
- **Mã:** UC-12, UC-13
- **Tác nhân:** Hai người chơi trong trận, Server
- **Mục đích:** Xử lý từng nước đi, xác minh quyền đi, thứ tự chuỗi `expected_seq`, luật hợp lệ và phát tán nước đi.
- **Luồng chính:**
  1. Đến lượt người chơi (ví dụ Player A quân X, lượt `seq = 1`), A nhập tọa độ `7,7`.
  2. Client A gửi: `MOVE\t<game_id>\t<seq=1>\t7\t7\n`.
  3. Server kiểm tra:
     - Đúng ván đấu (`game_id`) và người gửi đúng là người đến lượt.
     - Số thứ tự nước đi đúng với `game.expected_seq`.
     - Tọa độ `0 <= x, y <= 14` và ô `board[y][x]` đang trống.
  4. Server cập nhật `board[7][7] = 'X'`, tăng `game.expected_seq = 2`, chuyển lượt sang B.
  5. Server kiểm tra điều kiện thắng (theo UC-16). Nếu chưa kết thúc:
  6. Server gửi xác nhận cho A: `MOVE_ACK\t<game_id>\t1\t7\t7\n`.
  7. Server gửi broadcast cho B: `MOVE_BROADCAST\t<game_id>\t1\t7\t7\n`.
  8. Cả hai Client cập nhật bàn cờ hiển thị tại CLI.
- **Luồng ngoại lệ:**
  - Đi sai lượt / sai seq / ô đã có quân / tọa độ ngoài biên: Server gửi `ERR\t<game_id>\tERR_INVALID_MOVE\tNuoc di khong hop le\n` cho bên vi phạm; bàn cờ không bị thay đổi; lượt chơi giữ nguyên.

### UC-16 & UC-17: Kết thúc ván và Kết toán giao dịch (Tuần 3 & Tuần 4)
- **Mã:** UC-16, UC-17
- **Tác nhân:** Hai người chơi, Server Event Loop, SQLite DB Worker
- **Mục đích:** Phân định thắng/thua/hòa, lưu nhật ký nước đi và cập nhật điểm số bền vững một lần duy nhất.
- **Luồng chính:**
  1. Sau nước đi thứ `k` của Player A, hàm kiểm tra luật `check_win()` phát hiện 5 quân X liên tiếp.
  2. Server xác định ván đấu kết thúc: Player A THẮNG, Player B THUA.
  3. Server đánh dấu trạng thái ván `GAME_OVER_PENDING_COMMIT`.
  4. Server đẩy job giao dịch tới SQLite DB worker:
     - Ghi trạng thái ván: `WINNER = A, LOSER = B`.
     - Cộng điểm: A nhận +3 điểm, B nhận 0 điểm (Hòa: mỗi bên +1 điểm).
     - Lưu toàn bộ danh sách nước đi vào bảng `moves`.
  5. DB Worker thực hiện `BEGIN TRANSACTION ... COMMIT` an toàn.
  6. Nhận xác nhận COMMIT thành công, Server phát thông báo tới cả hai client:
     `GAME_OVER\t<game_id>\tWIN\t<A_username>\t<new_score_A>\t<new_score_B>\n`.
  7. Hai người chơi được trả về trạng thái `AVAILABLE`.

---

## 5. PHÂN RÃ NHIỆM VỤ TUẦN 1 (T1H1, T1H2, T1H3, T1H4)

Để đảm bảo đạt mốc **G1 (16–22/11/2026)**, phạm vi triển khai của Tuần 1 được chốt như sau:

| Mã việc | Thành viên | Bắt đầu - Hạn | Nội dung bàn giao | Phụ thuộc |
|---|---|---|---|---|
| **T1N1** | Nam | 16/11 | Tạo repository, cấu trúc thư mục, Makefile, .gitignore, build skeleton nền | Không |
| **T1H1** | Hưng | 16/11 | Chốt phạm vi, đặc tả ca sử dụng (`docs/use_cases.md`), luật 15×15, điều kiện thắng >=5 quân | Không |
| **T1N2** | Nam | 17–18/11 | Chốt cấu trúc dữ liệu Client/Game, buffer RX/TX, quy tắc dọn socket | T1N1, T1H1 |
| **T1H2** | Hưng | 17–18/11 | Đặc tả giao thức (`docs/protocol.md`), framing header 4 byte big-endian, TAB delimiter, mã hóa `include/protocol.h`, `src/common/protocol.c` | T1H1 |
| **T1N3** | Nam | 19–20/11 | Khung socket non-blocking, vòng lặp `poll()`, quản lý đa kết nối | T1N2, T1H2 |
| **T1H3** | Hưng | 19–20/11 | Client CLI C dồn kênh stdin & socket (không chờ nhập độc quyền) | T1H2 |
| **T1N4** | Nam | 21/11 | Tích hợp đường gửi nhận server, PING/PONG framing, xử lý ngắt/EAGAIN | T1N3, T1H3 |
| **T1H4** | Hưng | 21–22/11 | Kiểm thử tích hợp 2 client kết nối đồng thời, kiểm chứng ngắt kết nối, README & log nghiệm thu G1 | T1N4, T1H3 |

**Tiêu chí nghiệm thu G1 (22/11/2026):**
- Hai client C kết nối đồng thời tới server.
- Trao đổi thành công thông điệp PING/PONG theo đúng chuẩn framing nhị phân 4-byte length + TAB payload.
- Client vẫn nhận và hiển thị sự kiện từ Server khi người dùng chưa nhập liệu bàn phím.
- Đóng một client không làm ảnh hưởng đến client còn lại và không làm sập server.
- Server chấp nhận client mới kết nối sau khi client cũ ngắt kết nối.
