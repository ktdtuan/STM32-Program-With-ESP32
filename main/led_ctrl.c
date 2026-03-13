#include "led_ctrl.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "LED_CTRL";

typedef enum
{
	LED_EFFECT_NONE = 0,
	LED_EFFECT_BLINK,
	LED_EFFECT_ALTERNATE,
} led_effect_t;

static esp_timer_handle_t s_led_timer = NULL;
static bool s_initialized = false;

static led_effect_t s_effect = LED_EFFECT_NONE;
static led_color_t s_blink_color = LED_COLOR_RED;
static bool s_toggle_state = false;

static bool s_red_level = false;
static bool s_green_level = false;

static void apply_levels(bool red_on, bool green_on)
{
	s_red_level = red_on;
	s_green_level = green_on;
	(void)gpio_set_level(LED_RED_GPIO, red_on ? 1 : 0);
	(void)gpio_set_level(LED_GREEN_GPIO, green_on ? 1 : 0);
}

static void led_timer_cb(void *arg)
{
	(void)arg;

	s_toggle_state = !s_toggle_state;

	if (s_effect == LED_EFFECT_BLINK)
	{
		switch (s_blink_color)
		{
		case LED_COLOR_RED:
			apply_levels(s_toggle_state, false);
			break;
		case LED_COLOR_GREEN:
			apply_levels(false, s_toggle_state);
			break;
		case LED_COLOR_BOTH:
			apply_levels(s_toggle_state, s_toggle_state);
			break;
		default:
			break;
		}
	}
	else if (s_effect == LED_EFFECT_ALTERNATE)
	{
		if (s_toggle_state)
		{
			apply_levels(true, false);
		}
		else
		{
			apply_levels(false, true);
		}
	}
}

static esp_err_t ensure_timer_created(void)
{
	if (s_led_timer)
	{
		return ESP_OK;
	}

	const esp_timer_create_args_t args = {
		.callback = led_timer_cb,
		.arg = NULL,
		.dispatch_method = ESP_TIMER_TASK,
		.name = "led_fx",
		.skip_unhandled_events = true,
	};

	return esp_timer_create(&args, &s_led_timer);
}

esp_err_t led_ctrl_init(void)
{
	if (s_initialized)
	{
		return ESP_OK;
	}

	gpio_config_t io = {
		.pin_bit_mask = (1ULL << LED_RED_GPIO) | (1ULL << LED_GREEN_GPIO),
		.mode = GPIO_MODE_OUTPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};

	esp_err_t ret = gpio_config(&io);
	if (ret != ESP_OK)
	{
		return ret;
	}

	ret = ensure_timer_created();
	if (ret != ESP_OK)
	{
		return ret;
	}

	apply_levels(false, false);
	s_effect = LED_EFFECT_NONE;
	s_blink_color = LED_COLOR_RED;
	s_toggle_state = false;
	s_initialized = true;

	ESP_LOGI(TAG, "LED init done: red=%d green=%d", LED_RED_GPIO, LED_GREEN_GPIO);
	return ESP_OK;
}

esp_err_t led_effect_stop(void)
{
	if (!s_initialized)
	{
		return ESP_ERR_INVALID_STATE;
	}

	if (s_led_timer && esp_timer_is_active(s_led_timer))
	{
		(void)esp_timer_stop(s_led_timer);
	}
	s_effect = LED_EFFECT_NONE;
	return ESP_OK;
}

esp_err_t led_set_level(bool red_on, bool green_on)
{
	if (!s_initialized)
	{
		return ESP_ERR_INVALID_STATE;
	}

	(void)led_effect_stop();
	apply_levels(red_on, green_on);
	return ESP_OK;
}

esp_err_t led_blink_start(led_color_t color, uint32_t interval_ms)
{
	if (!s_initialized)
	{
		return ESP_ERR_INVALID_STATE;
	}
	if (interval_ms == 0)
	{
		return ESP_ERR_INVALID_ARG;
	}

	if (s_led_timer && esp_timer_is_active(s_led_timer))
	{
		(void)esp_timer_stop(s_led_timer);
	}

	s_effect = LED_EFFECT_BLINK;
	s_blink_color = color;
	s_toggle_state = false;
	apply_levels(false, false);

	return esp_timer_start_periodic(s_led_timer, (uint64_t)interval_ms * 1000ULL);
}

esp_err_t led_blink_alternate_start(uint32_t interval_ms)
{
	if (!s_initialized)
	{
		return ESP_ERR_INVALID_STATE;
	}
	if (interval_ms == 0)
	{
		return ESP_ERR_INVALID_ARG;
	}

	if (s_led_timer && esp_timer_is_active(s_led_timer))
	{
		(void)esp_timer_stop(s_led_timer);
	}

	s_effect = LED_EFFECT_ALTERNATE;
	s_toggle_state = false;
	apply_levels(true, false);

	return esp_timer_start_periodic(s_led_timer, (uint64_t)interval_ms * 1000ULL);
}

void led_set_ready(void)
{
    (void)led_effect_stop();
    (void)led_set_level(false, true);
}

void led_set_error(void)
{
    (void)led_effect_stop();
    (void)led_set_level(true, false);
}
