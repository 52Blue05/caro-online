# Project Brief — Cờ Caro trực tuyến

**Môn:** Thực hành Lập trình mạng (IT4062) · **Nhóm:** 8 · **Phiên bản:** 0.1

## Bài toán
Hai người trên hai máy cần tìm nhau, thách đấu và chơi một ván Caro qua mạng. Nếu có nhiều cặp cùng chơi, server phải giữ đúng bàn cờ, lượt và kết quả của từng cặp. Một client nhận chậm hoặc rời mạng không được làm các ván khác dừng theo.

## Người dùng và giá trị
- **Người chơi:** đăng nhập, biết ai đang rảnh, mời đối thủ, nhận phản hồi rõ ràng khi đánh và xem kết quả.
- **Người vận hành/demo:** khởi động một server, chạy nhiều client, tái hiện các lỗi mạng và kiểm tra ván nào bị ảnh hưởng.

## Hướng giải quyết
Client dòng lệnh kết nối server qua TCP. Server là trọng tài duy nhất; mỗi ván có mã, hai người chơi, bàn cờ và lượt riêng. Server dùng `poll` với socket không chặn; SQLite lưu tài khoản, kết quả và lịch sử.

## Mục tiêu chứng minh
1. Hai người chơi trọn một ván từ đăng nhập tới kết quả.
2. Bốn client chơi hai ván xen kẽ; một người thoát, ván còn lại tiếp tục.
3. Mục tiêu thử tải: 20 client chơi 10 ván, không gửi nhầm thông điệp giữa ván. Đây là tiêu chí kiểm thử, chưa phải kết quả đã đạt.

## Giới hạn
Bản đầu chạy trong môi trường lab kiểm soát. Chat, AI, xem trận và kết nối lại để tiếp tục ván nằm ngoài phạm vi. HTML đi kèm là prototype để duyệt luồng, không phải client mạng cuối cùng.
