#include "chip_lock_families.h"

#include <inttypes.h>

#include "esp_log.h"

#include "target_swd.h"

#define STM32FX_FLASH_BASE 0x40022000UL
#define STM32FX_FLASH_KEYR (STM32FX_FLASH_BASE + 0x04UL)
#define STM32FX_FLASH_OPTKEYR (STM32FX_FLASH_BASE + 0x08UL)
#define STM32FX_FLASH_SR (STM32FX_FLASH_BASE + 0x0CUL)
#define STM32FX_FLASH_CR (STM32FX_FLASH_BASE + 0x10UL)

#define STM32FX_OB_RDP_ADDR 0x1FFFF800UL

#define STM32_FLASH_KEY1 0x45670123UL
#define STM32_FLASH_KEY2 0xCDEF89ABUL

#define STM32FX_FLASH_CR_OPTPG (1UL << 4)
#define STM32FX_FLASH_CR_OPTER (1UL << 5)
#define STM32FX_FLASH_CR_STRT (1UL << 6)
#define STM32FX_FLASH_CR_LOCK (1UL << 7)
#define STM32FX_FLASH_CR_OPTWRE (1UL << 9)
#define STM32FX_FLASH_CR_OBL_LAUNCH (1UL << 13)

#define STM32FX_FLASH_SR_BSY (1UL << 0)

typedef struct
{
    const char *tag;
    uint8_t rdp_l0;
    uint8_t rdp_l1;
    uint8_t rdp_l2;
    const char *family_name;
} stm32fx_lock_cfg_t;

static uint8_t fx_rdp_byte_from_level(const stm32fx_lock_cfg_t *cfg, uint8_t lock_level)
{
    switch (lock_level)
    {
    case 0:
        return cfg->rdp_l0;
    case 1:
        return cfg->rdp_l1;
    case 2:
        return cfg->rdp_l2;
    default:
        return 0xFFU;
    }
}

