#include "target_swd.h"

#include <inttypes.h>

#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "soc/io_mux_reg.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "SWD";

// ================= LOW LEVEL ================= //

static inline void swd_delay(void)
{
	// Delay rất ngắn, để clock lên vài trăm kHz ~ 1 MHz
	for (volatile int i = 0; i < 1; i++)
	{
		__asm__ __volatile__("nop");
	}
}

// Đặt SWDIO làm output
static inline void swdio_set_output_fast(void)
{
	GPIO.enable_w1ts = (1 << SWDIO_GPIO);
	PIN_INPUT_DISABLE(GPIO_PIN_MUX_REG[SWDIO_GPIO]);
}

// Đặt SWDIO làm input
static inline void swdio_set_input_fast(void)
{
	GPIO.enable_w1tc = (1 << SWDIO_GPIO);
	PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[SWDIO_GPIO]);
}

static inline void swdio_write_fast(int lvl)
{
	if (lvl)
		GPIO.out_w1ts = (1 << SWDIO_GPIO);
	else
		GPIO.out_w1tc = (1 << SWDIO_GPIO);
}

static inline int swdio_read_fast(void)
{
	return (GPIO.in >> SWDIO_GPIO) & 1;
}

static inline void swclk_set(int level)
{
	if (level)
		GPIO.out_w1ts = (1 << SWCLK_GPIO);
	else
		GPIO.out_w1tc = (1 << SWCLK_GPIO);
	swd_delay();
}

// Ghi 1 bit khi SWDIO đã ở mode OUTPUT
static inline void swd_write_bit(int bit)
{
	swclk_set(0);
	swdio_write_fast(bit ? 1 : 0);
	swclk_set(1);
}

// Đọc 1 bit khi SWDIO đã ở mode INPUT
static inline int swd_read_bit(void)
{
	int bit;
	swclk_set(0);
	bit = swdio_read_fast();
	swclk_set(1);
	return bit;
}

// 1-cycle turnaround: host releases SWDIO, target takes ownership
static inline void swd_turnaround_host_to_target(void)
{
	swdio_write_fast(1);
	swclk_set(0);
	swclk_set(1);
	swdio_set_input_fast();
}

// 1-cycle turnaround: target releases SWDIO, host takes ownership
static inline void swd_turnaround_target_to_host(void)
{
	swdio_set_output_fast();
	swdio_write_fast(1);
	swclk_set(0);
	swclk_set(1);
}

// ================= SEQUENCE ================= //

// Line reset: ít nhất 50 xung với SWDIO = 1
static void swd_line_reset(void)
{
	swdio_set_output_fast();
	swdio_write_fast(1);

	// Không thêm delay dài, chỉ cần >= 50 xung clock với SWDIO = 1
	for (int i = 0; i < 56; i++)
	{
		swclk_set(0);
		swclk_set(1);
	}
}

// JTAG-to-SWD sequence (0xE79E, LSB first)
static void swd_send_jtag_to_swd(void)
{
	const uint16_t seq = 0xE79E; // LSB first
	swdio_set_output_fast();

	for (int i = 0; i < 16; i++)
	{
		int bit = (seq >> i) & 1;
		swd_write_bit(bit);
	}
}

// ================= PARITY ================= //

static int swd_parity4(int apndp, int rnw, int a2, int a3)
{
	int p = apndp ^ rnw ^ a2 ^ a3;
	return p & 1;
}

static int swd_parity32(uint32_t v)
{
	v ^= v >> 16;
	v ^= v >> 8;
	v ^= v >> 4;
	v &= 0xF;
	return (0x6996 >> v) & 1;
}

#define DP_REG_ABORT 0x0
#define DP_REG_CTRL_STAT 0x4
#define DP_REG_SELECT 0x8
#define DP_REG_RDBUFF 0xC

#define AP_REG_CSW 0x0
#define AP_REG_TAR 0x4
#define AP_REG_DRW 0xC

#define AP_CSW_32BIT_NOINC 0x23000052UL
#define AP_CSW_16BIT_NOINC 0x23000051UL
#define ARM_CPUID_ADDR 0xE000ED00UL

#define ARM_AIRCR_ADDR 0xE000ED0CUL
#define ARM_AIRCR_VECTKEY (0x5FAUL << 16)
#define ARM_AIRCR_SYSRESETREQ (1UL << 2)

