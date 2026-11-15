# ĐẶC TẢ GIAO THỨC TẦNG ỨNG DỤNG CỜ CARO TRỰC TUYẾN

**Dự án:** Cờ Caro trực tuyến (C/Linux)  
**Tác giả:** Đào Nhật Hưng (MSSV: 20235737)  
**Phản biện & Đồng thuận:** Trịnh Ngọc Hoàng Nam (MSSV: 20235793)  
**Mốc tài liệu:** T1H2 (Tuần 1 — 17–18/11/2026)  
**Phiên bản:** 1.0 (Bản chốt cơ sở cho Tuần 1 và mở rộng đến Tuần 6)  

---

## 1. ĐỊNH DẠNG KHUNG NHỊ PHÂN (BINARY FRAMING SPECIFICATION)

### 1.1. Cấu trúc tổng thể của Khung (Frame Layout)
Mọi thông điệp truyền trên kênh TCP giữa Client và Server bắt buộc phải tuân theo cấu trúc đóng khung nhị phân chuẩn hóa:

```
+------------------------------------+-------------------------------------------+
| Field                              | Size / Type                               |
+------------------------------------+-------------------------------------------+
| Length Header                      | 4 bytes (uint32_t, Big-Endian / Network)  |
| Payload Body                       | N bytes (N = Length Header, ASCII/UTF-8)  |
+------------------------------------+-------------------------------------------+
```

### 1.2. Quy tắc Length Header
- **Kích thước:** Cố định **4 byte** (`sizeof(uint32_t)`).
- **Thứ tự byte:** Bắt buộc sử dụng **Network Byte Order (Big-Endian)**.
  - Bên gửi phải chuyển đổi qua hàm `htonl()` trước khi ghi vào socket.
  - Bên nhận phải chuyển đổi qua hàm `ntohl()` sau khi đọc từ socket.
- **Phạm vi tính toán:** Giá trị độ dài **CHỈ TÍNH RIÊNG PHẦN PAYLOAD**, **KHÔNG BAO GỒM 4 BYTE HEADER**.
  - Ví dụ: Payload là `PING\thello\n` (11 bytes ký tự) thì giá trị Header là `11` (biểu diễn nhị phân trên dây: `0x00 0x00 0x00 0x0B`).
  - Tổng số byte thực tế truyền qua kết nối TCP cho gói tin này là: `4 + 11 = 15 bytes`.

### 1.3. Giới hạn kích thước và Bảo vệ chống tấn công (Limits & Safety)
- **Giới hạn Payload tiêu chuẩn (`MAX_PAYLOAD_SIZE`):** **4096 bytes** (4 KB).
  - Đây là kích thước tối đa cho mọi bản tin điều khiển, nước đi và thông báo trạng thái thông thường.
- **Giới hạn Bộ đệm tối đa (`MAX_FRAME_BUFFER`):** **65536 bytes** (64 KB).
  - Dành riêng cho các bản tin lớn ở Tuần 4–5 như tải toàn bộ nhật ký ván đấu hoặc danh sách bảng xếp hạng đầy đủ.
- **Xử lý vi phạm:**
  - Nếu bên nhận giải mã được giá trị Header `payload_len == 0`: Coi là frame rỗng không hợp lệ, ngắt kết nối.
  - Nếu `payload_len > MAX_FRAME_BUFFER`: Xác định đây là dữ liệu rác, lỗi byte order hoặc hành vi tấn công từ chối dịch vụ (Buffer Overflow / Memory Exhaustion Attack). Bên nhận **ngay lập tức đóng kết nối TCP (`close(fd)`)** và giải phóng bộ đệm liên quan mà không cần chờ đọc tiếp.

---

## 2. QUY ƯỚC ĐỊNH DẠNG PAYLOAD (PAYLOAD CONVENTIONS)

### 2.1. Bộ ký tự và Dấu phân tách trường
- Payload sử dụng định dạng văn bản mã hóa **UTF-8 / ASCII**.
- Các trường trong một thông điệp được phân tách duy nhất bằng ký tự **TAB (`\t` - ASCII 0x09)**.
- Kết thúc mỗi payload bắt buộc phải có một ký tự **xuống dòng (`\n` - ASCII 0x0A)**.
- **Không sử dụng khoảng trắng (` `) làm ký tự phân tách trường.** Khoảng trắng có thể xuất hiện bên trong giá trị của trường văn bản tự do (như tin nhắn báo lỗi hoặc tên hiển thị).

### 2.2. Cấu trúc tổng quát của Payload
```
<COMMAND>\t<ARG1>\t<ARG2>\t...\t<ARGN>\n
```

