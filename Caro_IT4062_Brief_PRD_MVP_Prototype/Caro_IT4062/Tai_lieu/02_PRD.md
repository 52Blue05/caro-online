# PRD — Cờ Caro trực tuyến v0.1

**Trạng thái:** Đặc tả đề xuất trước khi code. Các quy ước cụ thể bên dưới dùng để hai phần client và server triển khai thống nhất.

## User Flow
Đăng ký nếu chưa có tài khoản → Đăng nhập → Sảnh → Chọn đối thủ rảnh → Gửi lời mời → Đối thủ chấp nhận → Vào ván → Đánh theo lượt → Xem kết quả, nhật ký → Về sảnh hoặc xem xếp hạng.

- Từ chối/hết hạn: hai bên về luồng sảnh; không tạo ván.
- Nước sai: chỉ người gửi nhận lỗi; bàn cờ và lượt không đổi.
- Mất kết nối: server xử lý ván bị ảnh hưởng; các ván khác tiếp tục.

## Yêu cầu chức năng

| Mã | Yêu cầu |
|---|---|
| FR-01 | Đăng ký, đăng nhập và chỉ cho một phiên online trên mỗi tài khoản. |
| FR-02 | Cập nhật danh sách online và trạng thái rảnh/đang chơi. |
| FR-03 | Tạo, chấp nhận, từ chối lời mời; hết hạn sau 30 giây. Chỉ tạo ván nếu cả hai vẫn rảnh. |
| FR-04 | Tạo ván với mã riêng; phân X/O; hai người nhận cùng trạng thái ban đầu. |
| FR-05 | Kiểm tra nước đi theo phiên, thành viên ván, trạng thái, lượt, tọa độ và ô trống; đồng bộ hai client. |
| FR-06 | Xử lý thắng, hòa, đầu hàng và ngắt kết nối; gửi kết quả, lịch sử và trả người còn online về sảnh. |
| FR-07 | Lưu kết quả và điểm trong một giao dịch; đọc bảng xếp hạng và thống kê thắng/hòa/thua. |
| FR-08 | Duy trì nhiều ván độc lập trên cùng một server. |

## Luật nghiệp vụ và trạng thái
- Bàn 15 × 15; tọa độ giao thức từ 1 đến 15; X đi trước. Ít nhất 5 quân liên tiếp theo một trong bốn hướng là thắng, không xét chặn hai đầu. Bàn đầy mà không thắng là hòa.
- Thắng +3, hòa +1 mỗi người, thua +0; ván hủy không cộng điểm hay tính thắng/hòa/thua.
- Phiên: `GUEST → IDLE → PLAYING → IDLE`; ngắt kết nối thì xóa phiên. Lời mời: `PENDING → ACCEPTED / DECLINED / EXPIRED / CANCELLED`.
- Ván: `ACTIVE → FINISHED / CANCELLED`. Đề xuất khi phát hiện một người ngắt mạng: khóa nước đi và chờ 3 giây trước khi chốt. Nếu phát hiện cả hai ngắt trong khoảng này, hủy; nếu chỉ một người ngắt, người còn lại thắng. Không hỗ trợ nối lại ván. Thời hạn tính theo lúc server phát hiện.
- Nếu server khởi động lại, bản ghi còn `ACTIVE` chuyển `CANCELLED`; không phục hồi bàn cờ trong RAM.

## Giao thức mạng đề xuất
Mỗi khung: **4 byte số nguyên không dấu theo thứ tự byte mạng + nội dung JSON UTF-8**. Độ dài tính theo byte nội dung, hợp lệ từ 1 đến 65.536. Mỗi nội dung có trường `type`. Yêu cầu trực tiếp có `request_id`; phản hồi trực tiếp lặp lại mã đó. Thông báo tự đẩy có mã ván/lời mời nếu liên quan. Ví dụ sau khi tách khung:

```json
{"type":"MOVE","request_id":18,"match_id":42,"row":8,"col":9}
{"type":"MOVE_ACCEPTED","match_id":42,"seq":7,"row":8,"col":9,"stone":"X","next_turn":"O"}
{"type":"ERROR","request_id":18,"code":"NOT_YOUR_TURN"}
```

| Client → Server | Server → Client | Dữ liệu chính |
|---|---|---|
| REGISTER, LOGIN | REGISTER_OK, LOGIN_OK hoặc ERROR | Tên tài khoản, mật khẩu; server gắn danh tính với socket |
| — | ONLINE_LIST | Tên người chơi và trạng thái |
| CHALLENGE | CHALLENGE_SENT, CHALLENGE_NOTIFY | Người nhận, mã lời mời, hạn phản hồi |
| CHALLENGE_REPLY | CHALLENGE_STATUS, GAME_START | Mã lời mời, quyết định; mã ván, hai người, quân, lượt đầu |
| MOVE | MOVE_ACCEPTED hoặc ERROR | Mã ván, hàng/cột; xác nhận gồm số thứ tự nước và lượt kế tiếp; lượt kế tiếp là null nếu kết thúc |
| RESIGN | GAME_END, GAME_LOG | Mã ván; kết quả, lý do, điểm thay đổi; nhật ký có thứ tự |
| RANKING_GET | RANKING_LIST | Điểm, số thắng/hòa/thua |

Server tích lũy byte theo từng kết nối, tách nhiều khung khi đủ, giữ phần thiếu cho lần đọc sau. Nếu độ dài sai hoặc vượt giới hạn thì đóng kết nối đó; nếu payload có thể đọc nhưng lệnh không hợp lệ thì trả ERROR. Mã lỗi đề xuất: `AUTH_REQUIRED`, `ALREADY_ONLINE`, `PLAYER_BUSY`, `INVITE_EXPIRED`, `NOT_IN_MATCH`, `NOT_YOUR_TURN`, `BAD_COORDINATE`, `CELL_OCCUPIED`. Kết quả và nước cuối được gửi theo thứ tự MOVE_ACCEPTED → GAME_END → GAME_LOG.

## Yêu cầu phi chức năng và nghiệm thu

| Mã | Tình huống | Kết quả mong đợi |
|---|---|---|
| AC-01 | Chia header/payload thành nhiều lần gửi; gộp hai khung | Tách đúng các lệnh theo thứ tự, không thực thi lệnh thiếu. |
| AC-02 | Cùng lúc có hai lời mời liên quan một người, lần lượt chấp nhận cả hai | Tạo tối đa một ván cho người đó. |
| AC-03 | Bốn client, hai ván xen kẽ; gửi mã ván không thuộc người gửi | Không gửi nhầm thông báo hoặc thay đổi bàn cờ khác; từ chối lệnh sai quyền. |
| AC-04 | Một client ngừng đọc hoặc mất kết nối | Các ván khác vẫn chơi; hàng đợi được giới hạn theo client. |
| AC-05 | Kết thúc ván rồi lặp lại sự kiện kết toán | Chỉ có một kết quả; người thắng tăng đúng 3 điểm. |
| AC-06 | Thử 20 client / 10 ván đồng thời | Cả 10 ván tiếp tục hợp lệ; lưu log kiểm thử, không tuyên bố đạt trước khi đo. |

Đích triển khai: C/POSIX trên Linux trong VirtualBox; giao diện dòng lệnh. Mật khẩu lưu dưới dạng hash có salt bằng thư viện phù hợp. TCP thuần chỉ dùng trong lab; không triển khai công khai phiên bản này.