static int fx_wait_flash_ready(uint32_t timeout_ms)
{
    uint32_t wait = (timeout_ms == 0U) ? 500U : timeout_ms;
    for (uint32_t i = 0; i < wait; i++)
    {
        uint32_t sr = 0;
        if (swd_mem_read32(STM32FX_FLASH_SR, &sr) != 0)
        {
            return -1;
        }
        if ((sr & STM32FX_FLASH_SR_BSY) == 0U)
        {
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return -2;
}

static int fx_unlock_option_write(void)
{
    uint32_t cr = 0;
    if (swd_mem_read32(STM32FX_FLASH_CR, &cr) != 0)
    {
        return -1;
    }

    if (cr & STM32FX_FLASH_CR_LOCK)
    {
        if (swd_mem_write32(STM32FX_FLASH_KEYR, STM32_FLASH_KEY1) != 0 ||
            swd_mem_write32(STM32FX_FLASH_KEYR, STM32_FLASH_KEY2) != 0)
        {
            return -2;
        }
    }

    if (swd_mem_read32(STM32FX_FLASH_CR, &cr) != 0)
    {
        return -3;
    }

    if ((cr & STM32FX_FLASH_CR_OPTWRE) == 0U)
    {
        if (swd_mem_write32(STM32FX_FLASH_OPTKEYR, STM32_FLASH_KEY1) != 0 ||
            swd_mem_write32(STM32FX_FLASH_OPTKEYR, STM32_FLASH_KEY2) != 0)
        {
            return -4;
        }
    }

    if (swd_mem_read32(STM32FX_FLASH_CR, &cr) != 0)
    {
        return -5;
    }

    if ((cr & STM32FX_FLASH_CR_OPTWRE) == 0U)
    {
        return -6;
    }

    return 0;
}

static esp_err_t chip_lock_apply_level_fx(const stm32fx_lock_cfg_t *cfg, uint8_t lock_level)
{
    uint8_t rdp = fx_rdp_byte_from_level(cfg, lock_level);
    if (rdp == 0xFFU)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (fx_wait_flash_ready(1000U) != 0)
    {
        return ESP_FAIL;
    }

    if (fx_unlock_option_write() != 0)
    {
        return ESP_FAIL;
    }

    uint32_t cr = 0;
    if (swd_mem_read32(STM32FX_FLASH_CR, &cr) != 0)
    {
        return ESP_FAIL;
    }
    cr &= ~(STM32FX_FLASH_CR_OPTPG | STM32FX_FLASH_CR_OPTER);
    cr |= STM32FX_FLASH_CR_OPTER;
    if (swd_mem_write32(STM32FX_FLASH_CR, cr) != 0)
    {
        return ESP_FAIL;
    }

    cr |= STM32FX_FLASH_CR_STRT;
    if (swd_mem_write32(STM32FX_FLASH_CR, cr) != 0)
    {
        return ESP_FAIL;
    }

    if (fx_wait_flash_ready(5000U) != 0)
    {
        return ESP_FAIL;
    }

    cr &= ~STM32FX_FLASH_CR_OPTER;
    cr |= STM32FX_FLASH_CR_OPTPG;
    if (swd_mem_write32(STM32FX_FLASH_CR, cr) != 0)
    {
        return ESP_FAIL;
    }

    uint16_t rdp_half = (uint16_t)rdp | (uint16_t)(((uint8_t)(~rdp)) << 8);
    if (swd_mem_write16(STM32FX_OB_RDP_ADDR, rdp_half) != 0)
    {
        return ESP_FAIL;
    }

    if (fx_wait_flash_ready(3000U) != 0)
    {
        return ESP_FAIL;
    }

    // Stop option-byte programming/erase mode before launching OB reload.
    cr &= ~(STM32FX_FLASH_CR_OPTPG | STM32FX_FLASH_CR_OPTER | STM32FX_FLASH_CR_STRT);
    if (swd_mem_write32(STM32FX_FLASH_CR, cr) != 0)
    {
        return ESP_FAIL;
    }

    uint32_t ob_rdp_raw = 0;
    if (swd_mem_read32(STM32FX_OB_RDP_ADDR, &ob_rdp_raw) != 0)
    {
        return ESP_FAIL;
    }
    uint8_t ob_rdp = (uint8_t)(ob_rdp_raw & 0xFFU);
    if (ob_rdp != rdp)
    {
        ESP_LOGE(cfg->tag,
                 "RDP verify failed: expected=0x%02X read=0x%02X raw=0x%08" PRIX32,
                 rdp,
                 ob_rdp,
                 ob_rdp_raw);
        return ESP_FAIL;
    }

    ESP_LOGW(cfg->tag,
             "Applied %s RDP level=%u (byte=0x%02X half=0x%04X), launching OB reload",
             cfg->family_name,
             (unsigned)lock_level,
             rdp,
             (unsigned)rdp_half);

    cr |= STM32FX_FLASH_CR_OBL_LAUNCH;
    if (swd_mem_write32(STM32FX_FLASH_CR, cr) != 0)
    {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t chip_lock_apply_level_f0(uint8_t lock_level)
{
    static const stm32fx_lock_cfg_t cfg = {
        .tag = "LOCK_F0",
        .rdp_l0 = 0xAAU,
        .rdp_l1 = 0xBBU,
        .rdp_l2 = 0xCCU,
        .family_name = "STM32F0",
    };
    return chip_lock_apply_level_fx(&cfg, lock_level);
}

esp_err_t chip_lock_apply_level_f1(uint8_t lock_level)
{
    static const stm32fx_lock_cfg_t cfg = {
        .tag = "LOCK_F1",
        .rdp_l0 = 0xA5U,
        .rdp_l1 = 0x00U,
        .rdp_l2 = 0xCCU,
        .family_name = "STM32F1",
    };
    return chip_lock_apply_level_fx(&cfg, lock_level);
}