- `<COMMAND>`: Tên lệnh viết hoa toàn bộ (ví dụ: `PING`, `PONG`, `LOGIN`, `MOVE`, `ERR`).
- `<ARGx>`: Các tham số của lệnh tương ứng.

---

## 3. THIẾT KẾ CÁC TRƯỜNG MỞ RỘNG (EXTENDED FIELDS)

Để phục vụ mở rộng xuyên suốt từ Tuần 2 đến Tuần 6 mà không phá vỡ tính tương thích ngược, ba trường định danh cốt lõi được chuẩn hóa:

### 3.1. Trường `request_id` (Định danh yêu cầu)
- **Kiểu dữ liệu:** Số nguyên dương không dấu 32-bit (`uint32_t`, biểu diễn dạng chuỗi số thập phân trong payload, ví dụ: `1001`).
- **Mục đích:**
  - Trong mô hình I/O không chặn (non-blocking) và xử lý worker bất đồng bộ, các phản hồi từ Server không nhất thiết phải về theo thứ tự tuần tự tuyệt đối nếu có sự kiện hệ thống khác xen vào (như lời mời thách đấu từ người khác đẩy xuống).
  - Client gắn `request_id` tăng dần cho mỗi yêu cầu phát ra (`REGISTER`, `LOGIN`, `GET_ONLINE_LIST`, `CHALLENGE`).
  - Server khi gửi phản hồi `OK` hoặc `ERR` bắt buộc phải trả lại đúng `request_id` nhận được để Client CLI ghép nối chính xác phản hồi với tác vụ mà người dùng vừa thực hiện.

### 3.2. Trường `game_id` (Định danh ván cờ)
- **Kiểu dữ liệu:** Số nguyên dương không dấu 32-bit (`uint32_t`, biểu diễn chuỗi số thập phân, ví dụ: `42`).
- **Mục đích:**
  - Server hỗ trợ đồng thời ít nhất 10 ván cờ độc lập (`game_id = 1, 2, ...`).
  - Mọi bản tin liên quan đến trận đấu (`GAME_START`, `MOVE`, `MOVE_ACK`, `MOVE_BROADCAST`, `RESIGN`, `OFFER_DRAW`, `GAME_OVER`) đều phải mang `game_id`.
  - Giúp Server cô lập hoàn toàn các trận đấu: kiểm tra người gửi gói tin có thực sự là một trong hai đấu thủ của `game_id` đó hay không; ngăn chặn việc người chơi gửi nhầm hoặc cố ý can thiệp vào trận cờ của bàn khác.

### 3.3. Trường `expected_seq` / `seq` (Số thứ tự nước đi)
- **Kiểu dữ liệu:** Số nguyên dương không dấu 32-bit (`uint32_t`, đếm từ 1 trở đi).
- **Mục đích:**
  - Đảm bảo tính nhất quán và chống đảo lộn thứ tự nước đi (Out-of-order detection): Nước đầu tiên của trận luôn có `seq = 1` (quân X), nước tiếp theo có `seq = 2` (quân O),..., tăng tuần tự.
  - Server duy trì biến `game.expected_seq`. Khi nhận được gói `MOVE`:
    - Nếu `packet.seq == game.expected_seq`: Chấp nhận, xử lý nước đi, tăng `game.expected_seq++`.
    - Nếu `packet.seq != game.expected_seq`: Server từ chối ngay lập tức với lỗi `ERR_SEQ_MISMATCH`, không áp dụng nước đi lên bàn cờ.
  - Ngăn ngừa lỗi gửi lặp (replay attack / duplicate packet) khi mạng chập chờn.

---

## 4. BẢNG MÃ LỖI CHUẨN HÓA (ERROR CODES)

Mọi phản hồi lỗi từ Server đều dùng cú pháp:
```
ERR\t<request_id_or_game_id>\t<code>\t<message>\n
```