#define ARM_DHCSR_ADDR 0xE000EDF0UL
#define ARM_DCRSR_ADDR 0xE000EDF4UL
#define ARM_DCRDR_ADDR 0xE000EDF8UL
#define ARM_DFSR_ADDR 0xE000ED30UL
#define ARM_DHCSR_DBGKEY (0xA05FUL << 16)
#define ARM_DHCSR_C_DEBUGEN (1UL << 0)
#define ARM_DHCSR_C_HALT (1UL << 1)
#define ARM_DHCSR_S_HALT (1UL << 17)
#define ARM_DHCSR_S_REGRDY (1UL << 16)

#define ARM_XPSR_T_BIT (1UL << 24)

#define ARM_CORE_REG_R0 0U
#define ARM_CORE_REG_R1 1U
#define ARM_CORE_REG_R2 2U
#define ARM_CORE_REG_R3 3U
#define ARM_CORE_REG_R9 9U
#define ARM_CORE_REG_R13 13U
#define ARM_CORE_REG_R14 14U
#define ARM_CORE_REG_R15 15U
#define ARM_CORE_REG_XPSR 16U

#define STM32_DBGMCU_IDCODE_AHB 0xE0042000UL
#define STM32_DBGMCU_IDCODE_APB 0x40015800UL


// ================= REQUEST & ACCESS ================= //
static void swd_wait_before_request(void)
{
}

// Gửi 8-bit request, LSB first
// addr: 0, 4, 8, 12 (DP/AP register address)
static void swd_send_request(int apndp, int rnw, uint8_t addr)
{
	swd_wait_before_request();

	// addr bits A2,A3 (bit1,bit2 của addr/4)
	int a2 = (addr >> 2) & 1;
	int a3 = (addr >> 3) & 1;
	int parity = swd_parity4(apndp, rnw, a2, a3);

	uint8_t req =
		(1 << 0) |		// start = 1
		(apndp << 1) |	// APnDP
		(rnw << 2) |	// RnW
		(a2 << 3) |		// A2
		(a3 << 4) |		// A3
		(parity << 5) | // parity
		(0 << 6) |		// stop = 0
		(1 << 7);		// park = 1

	// Quan trọng: set OUTPUT 1 lần, không đổi direction trong từng bit
	swdio_set_output_fast();

	for (int i = 0; i < 8; i++)
	{
		swd_write_bit((req >> i) & 1);
	}
}

// Đọc từ DP (địa chỉ addr), lưu vào data_out
// return 0 = OK, <0 = lỗi
static int swd_read_dp(uint8_t addr, uint32_t *data_out)
{
	if (!data_out)
		return -10;

	// Gửi request: DP, Read
	swd_send_request(0, 1, addr);

	// Turnaround: host nhả bus, target nắm
	swd_turnaround_host_to_target();

	// Đọc ACK: 3 bit, LSB first
	int ack = 0;
	for (int i = 0; i < 3; i++)
	{
		int bit = swd_read_bit();
		ack |= (bit << i);
	}

	if (ack != 0x1)
	{
		ESP_LOGE(TAG, "ACK != 0x1, ack = 0x%x", ack);
		// Một số giá trị:
		// 0b001 = OK
		// 0b010 = WAIT
		// 0b100 = FAULT
		swd_turnaround_target_to_host();
		swdio_write_fast(1);
		return -1;
	}

	// Đọc 32-bit data (LSB first)
	uint32_t data = 0;
	for (int i = 0; i < 32; i++)
	{
		int bit = swd_read_bit();
		data |= ((uint32_t)bit << i);
	}

	// Đọc parity
	int parity = swd_read_bit();
	int calc_parity = swd_parity32(data);

	if (parity != calc_parity)
	{
		ESP_LOGE(TAG, "Parity mismatch: recv=%d calc=%d data=0x%08x",
				 parity, calc_parity, data);
		swd_turnaround_target_to_host();
		swdio_write_fast(1);
		return -2;
	}

	// Turnaround: trả bus lại cho host
	swd_turnaround_target_to_host();
	swdio_write_fast(1);

	*data_out = data;
	return 0;
}

