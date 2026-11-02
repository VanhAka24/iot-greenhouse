# Bài 3 — Điều khiển thiết bị an toàn

Họ tên: Bùi Sỹ Việt Anh · MSSV: 23110300

## File trong repo này
- control.ino — firmware máy trạng thái DỪNG/CHẠY/LỖI (có ngắt ngoài và timer phần cứng)
- fsm_diagram.png — sơ đồ trạng thái
- secrets.h.example — mẫu cấu hình WiFi 

## Thông số đã chọn
- Thời gian chạy tối đa MAX_RUN_MS = 20 s (trong khoảng gợi ý 10–30 s)
- Thời gian giữ trạng thái LỖI trước khi về DỪNG FAULT_HOLD_MS = 5 s
- Chống dội nút DEBOUNCE_MS = 50 ms
- Chu kỳ timer phần cứng = 2 s
- Ba mức duty cycle PWM: 25 % (64), 50 % (127), 90 % (229) trên thang 8 bit

## 4.1 Cấu hình phần cứng
- Tải: bơm DC, điều khiển qua driver A4950ELJTR.
- GPIO5 (MOTOR1_INT1): đầu vào IN1 của A4950, xuất PWM bằng LEDC kênh 0, tần số 5 kHz, 8 bit.
- GPIO17 (MOTOR1_INT2): đầu vào IN2 của A4950, luôn ở mức LOW khi bơm chạy.
- GPIO36: nút nhấn, dùng ngắt ngoài. Chân này không có pull-up nội nên cần điện trở kéo lên bên ngoài.
- GPIO26: LED trạng thái.
- I2C: SDA = GPIO33, SCL = GPIO32, đọc cảm biến nhiệt độ và độ ẩm từ Arduino slave địa chỉ 0x08.
- Khi khởi động, setup() đặt GPIO17 và LED về LOW, đặt PWM về 0 trước khi bật ngắt và timer.

## 4.2 Ngắt ngoài
ISR (btnISR) chỉ tăng biến đếm volatile btnRawCount, không in Serial và không gọi delay().
Chống dội thực hiện ngoài ISR: loop() ghi nhận thời điểm cạnh mới cuối cùng, và chỉ tính một lần bấm khi tín hiệu đã ổn định 50 ms. Cách này dùng thời gian, không dùng phần cứng.

## 4.3 Timer phần cứng và PWM
Timer 0 dùng prescaler 80, đếm 1 MHz, alarm 2 000 000 µs, tự nạp lại. ISR chỉ đặt cờ timerFlag. Mỗi lần cờ được xử lý trong loop(), chương trình đọc cảm biến, cập nhật máy trạng thái và in một dòng CSV có cột timer_cycle_ms.

## 4.4 Máy trạng thái và các ca thử
Thiết kế: DỪNG → CHẠY (bằng nút hoặc lệnh web) → LỖI khi hết MAX_RUN_MS hoặc cảm biến không hợp lệ → DỪNG sau FAULT_HOLD_MS.
- Khi ở LỖI, lệnh CHẠY bị bỏ qua.
- Khi ở DỪNG, bơm luôn tắt.
- Các ca thử (đầu vào và kỳ vọng):
  - Ca 1: gửi lệnh bật khi đang DỪNG và chưa hết giờ → CHẠY.
  - Ca 2: để CHẠY đến khi vượt MAX_RUN_MS, không gửi lệnh tắt → LỖI (TIMEOUT), rồi DỪNG.
  - Ca 3: rút cảm biến khi đang CHẠY → LỖI (SENSOR_INVALID), rồi DỪNG.

## Lệnh điều khiển
- Nút nhấn vật lý: đổi DỪNG ↔ CHẠY.
- Web (http://IP/): /api/cmd?action=start|stop, /api/duty?pct=25|50|90, /api/status.
- Serial (115200): '1', '2', '3' đặt duty; 'i' quét I2C; 's' in trạng thái.
