#include "chip_lock.h"

#include <inttypes.h>

#include "esp_log.h"

#include "chip_lock_families.h"

static const char *TAG = "CHIP_LOCK";

esp_err_t chip_lock_apply_level(uint32_t dev_id, uint8_t lock_level)
{
    if (lock_level > 2U)
    {
        return ESP_ERR_INVALID_ARG;
    }

    switch (dev_id & 0x0FFFUL)
    {
    case 0x460: // STM32G0
    case 0x468: // STM32G4 (compatible option-byte lock flow)
        return chip_lock_apply_level_g0(lock_level);
    case 0x440: // STM32F0
        return chip_lock_apply_level_f0(lock_level);
    case 0x410:
    case 0x412:
    case 0x414:
    case 0x418: // STM32F1
        return chip_lock_apply_level_f1(lock_level);
    case 0x422:
    case 0x432:
    case 0x438:
    case 0x446: // STM32F3 (use Fx/F1-style option-byte flow)
        return chip_lock_apply_level_f1(lock_level);
    case 0x411: // STM32F2
    case 0x413:
    case 0x419:
    case 0x421:
    case 0x423:
    case 0x431:
    case 0x433: // STM32F4
        ESP_LOGW(TAG,
                 "Lock flow for F2/F4 is not integrated yet for DEV_ID=0x%03" PRIX32,
                 dev_id & 0x0FFFUL);
        return ESP_ERR_NOT_SUPPORTED;
    default:
        ESP_LOGW(TAG, "Lock level not supported for DEV_ID=0x%03" PRIX32, dev_id & 0x0FFFUL);
        return ESP_ERR_NOT_SUPPORTED;
    }
}
