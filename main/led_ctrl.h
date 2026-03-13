#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LED_RED_GPIO   12
#define LED_GREEN_GPIO 14

typedef enum
{
	LED_COLOR_RED = 0,
	LED_COLOR_GREEN,
	LED_COLOR_BOTH,
} led_color_t;

// Initialize GPIOs and internal timer engine.
esp_err_t led_ctrl_init(void);

// 1) Blink one LED (or both) with periodic toggle.
esp_err_t led_blink_start(led_color_t color, uint32_t interval_ms);

// 2) Alternate red and green blinking.
esp_err_t led_blink_alternate_start(uint32_t interval_ms);

// Stop blinking effects and keep current level.
esp_err_t led_effect_stop(void);

// 3) Set LED steady ON/OFF.
esp_err_t led_set_level(bool red_on, bool green_on);

// Helpers for common states.
void led_set_ready(void);
void led_set_error(void);

#ifdef __cplusplus
}
#endif