| Mã số (Code) | Tên định danh (Constant) | Ý nghĩa chi tiết |
|---|---|---|
| `100` | `ERR_BAD_FRAME` | Khung nhị phân lỗi, độ dài sai hoặc thiếu `\n` cuối payload |
| `101` | `ERR_UNKNOWN_CMD` | Không nhận diện được tên lệnh |
| `102` | `ERR_INVALID_PARAM` | Thiếu trường hoặc tham số không đúng kiểu/định dạng |
| `103` | `ERR_PAYLOAD_TOO_LARGE` | Kích thước payload vượt quá ngưỡng cho phép |
| `200` | `ERR_UNAUTHORIZED` | Chưa đăng nhập nhưng cố gắng thực hiện hành động yêu cầu quyền |
| `201` | `ERR_AUTH_FAILED` | Sai tên đăng nhập hoặc mật khẩu |
| `202` | `ERR_USER_EXISTS` | Tên đăng nhập đã tồn tại khi đăng ký |
| `203` | `ERR_ALREADY_LOGGED_IN` | Tài khoản này đang được đăng nhập ở một phiên khác |
| `300` | `ERR_USER_NOT_FOUND` | Không tìm thấy người chơi đích (thách đấu / xem thông tin) |
| `301` | `ERR_USER_BUSY` | Đối thủ đang trong trận đấu hoặc đang bận |
| `302` | `ERR_INVITE_EXPIRED` | Lời mời thách đấu đã hết hạn (quá 30 giây) |
| `303` | `ERR_INVITE_INVALID` | Lời mời không tồn tại hoặc sai người phản hồi |
| `400` | `ERR_NOT_IN_GAME` | Gửi lệnh trận đấu khi không tham gia ván nào |
| `401` | `ERR_NOT_YOUR_TURN` | Đi quân khi chưa đến lượt mình |
| `402` | `ERR_INVALID_MOVE` | Nước đi ngoài biên bàn cờ (0..14) hoặc ô đã có quân |
| `403` | `ERR_SEQ_MISMATCH` | Số thứ tự nước đi `seq` không khớp với `expected_seq` của Server |
| `404` | `ERR_GAME_NOT_FOUND` | `game_id` không tồn tại hoặc đã kết thúc |
| `500` | `ERR_INTERNAL_SERVER` | Lỗi nội bộ server (lỗi bộ nhớ, hàng đợi worker đầy) |
| `501` | `ERR_DATABASE` | Lỗi thao tác cơ sở dữ liệu SQLite |

---

## 5. TẬP LỆNH GIAO THỨC THEO CÁC MỐC TUẦN

### 5.1. Các thông điệp Tuần 1 (Nền tảng PING/PONG & ERR)

#### 1. Lệnh PING (Client -> Server hoặc Server -> Client)
- **Cú pháp:**
  ```
  PING\t<payload_str>\n
  ```
- **Ý nghĩa:** Kiểm tra độ thông suốt của kết nối. `<payload_str>` là chuỗi ký tự tùy ý (thường là timestamp hoặc nonce để đo độ trễ round-trip time).
- **Ví dụ Payload:** `PING\t1731770000\n`

#### 2. Lệnh PONG (Server -> Client hoặc Client -> Server)
- **Cú pháp:**
  ```
  PONG\t<payload_str>\n
  ```
- **Ý nghĩa:** Phản hồi lại thông điệp PING, trả lại chính xác `<payload_str>` đã nhận được.
- **Ví dụ Payload:** `PONG\t1731770000\n`

#### 3. Lệnh ERR (Báo lỗi)
- **Cú pháp:**
  ```
  ERR\t<code>\t<message>\n
  ```
  *(Tuần 1 chưa có `request_id`, sang Tuần 3 mở rộng thành `ERR\t<ref_id>\t<code>\t<message>\n`)*
- **Ví dụ Payload:** `ERR\t101\tUnknown command\n`

---

### 5.2. Các thông điệp dự kiến cho Tuần 3–6 (Đã quy hoạch trước)

#### 1. Xác thực tài khoản (Tuần 3)
- `REGISTER\t<req_id>\t<username>\t<password>\n`
- `LOGIN\t<req_id>\t<username>\t<password>\n`
- `OK\t<req_id>\t<action_code>\t<optional_data>\n`

#### 2. Hiện diện và Thách đấu (Tuần 3)
- `GET_ONLINE\t<req_id>\n` -> `ONLINE_LIST\t<req_id>\t<count>\t<user1:status>\t...\n`
- `CHALLENGE\t<req_id>\t<target_user_id>\n`
- `CHALLENGE_REQ\t<invite_id>\t<challenger_id>\t<challenger_name>\n`
- `CHALLENGE_RESP\t<invite_id>\t<ACCEPT|DECLINE>\n`

#### 3. Diễn biến trận đấu (Tuần 3 & 4)
- `GAME_START\t<game_id>\t<your_piece:X|O>\t<opponent_name>\n`
- `MOVE\t<game_id>\t<seq>\t<x>\t<y>\n`
- `MOVE_ACK\t<game_id>\t<seq>\t<x>\t<y>\n` (Gửi cho người vừa đi)
- `MOVE_BROADCAST\t<game_id>\t<seq>\t<x>\t<y>\n` (Gửi cho đối thủ)
- `RESIGN\t<game_id>\n`
- `OFFER_DRAW\t<game_id>\n`
- `DRAW_RESP\t<game_id>\t<ACCEPT|DECLINE>\n`
- `GAME_OVER\t<game_id>\t<reason:WIN|DRAW|RESIGN|DISCONNECT>\t<winner_name>\t<score_p1>\t<score_p2>\n`