// ghi 32-bit DP register
// return 0 = OK, <0 = lỗi
static inline int swd_write_dp(uint8_t addr, uint32_t value)
{
	// 1) gửi request: DP, Write
	swd_send_request(0 /*DP*/, 0 /*Write*/, addr);

	// 2) turnaround: host -> target
	// For write ACK path, release bus before the turnaround clock.
	swdio_write_fast(1);
	swdio_set_input_fast();
	swclk_set(0);
	swclk_set(1);

	// 3) đọc ACK
	int ack = 0;
	for (int i = 0; i < 3; i++)
	{
		int bit = swd_read_bit();
		ack |= (bit << i);
	}
	if (ack != 0x1)
	{
		ESP_LOGE(TAG, "WRITE ACK != 0x1, ack = 0x%x", ack);
		swd_turnaround_target_to_host();
		swdio_write_fast(1);
		return -1; // WAIT/FAULT
	}

	// 4) turnaround: target -> host, gửi 32-bit data + parity
	swd_turnaround_target_to_host();
	uint32_t data = value;
	int parity = swd_parity32(data);

	for (int i = 0; i < 32; i++)
	{
		swdio_write_fast((data >> i) & 1);
		swclk_set(0);
		swclk_set(1);
	}

	swdio_write_fast(parity);
	swclk_set(0);
	swclk_set(1);

	// 5) park bus ở mức 1
	swdio_write_fast(1);

	return 0;
}

// Đọc AP register (pipelined): kết quả thật nằm ở DP_RDBUFF
static int swd_read_ap(uint8_t addr, uint32_t *data_out)
{
	if (!data_out)
		return -10;

	swd_send_request(1, 1, addr);

	// Turnaround: host -> target
	swdio_write_fast(1);
	swdio_set_input_fast();
	swclk_set(0);
	swclk_set(1);

	int ack = 0;
	for (int i = 0; i < 3; i++)
	{
		int bit = swd_read_bit();
		ack |= (bit << i);
	}
	if (ack != 0x1)
	{
		ESP_LOGE(TAG, "AP READ ACK != 0x1, ack = 0x%x", ack);
		swd_turnaround_target_to_host();
		swdio_write_fast(1);
		return -1;
	}

	// Đọc data phase của AP read (dummy theo pipeline, vẫn phải clock đủ)
	uint32_t dummy = 0;
	for (int i = 0; i < 32; i++)
	{
		int bit = swd_read_bit();
		dummy |= ((uint32_t)bit << i);
	}
	(void)dummy;

	int parity = swd_read_bit();
	(void)parity;

	// Turnaround: target -> host
	swd_turnaround_target_to_host();
	swdio_write_fast(1);

	// Lấy data thật từ RDBUFF
	return swd_read_dp(DP_REG_RDBUFF, data_out);
}

// Ghi AP register
static int swd_write_ap(uint8_t addr, uint32_t value)
{
	swd_send_request(1, 0, addr);

	// Turnaround: host -> target (đọc ACK)
	swdio_write_fast(1);
	swdio_set_input_fast();
	swclk_set(0);
	swclk_set(1);

	int ack = 0;
	for (int i = 0; i < 3; i++)
	{
		int bit = swd_read_bit();
		ack |= (bit << i);
	}
	if (ack != 0x1)
	{
		ESP_LOGE(TAG, "AP WRITE ACK != 0x1, ack = 0x%x", ack);
		swd_turnaround_target_to_host();
		swdio_write_fast(1);
		return -1;
	}

	// Turnaround: target -> host (ghi data)
	swd_turnaround_target_to_host();

	int parity = swd_parity32(value);
	for (int i = 0; i < 32; i++)
	{
		swdio_write_fast((value >> i) & 1);
		swclk_set(0);
		swclk_set(1);
	}

	swdio_write_fast(parity);
	swclk_set(0);
	swclk_set(1);

	swdio_write_fast(1);
	return 0;
}

// ================= PUBLIC API ================= //

