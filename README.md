# STM32 Flash Tool Trên ESP32

Thiết bị ESP32 đóng vai trò web server + SWD programmer để thao tác với STM32F/STM32G.

Chức năng chính:
- Kết nối mục tiêu qua SWD.
- Nạp firmware bằng thuật toán FLM.
- Erase chip.
- Đặt mức lock (RDP).
- Quản lý firmware và scenario trên LittleFS.

## 1. Nguyên lý hoạt động

Luồng tổng quát:
1. Người dùng thao tác trên web UI (Manual/Scenario/Dashboard).
2. Trình duyệt gọi API HTTP trên ESP32.
3. ESP32 đọc file từ LittleFS (firmware, .flm, scenario JSON).
4. ESP32 điều khiển SWD để giao tiếp STM32.
5. Kết quả trả về dạng JSON hoặc text để hiển thị trên web.

### SWD mapping
- SWDIO: GPIO 17
- SWCLK: GPIO 16
- GND ESP32 và GND STM32 phải nối chung.

### Lưu trữ file
- File được lưu trong LittleFS (mount tại `/littlefs`).
- Tên file chỉ cho phép ký tự: `A-Z a-z 0-9 . _ -`.
- API liệt kê file trả về danh sách JSON.

## 2. Kiến trúc phần mềm

Các module chính trong `main/`:
- `web_server.c`: khởi tạo HTTP server và đăng ký route.
- `web_manual.c`: API/manual page cho upload, flash, erase, lock.
- `web_scenario.c`: CRUD scenario JSON.
- `web_dashboard.c`: chạy scenario theo step (connect, unlock, flash, lock).
- `storage.c`: thao tác LittleFS.
- `target_swd.c`: giao tiếp SWD mức thấp.
- `flm_parser.c` + `flm_runtime.c`: parse/chạy thuật toán `.flm`.
- `hex2bin.c`: chuyển HEX sang BIN (dùng trong upload .hex ở Manual).
- `wifi_manager.c`: cấu hình STA/AP.

## 3. Hành vi hiện tại của hệ thống

### 3.1 Manual page
- Upload hỗ trợ: `.hex`, `.bin`, `.flm`.
- Nếu upload `.hex`: backend tự convert sang `.bin`, lưu file `.bin`, sau đó xóa `.hex` để tiết kiệm bộ nhớ.
- Flash chỉ chấp nhận firmware `.bin`.
- Nếu không chọn algorithm, hệ thống tự chọn `.flm` gần nhất theo family chip.

### 3.2 Scenario page
- Quản lý file scenario `.json`.
- Tên kịch bản lấy theo tên file `.json` (không lưu trường `name` trong nội dung JSON).
- Trường chính trong scenario:
    - `firmware`
    - `address`
    - `dlm`
    - `lock` (0..2)
    - `color` (mã màu hex, ví dụ `#38bdf8`)

Ví dụ scenario:

```json
{
    "firmware": "app.bin",
    "address": "0x08000000",
    "dlm": "STM32G0xx_128.flm",
    "lock": 0,
    "color": "#00FF00"
}
```

### 3.3 Tải File FLM
Bạn có thể tải các gói CMSIS-Pack và file thuật toán `.flm` tại:

- https://github.com/Open-CMSIS-Pack

### 3.4 Dashboard page
- Mặc định route `/` trỏ vào Dashboard.
- Nút scenario được tô màu theo `color` trong file scenario.
- Khi chạy scenario, backend thực thi tuần tự các bước:
    1. Parse request + load scenario.
    2. Connect target.
    3. Check lock và unlock nếu cần.
    4. Prepare flash.
    5. Flash.
    6. Apply lock theo cấu hình.
- API trả về danh sách `steps` sau khi hoàn tất request.

## 4. API chính

### Manual
- `GET /Manual.html`
- `GET /connect`
- `GET /list_fw`
- `POST /upload_fw?name=<filename>`
- `POST /delete_fw`
- `POST /flash`
- `POST /erase`
- `POST /lock`

### Scenario
- `GET /Scenario.html`
- `GET /list_scenario`
- `GET /scenario_get?name=<file.json>`
- `POST /scenario_save`
- `POST /scenario_delete`
- `POST /upload_scenario?name=<file.json>`

### Dashboard
- `GET /`
- `GET /Dashboard.html`
- `POST /dashboard_run`

## 5. Hướng dẫn sử dụng nhanh

1. Build và flash firmware ESP32.
2. Nối dây SWDIO/SWCLK/GND tới STM32 đích.
3. Kết nối vào mạng của ESP32 (AP hoặc STA tùy cấu hình).
4. Mở trình duyệt vào IP ESP32.
5. Vào Manual để:
     - Upload firmware/algorithm.
     - Connect chip.
     - Download (flash), Erase, Lock.
6. Vào Scenario để lưu kịch bản theo từng dòng sản phẩm.
7. Vào Dashboard để chạy nhanh kịch bản theo nút.

## 6. Build project (ESP-IDF)

Ví dụ lệnh chuẩn:

```bash
idf.py set-target esp32
idf.py build
idf.py -p <PORT> flash monitor
```

Yêu cầu:
- ESP-IDF đã cài đúng version đang dùng trong workspace.
- Partition có LittleFS.

## 7. Lưu ý vận hành

- Với chip đang lock, quá trình unlock có thể làm SWD tạm mất kết nối; phần mềm đã có bước reconnect trước khi flash ở Dashboard.
- Firmware `.bin` và vùng `address` phải nằm trong flash range mà `.flm` hỗ trợ.
- Tốc độ SWD phụ thuộc timing và dây nối thực tế.

## 8. Giới hạn hiện tại

- Dashboard hiện trả trạng thái theo step ở cuối request (không stream realtime từng bước).
- Chưa hỗ trợ JTAG.
- Chưa có lớp auth cho web API (nên dùng trong mạng tin cậy).

## 9. Bảo mật

- Không để lộ SSID/password thật trong source khi đưa ra môi trường production.
- Nên thêm xác thực cho API trước khi triển khai mạng ngoài.

## 10. License

MIT License.
