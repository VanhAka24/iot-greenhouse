# Bài 1 — Sơ đồ nhà kính theo mẫu (Cá nhân)

Họ tên: Bùi Sỹ Việt Anh
MSSV: 23110300
Nhóm: Cá nhân (23110300)

## 1. Cấu hình cá nhân

| Mục | Mẫu để tham khảo | Sinh viên điền |
|---|---|---|
| Bộ điều khiển | ESP32, node01 | ESP32, node23110300 |
| Cảm biến | DHT11 | DHT11 |
| Đầu ra thử | LED trên breadboard | LED đỏ / xanh trên bo Farm House |
| Nơi chạy broker/dịch vụ | PC của nhóm | PC cá nhân |
| Topic số đo | nhom01/node01/telemetry | 23110300/node23110300/telemetry |
| Topic lệnh | nhom01/node01/cmd | 23110300/node23110300/cmd |
| Topic xác nhận | nhom01/node01/ack | 23110300/node23110300/ack |

## 2. Năm sơ đồ theo mẫu

- Sơ đồ chức năng: ./01_chuc_nang.png
- Luồng số đo: ./02_luong_so_do.png
- Luồng lệnh: ./03_luong_lenh.png
- Quy trình xử lý lệnh (Activity): ./04_hoat_dong.png
- Triển khai trên kit (Deployment): ./05_trien_khai.png
- Tệp nguồn sơ đồ: ./01-chuc-nang.puml, ./02-luong-so-do.puml, ./03-luong-lenh.puml, ./04-hoat-dong.puml, ./05-trien-khai.puml

## 3. Bảng chân kết nối nhà kính (Schematic trang 4 - ESP32)

| Tên thiết bị / Linh kiện | Ký hiệu / Đường tín hiệu | Chân GPIO ESP32 | Linh kiện trung gian 
|---|---|---|---|---|
| Cảm biến Nhiệt độ/Độ ẩm | `DATA_RH` | GPIO 23 | Điện trở kéo lên `R27` | 
| Màn hình / Cảm biến I²C | `SDA` / `SCL` | GPIO 21 / GPIO 22 | Điện trở gắn dòng | 
| Đèn LED đỏ (RED_LED) | `LED_R` | GPIO 25 | Transistor `MMBT3904` (`Q18`) | 
| Đèn LED xanh (GREEN_LED)| `LED_G` | GPIO 26 | Transistor `MMBT3904` (`Q12`) |
| Động cơ 1 (MOTOR 1) | `MOTOR1_INT1` / `MOTOR1_INT2` | GPIO 32 / GPIO 33 | IC công suất `44950LITR` (`U2`) | 
| Động cơ 2 (MOTOR 2) | `MOTOR2_INT1` / `MOTOR2_INT2` | GPIO 18 / GPIO 19 | IC công suất `44950LITR` (`U3`) | 
| Dải LED (ARRAY_LED3) | `ARRAY_LED3` | GPIO 14 | Mạch đệm `N531` (`U11`) | 
| Dải LED (ARRAY_LED4) | `ARRAY_LED4` | GPIO 13 | Mạch đệm `N531` (`U12`) | 

## 4. Giải thích ngắn theo mẫu

- **Một số đo đi đâu?** DHT11 → ESP32 → MQTT Broker → Dịch vụ ứng dụng (ApplicationService) → Cơ sở dữ liệu (MeasurementStore) / Giao diện người dùng (WebUI).
- **Một lệnh đi đâu?** Giao diện WebUI → Dịch vụ ứng dụng → MQTT Broker → ESP32 → Điều khiển GPIO đặt trạng thái LED; Phản hồi `ack` đi ngược từ ESP32 qua Broker về Dịch vụ ứng dụng để hiển thị lên WebUI.
- **Broker có tự đọc cảm biến không?** Không. Cảm biến do vi điều khiển ESP32 đọc dữ liệu. Broker chỉ đóng vai trò trung gian chuyển tiếp các bản tin MQTT giữa ESP32 và Dịch vụ ứng dụng.
- **Có ack nghĩa là gì?** Node ESP32 xác nhận đã nhận lệnh hợp lệ và đã gọi lệnh thiết lập logic cho chân GPIO. Điều này chưa có nghĩa là có phép đo vật lý độc lập chứng minh dòng điện đã chạy qua LED/tải thực tế.
- **Em đã đổi gì từ mẫu?** Cấu hình lại các mã định danh `nhom01` -> `23110300`, `node01` -> `node23110300`, đổi tên topic tương ứng và chỉ định môi trường chạy Broker/Dịch vụ trên PC cá nhân.
- **Đường nối Use Case có phải luồng dữ liệu không?** Không, đường nối giữa Actor và Use Case (association) thể hiện vai trò tham gia chức năng của người dùng, không mô tả luồng truyền dữ liệu hay thứ tự thực hiện.
- **Activity và Sequence khác nhau thế nào?** Activity mô tả quy trình, tiến trình xử lý, các nút rẽ nhánh và thành phần chịu trách nhiệm theo luồng công việc; trong khi Sequence mô tả chi tiết các bản tin/lời gọi trao đổi giữa các đối tượng theo thứ tự thời gian.

## 5. Phần kiểm chứng để lại cho các bài sau

- Trên kit/PC: Môi trường chạy Mosquitto Broker, dịch vụ ứng dụng Python và phần mềm nạp bo ESP32.
- Trên nhà kính chung: Kết nối mạch phần cứng thật, đấu nối tải công suất và kiểm chứng tín hiệu đo/điều khiển thực tế.