void swd_init(void)
{
	ESP_LOGI(TAG, "Init SWD pins: SWDIO=%d, SWCLK=%d", SWDIO_GPIO, SWCLK_GPIO);

	// SWCLK: output, default low
	gpio_config_t io_conf = {0};
	io_conf.pin_bit_mask = (1ULL << SWCLK_GPIO);
	io_conf.mode = GPIO_MODE_OUTPUT;
	io_conf.pull_down_en = 0;
	io_conf.pull_up_en = 0;
	io_conf.intr_type = GPIO_INTR_DISABLE;
	gpio_config(&io_conf);
	gpio_set_level(SWCLK_GPIO, 0);

	// SWDIO: output, idle high
	io_conf.pin_bit_mask = (1ULL << SWDIO_GPIO);
	io_conf.mode = GPIO_MODE_OUTPUT;
	io_conf.pull_down_en = 0;
	io_conf.pull_up_en = 1; // nhẹ cho an toàn
	io_conf.intr_type = GPIO_INTR_DISABLE;
	gpio_config(&io_conf);
	gpio_set_level(SWDIO_GPIO, 1);
}

int swd_read_idcode(uint32_t *idcode)
{
	if (!idcode)
		return -10;

	ESP_LOGI(TAG, "Read IDCODE...");

	// Reset line + JTAG-to-SWD + reset line (khuyến nghị theo spec)
	swd_line_reset();
	swd_send_jtag_to_swd();
	swd_line_reset();

	// Keep legacy idle pattern before first DP access (validated in hardware test).
	swdio_set_output_fast();
	swdio_write_fast(0);
	swclk_set(0);
	swclk_set(1);

	// ESP_LOGI(TAG, "Line Reset FINISH");

	uint32_t val = 0;
	int ret = swd_read_dp(0x0, &val);
	if (ret == 0)
	{
		*idcode = val;
		ESP_LOGI(TAG, "IDCODE = 0x%08x", val);
	}
	else
	{
		ESP_LOGE(TAG, "swd_read_dp failed, ret=%d", ret);
	}
	return ret;
}

int swd_start(void)
{
	uint32_t v;
	int ret;

	// 0. Xoá toàn bộ sticky error bằng ABORT
	//    STKERRCLR | STKCMPCLR | WDERRCLR | ORUNERRCLR = 0x1E
	ret = swd_write_dp(DP_REG_ABORT /*ABORT*/, 0x0000001E);
	if (ret != 0)
	{
		ESP_LOGE(TAG, "swd_start: write ABORT failed, ret=%d", ret);
		return -1;
	}

	// 1. Đọc thử CTRL/STAT (DP[0x4]) – giống ST-LINK: request READ CTRL/STAT
	ret = swd_read_dp(DP_REG_CTRL_STAT /*CTRL/STAT*/, &v);
	if (ret != 0)
	{
		ESP_LOGE(TAG, "swd_start: first READ CTRL/STAT failed, ret=%d", ret);
		return -2;
	}
	// ESP_LOGI(TAG, "CTRL/STAT before power-up = 0x%08" PRIX32, v);
	swd_delay();
	swd_delay();
	swd_delay();

	// 2. Ghi yêu cầu power-up:
	//    thường set cả 2 bit CDBGPWRUPREQ và CSYSPWRUPREQ -> 0x50000000
	ret = swd_write_dp(DP_REG_CTRL_STAT /*CTRL/STAT*/, 0x50000000);
	if (ret != 0)
	{
		ESP_LOGE(TAG, "swd_start: WRITE CTRL/STAT power-up failed, ret=%d", ret);
		return -3;
	}

	// 3. Poll đợi 2 bit ACK lên 1: (CDBGPWRUPACK | CSYSPWRUPACK)
	//    Tương đương (v & 0xA0000000) == 0xA0000000
	for (int i = 0; i < 50; i++)
	{ // cho timeout tránh lock
		ret = swd_read_dp(DP_REG_CTRL_STAT /*CTRL/STAT*/, &v);
		if (ret != 0)
		{
			ESP_LOGE(TAG, "swd_start: READ CTRL/STAT poll failed, ret=%d", ret);
			return -4;
		}

		if ((v & 0xA0000000) == 0xA0000000)
		{
			ESP_LOGI(TAG, "CTRL/STAT power-up OK = 0x%08" PRIX32, v);
			return 0;
		}
	}

	ESP_LOGE(TAG, "swd_start: timeout waiting for power-up ACK, last CTRL/STAT=0x%08" PRIX32, v);
	return -5;
}

