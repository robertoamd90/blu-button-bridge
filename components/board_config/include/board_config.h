#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BOARD_CONFIG_MAX_ACTION_GPIOS 16

int board_config_system_led_gpio(void);
bool board_config_system_led_active_low(void);

int board_config_boot_button_gpio(void);
bool board_config_boot_button_active_low(void);

int board_config_get_allowed_action_gpios(uint8_t *out, int max_count);
