# Cờ Caro trực tuyến · IT4062
**Nhóm 8 · Bản đặc tả và prototype v0.1**

Mở `Prototype/index.html` bằng trình duyệt sau khi giải nén. Prototype hoạt động ngoại tuyến, không cần cài đặt. Nó mô phỏng server trong trình duyệt; chưa kết nối TCP hay SQLite thật.

**Demo nhanh:** bấm **Hai ván mẫu** → đánh một ô ở ván 42 → chọn ván 43 và đánh tiếp → quay lại ván 42, bấm **Mô phỏng mất kết nối** → ván 43 vẫn chơi được. Nút **Đổi góc nhìn** cho phép thao tác từ phía đối thủ. Nút **Gửi sai lượt** minh họa server từ chối lệnh mà không đổi bàn cờ.

**Demo luồng đầy đủ:** bấm **Làm mới** → **Vào sảnh** → chọn đối thủ → **Gửi lời mời** → chuyển góc nhìn của người nhận và chấp nhận → đánh cờ hoặc đầu hàng → xem nhật ký và bảng điểm.

| Tài liệu | Mục đích |
|---|---|
| [01_Brief](Tai_lieu/01_Brief.md) | Bài toán, người dùng, giá trị và mục tiêu |
| [02_PRD](Tai_lieu/02_PRD.md) | Yêu cầu, luồng nghiệp vụ, giao thức và nghiệm thu |
| [03_User_Stories](Tai_lieu/03_User_Stories.md) | Backlog theo góc nhìn người dùng, kèm tiêu chí chấp nhận |
| [04_MVP](Tai_lieu/04_MVP.md) | Phạm vi, mốc triển khai và kịch bản demo |
| [05_Data_Model](Data_model/05_Data_Model.md) | ERD, vòng đời dữ liệu và các ràng buộc |
| [schema.sql](Data_model/schema.sql) | Schema SQLite đề xuất |

Prototype dùng tài khoản mẫu và chốt ván ngay khi bấm mô phỏng mất kết nối. Quy tắc chờ khi hai phía ngắt và lưu bền vững thuộc bản triển khai mạng trong PRD. Hai nút **Hai ván mẫu** và **Làm mới** đặt lại dữ liệu demo.
