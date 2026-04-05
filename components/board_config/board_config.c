#include <string.h>
#include "sdkconfig.h"
#include "board_config.h"

#if CONFIG_BBB_BOARD_ESP32_DEVKIT_V1

static const uint8_t s_allowed_action_gpios[] = {
    16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33,
};

int board_config_system_led_gpio(void)
{
    return 2;
}

bool board_config_system_led_active_low(void)
{
    return false;
}

int board_config_boot_button_gpio(void)
{
    return 0;
}

bool board_config_boot_button_active_low(void)
{
    return true;
}

#elif CONFIG_BBB_BOARD_ESP32C3_SUPERMINI

static const uint8_t s_allowed_action_gpios[] = {
    0, 1, 3, 4, 5, 6, 7, 10, 20, 21,
};

int board_config_system_led_gpio(void)
{
    return 8;
}

bool board_config_system_led_active_low(void)
{
    return true;
}

int board_config_boot_button_gpio(void)
{
    return 9;
}

bool board_config_boot_button_active_low(void)
{
    return true;
}

#else
#error "Unsupported BluButtonBridge board profile"
#endif

int board_config_get_allowed_action_gpios(uint8_t *out, int max_count)
{
    int count = (int)(sizeof(s_allowed_action_gpios) / sizeof(s_allowed_action_gpios[0]));
    if (!out || max_count <= 0) {
        return count;
    }

    if (count > max_count) {
        count = max_count;
    }

    memcpy(out, s_allowed_action_gpios, (size_t)count);
    return count;
}