#### 4. Bảng xếp hạng và Nhật ký ván (Tuần 4)
- `GET_LEADERBOARD\t<req_id>\t<limit>\t<offset>\n`
- `GET_GAME_LOG\t<req_id>\t<game_id>\n`

---

## 6. VÍ DỤ MINH HỌA TRÊN ĐƯỜNG TRUYỀN (WIRE FORMAT EXAMPLES)

### Ví dụ 1: Gói tin PING với payload `PING\t1234\n`
- **Payload:** `PING\t1234\n`
  - Ký tự: `'P'`, `'I'`, `'N'`, `'G'`, `'\t'`, `'1'`, `'2'`, `'3'`, `'4'`, `'\n'`
  - Chiều dài: 10 bytes.
- **Header:**
  - Giá trị: `10`
  - Biểu diễn nhị phân Big-Endian (4 bytes): `0x00 0x00 0x00 0x0A`
- **Dữ liệu thô trên dây (14 bytes):**
  ```
  00 00 00 0a 50 49 4e 47 09 31 32 33 34 0a
  ```

### Ví dụ 2: Gói tin PONG phản hồi `PONG\t1234\n`
- **Payload:** `PONG\t1234\n` (10 bytes)
- **Header:** `0x00 0x00 0x00 0x0A`
- **Dữ liệu thô trên dây (14 bytes):**
  ```
  00 00 00 0a 50 4f 4e 47 09 31 32 33 34 0a
  ```

### Ví dụ 3: Gói tin báo lỗi `ERR\t101\tUnknown command\n`
- **Payload:** `ERR\t101\tUnknown command\n` (24 bytes)
- **Header:** `0x00 0x00 0x00 0x18`
- **Dữ liệu thô trên dây (28 bytes):**
  ```
  00 00 00 18 45 52 52 09 31 30 31 09 55 6e 6b 6e 6f 77 6e 20 63 6f 6d 6d 61 6e 64 0a
  ```

---

## 7. THUẬT TOÁN ĐÓNG GÓI VÀ GIẢI ĐÓNG KHUNG (PACKING & UNPACKING)

### 7.1. Đóng gói khung (Packing Algorithm)
```c
int protocol_pack_frame(const char *payload, size_t payload_len, uint8_t *out_buf, size_t *out_len);
```
1. Kiểm tra `payload_len == 0` hoặc `payload_len > MAX_PAYLOAD_SIZE` -> Báo lỗi `ERR_INVALID_PARAM`.
2. Kiểm tra `payload[payload_len - 1] != '\n'` -> Nếu thiếu ký tự dòng mới thì tự động bổ sung hoặc báo lỗi.
3. Chuyển đổi `uint32_t len_be = htonl((uint32_t)payload_len);`.
4. Copy 4 byte `len_be` vào `out_buf[0..3]`.
5. Copy `payload_len` bytes từ `payload` vào `out_buf[4 .. 4 + payload_len - 1]`.
6. Trả về `*out_len = 4 + payload_len`.

### 7.2. Giải đóng khung từ bộ đệm tích lũy (Unpacking Algorithm)
```c
int protocol_unpack_frame(const uint8_t *in_buf, size_t in_len, char *out_payload, size_t *out_payload_len, size_t *consumed_bytes);
```
Hàm này giải quyết bài toán:
- **Dữ liệu chưa đủ Header (< 4 bytes):** Trả về mã chờ dữ liệu `PROTO_NEED_MORE_DATA`.
- **Dữ liệu chưa đủ Payload (< 4 + payload_len):** Trả về mã chờ dữ liệu `PROTO_NEED_MORE_DATA`.
- **Đã đủ ít nhất 1 khung hoàn chỉnh:**
  1. Trích xuất `payload_len = ntohl(*(uint32_t*)in_buf)`.
  2. Sao chép `payload_len` bytes vào `out_payload`, thêm ký tự kết thúc chuỗi `'\0'`.
  3. Gán `*consumed_bytes = 4 + payload_len`.
  4. Trả về mã thành công `PROTO_OK`.
  5. Bên gọi dịch chuyển dữ liệu còn dư trong `in_buf` lên đầu qua `memmove()` và tiếp tục lặp giải khung tiếp theo nếu có (xử lý coalesced frames).
