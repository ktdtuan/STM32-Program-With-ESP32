#include "chip_lock_families.h"

#include <inttypes.h>

#include "esp_log.h"

#include "target_swd.h"

static const char *TAG = "LOCK_G0";

#define STM32G0_FLASH_BASE 0x40022000UL
#define STM32G0_FLASH_KEYR (STM32G0_FLASH_BASE + 0x08UL)
#define STM32G0_FLASH_OPTKEYR (STM32G0_FLASH_BASE + 0x0CUL)
#define STM32G0_FLASH_SR (STM32G0_FLASH_BASE + 0x10UL)
#define STM32G0_FLASH_CR (STM32G0_FLASH_BASE + 0x14UL)
#define STM32G0_FLASH_OPTR (STM32G0_FLASH_BASE + 0x20UL)

#define STM32_FLASH_KEY1 0x45670123UL
#define STM32_FLASH_KEY2 0xCDEF89ABUL
#define STM32_OPT_KEY1 0x08192A3BUL
#define STM32_OPT_KEY2 0x4C5D6E7FUL

#define STM32G0_FLASH_CR_OPTSTRT (1UL << 17)
#define STM32G0_FLASH_CR_OBL_LAUNCH (1UL << 27)
#define STM32G0_FLASH_CR_OPTLOCK (1UL << 30)
#define STM32G0_FLASH_CR_LOCK (1UL << 31)

#define STM32G0_FLASH_SR_BSY1 (1UL << 16)
#define STM32G0_FLASH_SR_CFGBSY (1UL << 18)

#define STM32_RDP_LEVEL_0 0xAAUL
#define STM32_RDP_LEVEL_1 0xBBUL
#define STM32_RDP_LEVEL_2 0xCCUL

static uint32_t g0_rdp_value_from_level(uint8_t lock_level)
{
    switch (lock_level)
    {
    case 0:
        return STM32_RDP_LEVEL_0;
    case 1:
        return STM32_RDP_LEVEL_1;
    case 2:
        return STM32_RDP_LEVEL_2;
    default:
        return 0;
    }
}

static int g0_wait_flash_ready(uint32_t timeout_ms)
{
    uint32_t wait = (timeout_ms == 0U) ? 500U : timeout_ms;
    for (uint32_t i = 0; i < wait; i++)
    {
        uint32_t sr = 0;
        if (swd_mem_read32(STM32G0_FLASH_SR, &sr) != 0)
        {
            return -1;
        }
        if ((sr & (STM32G0_FLASH_SR_BSY1 | STM32G0_FLASH_SR_CFGBSY)) == 0U)
        {
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return -2;
}

esp_err_t chip_lock_apply_level_g0(uint8_t lock_level)
{
    uint32_t rdp = g0_rdp_value_from_level(lock_level);
    if (rdp == 0U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (g0_wait_flash_ready(1000U) != 0)
    {
        return ESP_FAIL;
    }

    uint32_t cr = 0;
    if (swd_mem_read32(STM32G0_FLASH_CR, &cr) != 0)
    {
        return ESP_FAIL;
    }

    if (cr & STM32G0_FLASH_CR_LOCK)
    {
        if (swd_mem_write32(STM32G0_FLASH_KEYR, STM32_FLASH_KEY1) != 0 ||
            swd_mem_write32(STM32G0_FLASH_KEYR, STM32_FLASH_KEY2) != 0)
        {
            return ESP_FAIL;
        }
    }

    if (swd_mem_read32(STM32G0_FLASH_CR, &cr) != 0)
    {
        return ESP_FAIL;
    }

    if (cr & STM32G0_FLASH_CR_OPTLOCK)
    {
        if (swd_mem_write32(STM32G0_FLASH_OPTKEYR, STM32_OPT_KEY1) != 0 ||
            swd_mem_write32(STM32G0_FLASH_OPTKEYR, STM32_OPT_KEY2) != 0)
        {
            return ESP_FAIL;
        }
    }

    uint32_t optr = 0;
    if (swd_mem_read32(STM32G0_FLASH_OPTR, &optr) != 0)
    {
        return ESP_FAIL;
    }

    uint32_t old_rdp = optr & 0xFFUL;
    if (old_rdp == rdp)
    {
        ESP_LOGI(TAG, "RDP already at level=%u (0x%02" PRIX32 ")", (unsigned)lock_level, rdp);
        return ESP_OK;
    }

    optr = (optr & ~0xFFUL) | rdp;
    if (swd_mem_write32(STM32G0_FLASH_OPTR, optr) != 0)
    {
        return ESP_FAIL;
    }

    if (swd_mem_read32(STM32G0_FLASH_CR, &cr) != 0)
    {
        return ESP_FAIL;
    }
    cr |= STM32G0_FLASH_CR_OPTSTRT;
    if (swd_mem_write32(STM32G0_FLASH_CR, cr) != 0)
    {
        return ESP_FAIL;
    }

    if (g0_wait_flash_ready(5000U) != 0)
    {
        return ESP_FAIL;
    }

    if (swd_mem_read32(STM32G0_FLASH_CR, &cr) != 0)
    {
        return ESP_FAIL;
    }
    cr |= STM32G0_FLASH_CR_OBL_LAUNCH;
    if (swd_mem_write32(STM32G0_FLASH_CR, cr) != 0)
    {
        return ESP_FAIL;
    }

    ESP_LOGW(TAG, "Applied RDP level=%u (0x%02" PRIX32 "), target will reset", (unsigned)lock_level, rdp);
    return ESP_OK;
}
