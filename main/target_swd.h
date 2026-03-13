#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Chân dùng cho SWD
#define SWDIO_GPIO 17
#define SWCLK_GPIO 16


#ifdef __cplusplus
extern "C"
{
#endif

// Khởi tạo GPIO cho SWD
void swd_init(void);

// Đọc IDCODE từ DP (addr 0x0)
// Trả 0 nếu OK, <0 nếu lỗi
int swd_read_idcode(uint32_t *idcode);

// Bắt đầu giao tiếp SWD (power-up, v.v.)
// Trả 0 nếu OK, <0 nếu lỗi
int swd_start(void);

// Đọc IDR của AP0 (dùng để xác nhận debug AP đã sẵn sàng)
// Trả 0 nếu OK, <0 nếu lỗi
int swd_read_ap_idr(uint32_t *idr);

// Đọc thanh ghi CPUID (0xE000ED00) qua MEM-AP
// Trả 0 nếu OK, <0 nếu lỗi
int swd_read_target_cpuid(uint32_t *cpuid);

// Đọc/Ghi 32-bit memory qua MEM-AP (AP0)
// Trả 0 nếu OK, <0 nếu lỗi
int swd_mem_read32(uint32_t addr, uint32_t *value);
int swd_mem_write32(uint32_t addr, uint32_t value);
int swd_mem_write16(uint32_t addr, uint16_t value);
int swd_mem_write_block(uint32_t addr, const uint8_t *data, uint32_t len);

// Core debug control (Cortex-M)
int swd_read_core_dhcsr(uint32_t *dhcsr);
int swd_halt_core(uint32_t *dhcsr_after);
int swd_resume_core(uint32_t *dhcsr_after);
int swd_core_call(uint32_t func_addr,
		  uint32_t lr_addr,
		  uint32_t sp,
		  uint32_t r9,
		  const uint32_t args[4],
		  uint32_t *out_r0,
		  uint32_t timeout_ms);

// Nhận diện target STM32 trước khi làm erase/flash
int swd_read_dbgmcu_idcode(uint32_t *idcode);
int swd_read_flash_size_kb(uint32_t *flash_kb);

// Reset target và cho chạy luôn
int swd_target_reset_run(void);

#ifdef __cplusplus
}
#endif
