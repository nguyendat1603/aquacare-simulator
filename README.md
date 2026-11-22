# aquacare-simulator
Hệ thống chăm sóc hải sản trong hồ nuôi bao gồm các bộ phận:
 - Các cảm biến môi trường đo đạc nồng độ muối, nồng độ oxi, độ pH trong nước
 - Bộ phận bơm nước với lưu lượng V (m3/giờ) và bơm trong T (giờ) do người dùng thiết lập.
 - Bộ phận quạt sục khí bề mặt với tốc độ quay C (vòng/phút), hoạt động theo các khoảng thời gian trong ngày do người dùng thiết lập.
 - Bộ phận cân bằng độ pH bằng phun vôi bột với lượng Wca(kg) mỗi lần khi độ pH giảm xuống dưới mức pHmin
 - Bộ phận máy cho ăn tự động, hoạt động theo các thời điểm trong ngày với lượng thức ăn Wf(kg) mỗi lần.
 Xây dựng ứng dụng mô phỏng bao gồm 2 loại chương trình:
 - Chương trình mô phỏng các loại cảm biến và thiết bị:
 + Mỗi thiết bị có một giá trị ID duy nhất
 + Lắng nghe các thông điệp quét trên một cổng ứng dụng nào đó.
 + Khi nhận được yêu cầu kết nối, kiểm tra mật khẩu trong thông điệp. Nếu mật khẩu trùng khớp thì gửi lại thông điệp có chứa các thông tin về thiết bị. Để quản lý các ứng dụng đã được xác thực, thiết bị tạo ra một giá trị token ngẫu nhiên gắn với ID của ứng dụng.
 + Thực hiện các lệnh điều khiển bật/tắt, thiết lập thông số hoạt động
 - Chương trình điều khiển cho người dùng, gồm các chức năng:
 + Mỗi ứng dụng cài đặt được khởi tạo một giá trị ID ngẫu nhiên
 + Quản lý các hồ nuôi
 + Khi thực hiện lệnh quét, chương trình quét các thiết bị và hiển thị loại thiết bị và ID cho người dùng. Người dùng thêm thiết bị mới này vào danh sách thiết bị trong hồ nuôi.
 + Người dùng yêu cầu kết nối tới thiết bị. Chương trình gửi mật khẩu do người dùng nhập và ID của ứng dụng.
 + Đổi mật khẩu của thiết bị
 + Người dùng có thể điều khiển bật/tắt, thiết lập thông số hoạt động

Cách cho điểm: 
 - Xử lý truyền dòng: 1 điểm
 - Cài đặt cơ chế vào/ra socket trên server: 2 điểm
 - Xác thực truy cập điều khiển: 1 điểm
 - Khởi tạo các thông số cho thiết bị: 1 điểm
 - Quét thiết bị: 2 điểm
 - Kết nối thiết bị: 2 điểm
 - Đổi mật khẩu: 1 điểm
 - Điều khiển bật tắt: 1 điểm
 - Thiết lập thông số cho bộ phận bơm nước: 1 điểm
 - Thiết lập thông số cho quạt sục khí bề mặt: 1 điểm
 - Thiết lập thông số cho bộ cân bằng pH: 1 điểm
 - Thiết lập thông số cho bộ phận cho ăn tự động: 1 điểm
 - Điều khiển trực tiếp (Bơm nước, sục khí, cho ăn) bên cạnh các cơ chế lập lịch: 1 điểm
 - Lấy thông tin, thông số mỗi loại thiết bị(cảm biến môi trường, bơm nước, sục khí, cân bằng pH, cho ăn): 2 điểm
 - Ghi log hoạt động trên mỗi thiết bị: 1 điểm
 - Quản lý thông tin hồ nuôi: 1 điểm

## Module xử lý truyền dòng TCP

Module `tcp_stream.c` và `tcp_stream.h` đóng gói mỗi thông điệp thành tiền tố
độ dài 4 byte (network byte order) và phần dữ liệu. `tcp_send_message()` gửi
đủ tiền tố và nội dung, kể cả khi `send()` chỉ gửi được một phần.
`tcp_receive_message()` đọc đủ một thông điệp, không làm mất dữ liệu của thông
điệp kế tiếp nếu nhiều thông điệp đã được nhận cùng lúc. Kích thước dữ liệu tối
đa là 1 MiB. Hàm nhận trả về `1` khi nhận thành công, `0` khi phía bên kia đóng
kết nối đúng giữa hai thông điệp, và `-1` khi có lỗi; dữ liệu nhận được do bên
gọi cấp phát và cần giải phóng bằng `free()`.

### Echo server dùng `select()`

Server dùng socket non-blocking và `select()` để theo dõi socket lắng nghe cùng
các Client. Mỗi Client có parser và hàng đợi gửi riêng; dữ liệu chưa đủ một
khung được giữ lại mà không chặn các Client khác. Hàng đợi gửi cũng xử lý
trường hợp `send()` chỉ gửi được một phần. Mỗi Client được giới hạn hàng đợi
gửi ở 4 MiB; Client vượt giới hạn sẽ bị ngắt kết nối.

Biên dịch trên Ubuntu/Linux:

```sh
gcc -Wall -Wextra -std=c11 tcp_stream.c tcp_server.c -o tcp_server
gcc -Wall -Wextra -std=c11 tcp_stream.c tcp_client.c -o tcp_client
gcc -Wall -Wextra -std=c11 -pthread tcp_stream.c test_tcp_stream.c -o test_tcp_stream
```

Chạy Server (cổng mặc định `9000`, có thể chỉ định cổng khác):

```sh
./tcp_server 9000
```

Trong terminal khác, gửi một thông điệp và xem phản hồi:

```sh
./tcp_client 127.0.0.1 9000 "Xin chao server"
```

Để thử nhiều Client cùng lúc, mở thêm terminal và chạy lệnh `tcp_client` với
các thông điệp khác nhau; Server phục vụ chúng trong một tiến trình, không tạo
thread/process riêng cho từng kết nối. Dừng Server bằng `Ctrl+C`.

Chương trình kiểm thử dùng `socketpair()` để kiểm tra thông điệp bị chia nhỏ,
nhiều thông điệp liên tiếp, dữ liệu nhị phân, thông điệp rỗng và thông điệp lớn:

```sh
gcc -Wall -Wextra -std=c11 -pthread tcp_stream.c test_tcp_stream.c -o test_tcp_stream
./test_tcp_stream
```