int swd_read_ap_idr(uint32_t *idr)
{
	if (!idr)
		return -10;

	// APSEL=0, APBANKSEL=0xF để đọc IDR tại addr 0xC
	int ret = swd_write_dp(DP_REG_SELECT, 0x000000F0);
	if (ret != 0)
		return -1;

	ret = swd_read_ap(AP_REG_DRW, idr);
	if (ret != 0)
		return -2;

	ESP_LOGI(TAG, "AP0 IDR = 0x%08" PRIX32, *idr);
	return 0;
}

int swd_read_target_cpuid(uint32_t *cpuid)
{
	if (!cpuid)
		return -10;

	int ret = swd_write_dp(DP_REG_SELECT, 0x00000000);
	if (ret != 0)
		return -1;

	ret = swd_write_ap(AP_REG_CSW, AP_CSW_32BIT_NOINC);
	if (ret != 0)
		return -2;

	ret = swd_write_ap(AP_REG_TAR, ARM_CPUID_ADDR);
	if (ret != 0)
		return -3;

	ret = swd_read_ap(AP_REG_DRW, cpuid);
	if (ret != 0)
		return -4;

	ESP_LOGI(TAG, "Target CPUID = 0x%08" PRIX32, *cpuid);
	return 0;
}

int swd_mem_read32(uint32_t addr, uint32_t *value)
{
	if (!value)
		return -10;

	int ret = swd_write_dp(DP_REG_SELECT, 0x00000000);
	if (ret != 0)
		return -1;

	ret = swd_write_ap(AP_REG_CSW, AP_CSW_32BIT_NOINC);
	if (ret != 0)
		return -2;

	ret = swd_write_ap(AP_REG_TAR, addr);
	if (ret != 0)
		return -3;

	ret = swd_read_ap(AP_REG_DRW, value);
	if (ret != 0)
		return -4;

	return 0;
}

int swd_mem_write32(uint32_t addr, uint32_t value)
{
	int ret = swd_write_dp(DP_REG_SELECT, 0x00000000);
	if (ret != 0)
		return -1;

	ret = swd_write_ap(AP_REG_CSW, AP_CSW_32BIT_NOINC);
	if (ret != 0)
		return -2;

	ret = swd_write_ap(AP_REG_TAR, addr);
	if (ret != 0)
		return -3;

	ret = swd_write_ap(AP_REG_DRW, value);
	if (ret != 0)
		return -4;

	return 0;
}

int swd_mem_write16(uint32_t addr, uint16_t value)
{
	if (addr & 0x1UL)
		return -10;

	int ret = swd_write_dp(DP_REG_SELECT, 0x00000000);
	if (ret != 0)
		return -1;

	ret = swd_write_ap(AP_REG_CSW, AP_CSW_16BIT_NOINC);
	if (ret != 0)
		return -2;

	ret = swd_write_ap(AP_REG_TAR, addr);
	if (ret != 0)
		return -3;

	uint32_t drw = (uint32_t)value;
	if (addr & 0x2UL)
	{
		drw <<= 16;
	}

	ret = swd_write_ap(AP_REG_DRW, drw);
	if (ret != 0)
		return -4;

	return 0;
}

int swd_mem_write_block(uint32_t addr, const uint8_t *data, uint32_t len)
{
	if (!data || len == 0)
		return -10;

	uint32_t off = 0;
	while (off < len)
	{
		uint32_t cur_addr = addr + off;
		if ((cur_addr & 0x3UL) == 0 && (len - off) >= 4U)
		{
			uint32_t w = (uint32_t)data[off] |
					 ((uint32_t)data[off + 1] << 8) |
					 ((uint32_t)data[off + 2] << 16) |
					 ((uint32_t)data[off + 3] << 24);
			int ret = swd_mem_write32(cur_addr, w);
			if (ret != 0)
				return -1;
			off += 4U;
			continue;
		}

		uint32_t aligned = cur_addr & ~0x3UL;
		uint32_t word = 0;
		int ret = swd_mem_read32(aligned, &word);
		if (ret != 0)
			return -2;

		uint32_t lane = cur_addr & 0x3UL;
		word &= ~(0xFFUL << (lane * 8U));
		word |= ((uint32_t)data[off] << (lane * 8U));

		ret = swd_mem_write32(aligned, word);
		if (ret != 0)
			return -3;

		off++;
	}

	return 0;
}

