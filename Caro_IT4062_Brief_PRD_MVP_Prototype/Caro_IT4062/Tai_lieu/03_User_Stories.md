# User Stories và tiêu chí chấp nhận

| ID | User story | Given / When / Then | PRD |
|---|---|---|---|
| US-01 | Là người chơi mới, tôi muốn tạo tài khoản để vào sảnh. | Tên chưa tồn tại / đăng ký hợp lệ rồi đăng nhập / vào sảnh; tên trùng hoặc mật khẩu sai được báo lỗi. | FR-01 |
| US-02 | Là người chơi, tôi muốn biết ai đang rảnh để chọn đối thủ. | Đã vào sảnh / người khác đăng nhập hoặc vào ván / danh sách phản ánh đúng trạng thái. | FR-02 |
| US-03 | Là người mời, tôi muốn biết kết quả lời thách đấu để quyết định chờ hay chọn người khác. | Cả hai rảnh / gửi lời mời / đối thủ nhận thông báo; từ chối hoặc hết hạn thì không tạo ván. | FR-03 |
| US-04 | Là người nhận lời mời, tôi muốn chấp nhận để vào đúng ván với người đã mời. | Lời mời còn hạn / chấp nhận / cả hai nhận cùng mã ván và quân khác nhau; lần chấp nhận thứ hai không tạo ván mới. | FR-03, FR-04 |
| US-05 | Là người chơi, tôi muốn đánh theo lượt và thấy cùng bàn cờ với đối thủ. | Đang đến lượt X / X đánh ô trống / cả hai thấy quân X tại cùng tọa độ; O được đi tiếp nếu ván chưa hết. | FR-05 |
| US-06 | Là người chơi, tôi muốn hiểu vì sao nước đi bị từ chối để có thể đánh lại. | Sai lượt, sai ô hoặc sai mã ván / gửi nước đi / nhận lỗi; bàn cờ và lượt chính thức không thay đổi. | FR-05 |
| US-07 | Là người chơi, tôi muốn biết kết quả và lý do kết thúc. | Đang chơi / có 5 quân liền nhau, bàn đầy không ai thắng hoặc một người đầu hàng / nhận kết quả đúng và nhật ký theo thứ tự. | FR-06 |
| US-08 | Là người chơi, tôi muốn ván của mình vẫn tiếp tục khi cặp khác bị mất mạng. | Có hai ván / một người của ván thứ nhất ngắt kết nối / ván thứ hai vẫn đánh được; bàn cờ và lượt của ván thứ hai không bị can thiệp. | FR-06, FR-08 |
| US-09 | Là người chơi, tôi muốn xem điểm sau ván để theo dõi kết quả. | Ván đã kết toán / mở xếp hạng / điểm và thống kê khớp kết quả, sự kiện lặp không cộng điểm lần hai. | FR-07 |
