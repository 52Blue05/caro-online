# MVP — Một server, nhiều ván độc lập

**Giá trị cần chứng minh:** người chơi có thể tìm đối thủ và hoàn thành ván Caro qua mạng, kể cả khi server phục vụ những cặp khác.

| Trong MVP | Sau MVP |
|---|---|
| Client dòng lệnh, tài khoản và sảnh | Giao diện đồ họa hoàn chỉnh |
| Thách đấu, chấp nhận, từ chối, hết hạn | Ghép trận tự động, chat, xem trận |
| Luật 15 × 15, đầu hàng, kết quả | AI đối thủ, tùy chọn luật và đồng hồ lượt |
| Nhiều ván độc lập, xử lý client chậm và ngắt kết nối | Kết nối lại, phục hồi bàn cờ, nhiều máy server |
| Nhật ký, điểm và xếp hạng cơ bản | Phân tích trận và bảng xếp hạng theo mùa |

| Mốc | Công việc | Điều kiện qua mốc |
|---|---|---|
| M1 | Chốt schema thông điệp, mã lỗi và vòng đời ván; dựng TCP và parser khung. | Hai client gửi/nhận được; thử khung chia nhỏ và gộp khung. |
| M2 | Tài khoản, sảnh, thách đấu và luật Caro. | Hai client chơi trọn một ván; nước sai không đổi lượt. |
| M3 | Tách trạng thái theo mã ván, hàng đợi theo kết nối, xử lý ngắt mạng. | Bốn client chơi hai ván xen kẽ; ván còn lại tiếp tục khi một client rời. |
| M4 | Kết toán SQLite, lịch sử, bảng điểm và thử tải. | Điểm lưu qua khởi động lại, chỉ cộng một lần; chạy thử 20 client/10 ván và lưu log. |

**Phân công đề xuất:** một người phụ trách vòng lặp mạng, phiên và giao thức; một người phụ trách client, luật Caro và nghiệp vụ ván. Cùng chốt interface trước khi chia, tích hợp SQLite và kiểm thử chéo.

**Demo:** chạy server trên Linux/VirtualBox, mở bốn client, tạo hai ván, đánh xen kẽ; gửi một nước sai lượt; ngắt một client ở ván đầu; tiếp tục đánh ở ván thứ hai; kiểm tra kết quả và bảng điểm. Prototype HTML dùng để duyệt kịch bản trước khi demo mạng thật.
