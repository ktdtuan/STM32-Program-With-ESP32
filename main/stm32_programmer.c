#include "stm32_programmer.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "chip_lock/chip_lock.h"
#include "target_swd.h"

static const char *TAG = "STM32_PROG";

#define STM32Gx_FLASH_OPTR 0x40022020UL
#define STM32Fx_OB_RDP_ADDR 0x1FFFF800UL

static uint8_t decode_rdp_level(uint8_t raw, uint8_t level0_code, uint8_t level1_code, uint8_t level2_code)
{
	if (raw == level0_code)
	{
		return 0;
	}
	if (raw == level2_code)
	{
		return 2;
	}
	if (raw == level1_code)
	{
		return 1;
	}
	// Any non-level0/non-level2 state is treated as protected for safe behavior.
	return 1;
}

const char *stm32_family_token_from_devid(uint32_t dev_id)
{
	switch (dev_id & 0x0FFFUL)
	{
	case 0x440:
		return "f0";
	case 0x460:
		return "g0";
	case 0x468:
		return "g4";
	case 0x410:
	case 0x412:
	case 0x414:
	case 0x418:
		return "f1";
	case 0x422:
	case 0x432:
	case 0x438:
	case 0x446:
		return "f3";
	case 0x411:
		return "f2";
	case 0x413:
	case 0x419:
	case 0x421:
	case 0x423:
	case 0x431:
	case 0x433:
		return "f4";
	default:
		return "unknown";
	}
}

esp_err_t stm32_detect_profile(stm32_chip_profile_t *out_profile, uint32_t *out_dbgmcu_idcode)
{
	if (!out_profile)
	{
		return ESP_ERR_INVALID_ARG;
	}

	uint32_t dp_idcode = 0;
	int ret = swd_read_idcode(&dp_idcode);
	if (ret != 0)
	{
		return ESP_FAIL;
	}

	ret = swd_start();
	if (ret != 0)
	{
		return ESP_FAIL;
	}

	uint32_t dbgmcu = 0;
	ret = swd_read_dbgmcu_idcode(&dbgmcu);
	if (ret != 0)
	{
		return ESP_FAIL;
	}

	uint32_t dev_id = dbgmcu & 0x0FFFUL;
	const char *family = stm32_family_token_from_devid(dev_id);
	memset(out_profile, 0, sizeof(*out_profile));
	out_profile->dev_id = dev_id;
	out_profile->family = family;
	out_profile->supports_direct_flash = (dev_id == 0x440UL || dev_id == 0x460UL);

	if (out_dbgmcu_idcode)
	{
		*out_dbgmcu_idcode = dbgmcu;
	}

	ESP_LOGI(TAG, "Detected DEV_ID=0x%03" PRIX32 " family=%s", dev_id, family);
	return ESP_OK;
}

esp_err_t stm32_flash_firmware_stream(FILE *firmware_fp,
									  uint32_t base_addr,
									  uint32_t max_bytes,
									  uint32_t *out_written_bytes)
{
	(void)firmware_fp;
	(void)base_addr;
	(void)max_bytes;
	(void)out_written_bytes;
	ESP_LOGW(TAG, "stm32_flash_firmware_stream disabled (native path removed, use FLM runtime)");
	return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t stm32_flash_firmware_buffer(const uint8_t *firmware,
						  uint32_t firmware_size,
						  uint32_t base_addr,
						  uint32_t max_bytes,
						  uint32_t *out_written_bytes)
{
	(void)firmware;
	(void)firmware_size;
	(void)base_addr;
	(void)max_bytes;
	(void)out_written_bytes;
	ESP_LOGW(TAG, "stm32_flash_firmware_buffer disabled (native path removed, use FLM runtime)");
	return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t stm32_apply_lock_level(uint32_t dev_id, uint8_t lock_level)
{
	return chip_lock_apply_level(dev_id, lock_level);
}

esp_err_t stm32_get_lock_level(uint32_t dev_id, uint8_t *out_lock_level)
{
	if (!out_lock_level)
	{
		return ESP_ERR_INVALID_ARG;
	}

	uint32_t did = dev_id & 0x0FFFUL;
	uint32_t v = 0;

	switch (did)
	{
	case 0x460: // G0
	case 0x468: // G4
		if (swd_mem_read32(STM32Gx_FLASH_OPTR, &v) != 0)
		{
			return ESP_FAIL;
		}
		*out_lock_level = decode_rdp_level((uint8_t)(v & 0xFFU), 0xAAU, 0xBBU, 0xCCU);
		return ESP_OK;

	case 0x440: // F0
		if (swd_mem_read32(STM32Fx_OB_RDP_ADDR, &v) != 0)
		{
			return ESP_FAIL;
		}
		*out_lock_level = decode_rdp_level((uint8_t)(v & 0xFFU), 0xAAU, 0xBBU, 0xCCU);
		return ESP_OK;

	case 0x410:
	case 0x412:
	case 0x414:
	case 0x418: // F1
	case 0x422:
	case 0x432:
	case 0x438:
	case 0x446: // F3 (shared with F1-style flow)
		if (swd_mem_read32(STM32Fx_OB_RDP_ADDR, &v) != 0)
		{
			return ESP_FAIL;
		}
		*out_lock_level = decode_rdp_level((uint8_t)(v & 0xFFU), 0xA5U, 0x00U, 0xCCU);
		return ESP_OK;

	default:
		return ESP_ERR_NOT_SUPPORTED;
	}
}