static int swd_core_access_reg(uint8_t reg, uint32_t *value, int write)
{
	if (!value || reg > ARM_CORE_REG_XPSR)
	{
		return -10;
	}

	if (write)
	{
		if (swd_mem_write32(ARM_DCRDR_ADDR, *value) != 0)
			return -1;
	}

	uint32_t dcrsr = (uint32_t)reg;
	if (write)
	{
		dcrsr |= (1UL << 16); // REGWnR
	}

	if (swd_mem_write32(ARM_DCRSR_ADDR, dcrsr) != 0)
		return -2;

	for (int i = 0; i < 100; i++)
	{
		uint32_t dhcsr = 0;
		if (swd_mem_read32(ARM_DHCSR_ADDR, &dhcsr) != 0)
			return -3;
		if (dhcsr & ARM_DHCSR_S_REGRDY)
		{
			if (!write)
			{
				if (swd_mem_read32(ARM_DCRDR_ADDR, value) != 0)
					return -4;
			}
			return 0;
		}
		vTaskDelay(pdMS_TO_TICKS(1));
	}

	return -5;
}

int swd_read_core_dhcsr(uint32_t *dhcsr)
{
	if (!dhcsr)
		return -10;

	int ret = swd_mem_read32(ARM_DHCSR_ADDR, dhcsr);
	if (ret != 0)
		return -1;

	ESP_LOGI(TAG, "DHCSR = 0x%08" PRIX32, *dhcsr);
	return 0;
}

int swd_halt_core(uint32_t *dhcsr_after)
{
	uint32_t v = 0;
	int ret = swd_mem_write32(ARM_DHCSR_ADDR, ARM_DHCSR_DBGKEY | ARM_DHCSR_C_DEBUGEN | ARM_DHCSR_C_HALT);
	if (ret != 0)
		return -1;

	ret = swd_mem_read32(ARM_DHCSR_ADDR, &v);
	if (ret != 0)
		return -2;

	if (dhcsr_after)
		*dhcsr_after = v;

	if ((v & ARM_DHCSR_S_HALT) == 0)
		return -3;

	ESP_LOGI(TAG, "Core halted, DHCSR = 0x%08" PRIX32, v);
	return 0;
}

int swd_resume_core(uint32_t *dhcsr_after)
{
	uint32_t v = 0;
	int ret = swd_mem_write32(ARM_DHCSR_ADDR, ARM_DHCSR_DBGKEY | ARM_DHCSR_C_DEBUGEN);
	if (ret != 0)
		return -1;

	ret = swd_mem_read32(ARM_DHCSR_ADDR, &v);
	if (ret != 0)
		return -2;

	if (dhcsr_after)
		*dhcsr_after = v;

	ESP_LOGI(TAG, "Core resumed, DHCSR = 0x%08" PRIX32, v);
	return 0;
}

