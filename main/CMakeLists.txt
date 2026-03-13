# Define source files
set(SOURCES "web_server.c"
			"web_dashboard.c"
			"web_manual.c"
			"web_scenario.c"
            "storage.c"
			"hex2bin.c"
            "flm_parser.c"
			"flm_runtime.c"
            "target_swd.c"
			"chip_lock/chip_lock.c"
			"chip_lock/chip_lock_g0.c"
			"chip_lock/chip_lock_fx.c"
			"stm32_programmer.c"
			"led_ctrl.c"
			"wifi_manager.c"
			"main.c")

# Define include directories
set(INCLUDE_DIS ".")

# Embedded web files
file(GLOB WEB_BROWERS ${CMAKE_CURRENT_SOURCE_DIR}/web/*.html)

# Register the component
idf_component_register(SRCS ${SOURCES}
						EMBED_FILES ${WEB_BROWERS}
						INCLUDE_DIRS ${INCLUDE_DIS}
						REQUIRES esp_http_server 
									esp_wifi 
									nvs_flash 
									driver 
									json 
									esp_timer
									app_update
									littlefs
						)