# Đặc tả Cấu trúc Topic và Định dạng Payload MQTT

**Sinh viên thực hiện:** Bùi Sỹ Việt Anh  
**MSSV:** 23110300  
**Học phần:** Phát triển ứng dụng IoT (CSE3069) — Bài thực hành 04  

---

## 1. Cấu trúc Topic (Topic Hierarchy)

Hệ thống tuân thủ cấu trúc phân cấp chuẩn trong IoT công nghiệp:  
`[nhóm_hoặc_tên_sinh_viên]/[mã_thiết_bị]/[loại_dữ_liệu]`

| Topic | Chiều truyền | QoS | Retain | Mục đích & Ý nghĩa |
|---|---|:---:|:---:|---|
| `VietAnh/greenhouse01/telemetry` | ESP32 $\rightarrow$ Broker | 0 | False | Luồng dữ liệu cảm biến định kỳ (chu kỳ 2 giây/bản tin). Dữ liệu biến động nhanh, chấp nhận mất mát tối thiểu nếu nghẽn mạng cục bộ nên dùng QoS 0 để tối ưu băng thông và giảm độ trễ vi điều khiển. |
| `VietAnh/greenhouse01/status` | ESP32 $\leftrightarrow$ Broker | 1 | True | Thông tin vòng đời thiết bị (Lifecycle / Presence). Sử dụng cơ chế LWT (Last Will and Testament) để Broker tự động phát `"offline"` khi mất kết nối bất ngờ, và gửi `"online"` khi vừa kết nối lại. |

---

## 2. Định dạng Payload Telemetry (JSON Payload)

### 2.1 Bản tin hợp lệ (Trạng thái đo bình thường)

```json
{
  "device_id": "greenhouse01",
  "seq": 145,
  "ts": 290140,
  "temp": 28.6,
  "hum": 68.4,
  "rain": 0,
  "rssi": -58,
  "valid": true
}
```

### 2.2 Bản tin khi cảm biến gặp sự cố / ngắt kết nối (Failsafe)

```json
{
  "device_id": "greenhouse01",
  "seq": 180,
  "ts": 360140,
  "temp": null,
  "hum": null,
  "rain": null,
  "rssi": -62,
  "valid": false
}
```

---

## 3. Giải thích chi tiết các trường dữ liệu

| Trường dữ liệu | Kiểu dữ liệu | Đơn vị | Giải thích ý nghĩa kỹ thuật |
|---|---|:---:|---|
| `device_id` | `string` | — | Mã định danh duy nhất của thiết bị trong hệ thống, khớp hoàn toàn với định danh trên topic (`greenhouse01`). |
| `seq` | `unsigned long` | — | Số thứ tự tăng dần liên tục (+1 mỗi chu kỳ 2s). Giúp bên nhận phát hiện rớt gói mạng, gói tin đến trễ hoặc trùng lặp. |
| `ts` | `unsigned long` | `ms` | Thời gian tính bằng mili-giây từ lúc ESP32 khởi động (`millis()`). Dùng để đo lường độ trễ mạng và độ tươi của dữ liệu. |
| `temp` | `float` hoặc `null` | `°C` | Nhiệt độ không khí trong nhà kính đo từ cảm biến I2C/DHT. Nếu cảm biến đứt dây, đọc lỗi sẽ là `null`. |
| `hum` | `float` hoặc `null` | `%` | Độ ẩm tương đối của không khí trong nhà kính. Giá trị thiếu trả về `null`. |
| `rain` | `int` hoặc `null` | — | Trạng thái cảm biến mưa: `0` là khô ráo (không mưa), `1` là có nước đọng (mưa). Nếu cảm biến hỏng/rút dây trả về `null`. |
| `rssi` | `int` | `dBm` | Cường độ tín hiệu WiFi thu được tại ESP32, dùng đánh giá chất lượng kênh truyền vô tuyến. |
| `valid` | `boolean` | — | Cờ chất lượng dữ liệu: `true` nếu mọi cảm biến hoạt động và số đo nằm trong dải vật lý hợp lệ; `false` nếu phát hiện bất thường. |

---

## 4. Quy tắc phân biệt nghiêm ngặt giữa `null` và `0`

Kế thừa kinh nghiệm xử lý dữ liệu từ Bài thực hành 02:
1. **Giá trị `0` (Zero):** Là một số đo vật lý có ý nghĩa thực tế.
   - Ví dụ: `rain: 0` xác nhận cảm biến hoạt động bình thường và môi trường đang **hoàn toàn khô ráo, không có mưa**.
   - Ví dụ: `temp: 0.0` đại diện cho nhiệt độ thực tế đang ở mức 0 độ C.
2. **Giá trị `null`:** Biểu thị trạng thái **mất dữ liệu (Missing Data)** do cảm biến chưa sẵn sàng, dây cắm bị lỏng hoặc phần cứng giao tiếp I2C bị treo.
3. **Quy định thiết kế:** Tuyệt đối không dùng số `0` hoặc `-999` để thay thế cho giá trị lỗi. Việc sử dụng `null` chuẩn JSON giúp hệ thống phía trên (Dashboard, Database, Controller) nhận diện chính xác lỗi để kích hoạt quy trình an toàn (Failsafe), tránh việc nhầm tưởng nhiệt độ = 0°C hay trời không mưa mà bơm tưới hoạt động sai lệch.