int swd_core_call(uint32_t func_addr,
		  uint32_t lr_addr,
		  uint32_t sp,
		  uint32_t r9,
		  const uint32_t args[4],
		  uint32_t *out_r0,
		  uint32_t timeout_ms)
{
	if (!args || func_addr == 0 || lr_addr == 0 || sp == 0)
	{
		return -10;
	}

	uint32_t dhcsr = 0;
	if (swd_halt_core(&dhcsr) != 0)
		return -1;

	// Clear debug event status flags before run.
	if (swd_mem_write32(ARM_DFSR_ADDR, 0x1FUL) != 0)
		return -2;

	uint32_t r0 = args[0];
	uint32_t r1 = args[1];
	uint32_t r2 = args[2];
	uint32_t r3 = args[3];
	uint32_t r13 = sp;
	uint32_t r14 = (lr_addr | 1UL);
	uint32_t r15 = (func_addr | 1UL);
	uint32_t xpsr = ARM_XPSR_T_BIT;

	if (swd_core_access_reg(ARM_CORE_REG_R0, &r0, 1) != 0)
		return -3;
	if (swd_core_access_reg(ARM_CORE_REG_R1, &r1, 1) != 0)
		return -4;
	if (swd_core_access_reg(ARM_CORE_REG_R2, &r2, 1) != 0)
		return -5;
	if (swd_core_access_reg(ARM_CORE_REG_R3, &r3, 1) != 0)
		return -6;
	if (swd_core_access_reg(ARM_CORE_REG_R9, &r9, 1) != 0)
		return -7;
	if (swd_core_access_reg(ARM_CORE_REG_R13, &r13, 1) != 0)
		return -8;
	if (swd_core_access_reg(ARM_CORE_REG_R14, &r14, 1) != 0)
		return -9;
	if (swd_core_access_reg(ARM_CORE_REG_R15, &r15, 1) != 0)
		return -11;
	if (swd_core_access_reg(ARM_CORE_REG_XPSR, &xpsr, 1) != 0)
		return -12;

	if (swd_resume_core(&dhcsr) != 0)
		return -13;

	uint32_t wait_ms = (timeout_ms == 0) ? 1000U : timeout_ms;
	for (uint32_t i = 0; i < wait_ms; i++)
	{
		if (swd_mem_read32(ARM_DHCSR_ADDR, &dhcsr) != 0)
			return -14;
		if (dhcsr & ARM_DHCSR_S_HALT)
		{
			uint32_t ret_r0 = 0;
			if (swd_core_access_reg(ARM_CORE_REG_R0, &ret_r0, 0) != 0)
				return -15;
			if (out_r0)
			{
				*out_r0 = ret_r0;
			}
			return 0;
		}
		vTaskDelay(pdMS_TO_TICKS(1));
	}

	(void)swd_halt_core(&dhcsr);
	return -16;
}

int swd_read_dbgmcu_idcode(uint32_t *idcode)
{
	if (!idcode)
		return -10;

	uint32_t v = 0;
	int ret = swd_mem_read32(STM32_DBGMCU_IDCODE_AHB, &v);
	if (ret == 0 && v != 0x00000000UL && v != 0xFFFFFFFFUL)
	{
		*idcode = v;
		ESP_LOGI(TAG, "DBGMCU_IDCODE(AHB) = 0x%08" PRIX32, v);
		return 0;
	}

	ret = swd_mem_read32(STM32_DBGMCU_IDCODE_APB, &v);
	if (ret == 0 && v != 0x00000000UL && v != 0xFFFFFFFFUL)
	{
		*idcode = v;
		ESP_LOGI(TAG, "DBGMCU_IDCODE(APB) = 0x%08" PRIX32, v);
		return 0;
	}

	return -1;
}

int swd_read_flash_size_kb(uint32_t *flash_kb)
{
	if (!flash_kb)
		return -10;

	// Chon thu tu probe theo DEV_ID de giam nguy co FAULT ACK tren dia chi sai.
	static const uint32_t probe_addr[] = {
		0x1FFF7A22UL, // F1/F3/L1 (16-bit)
		0x1FFFF7CCUL, // F0 (16-bit)
		0x1FFF75E0UL, // G0 (16-bit)
	};

	size_t probe_count = sizeof(probe_addr) / sizeof(probe_addr[0]);

	for (size_t i = 0; i < probe_count; i++)
	{
		uint32_t addr = probe_addr[i];
		uint32_t aligned = addr & ~0x3UL;
		uint32_t word = 0;

		int ret = swd_mem_read32(aligned, &word);
		if (ret != 0)
		{
			// Khi probe nham dia chi, target co the tra FAULT va set sticky error.
			(void)swd_write_dp(DP_REG_ABORT, 0x0000001E);
			continue;
		}

		uint32_t shift = (addr & 0x2UL) ? 16U : 0U;
		uint32_t kb = (word >> shift) & 0xFFFFUL;

		if (kb >= 8UL && kb <= 4096UL)
		{
			*flash_kb = kb;
			ESP_LOGI(TAG, "FLASH_SIZE = %" PRIu32 " KB", kb);
			return 0;
		}
	}

	return -1;
}

int swd_target_reset_run(void)
{
	int ret = swd_mem_write32(ARM_AIRCR_ADDR, ARM_AIRCR_VECTKEY | ARM_AIRCR_SYSRESETREQ);
	if (ret != 0)
	{
		return -1;
	}

	vTaskDelay(pdMS_TO_TICKS(40));
	ESP_LOGI(TAG, "Target reset requested");
	return 0;
}
