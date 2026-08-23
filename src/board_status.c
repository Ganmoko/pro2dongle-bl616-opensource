// SPDX-License-Identifier: GPL-3.0-only
#include "board_status.h"

#include "board.h"
#include "bflb_gpio.h"
#include "FreeRTOS.h"
#include "task.h"

#include "board_config.h"
#include "pro2_ble.h"
#include "usb_nintendo.h"

static struct bflb_device_s *gpio;
static volatile board_status_t current_status = BOARD_STATUS_BOOT;

static void led_set(bool on)
{
    if (!gpio) {
        return;
    }
#if LED_ACTIVE_LOW
    if (on) {
        bflb_gpio_reset(gpio, LED_PIN);
    } else {
        bflb_gpio_set(gpio, LED_PIN);
    }
#else
    if (on) {
        bflb_gpio_set(gpio, LED_PIN);
    } else {
        bflb_gpio_reset(gpio, LED_PIN);
    }
#endif
}

void board_status_init(void)
{
    gpio = bflb_device_get_by_name("gpio");
    bflb_gpio_init(gpio, LED_PIN,
                   GPIO_OUTPUT | GPIO_PULLUP | GPIO_SMT_EN | GPIO_DRV_0);
    bflb_gpio_init(gpio, BOOT_BUTTON_PIN,
                   GPIO_INPUT | GPIO_FLOAT | GPIO_SMT_EN);
    led_set(false);
}

void board_status_set(board_status_t status)
{
    current_status = status;
}

bool board_button_pressed(void)
{
    return gpio && bflb_gpio_read(gpio, BOOT_BUTTON_PIN) != 0;
}

static void show_short_pulses(uint8_t count)
{
    for (uint8_t i = 0; i < count; ++i) {
        led_set(true);
        vTaskDelay(pdMS_TO_TICKS(180));
        led_set(false);
        vTaskDelay(pdMS_TO_TICKS(320));
    }
}

static void show_ble_error_code(uint8_t code)
{
    led_set(false);
    vTaskDelay(pdMS_TO_TICKS(600));

    if (code == 0) {
        if (!usb_nintendo_ready()) {
            /* BLE is healthy but USB is not configured: 9 / USB stage. */
            show_short_pulses(9);
            vTaskDelay(pdMS_TO_TICKS(900));
            uint8_t stage = usb_nintendo_led_stage();
            show_short_pulses(stage ? stage : 1u);
            vTaskDelay(pdMS_TO_TICKS(800));
            return;
        }
        /* A single long pulse means that no BLE failure is recorded. */
        led_set(true);
        vTaskDelay(pdMS_TO_TICKS(1200));
        led_set(false);
        vTaskDelay(pdMS_TO_TICKS(600));
        return;
    }

    show_short_pulses(code);

    uint8_t detail = pro2_ble_led_error_detail();
    if (detail) {
        vTaskDelay(pdMS_TO_TICKS(900));
        show_short_pulses(detail);
    }

    uint8_t value = pro2_ble_led_error_value();
    if (value) {
        vTaskDelay(pdMS_TO_TICKS(900));
        show_short_pulses(value);
    }
    vTaskDelay(pdMS_TO_TICKS(800));
}

void board_status_task(void *arg)
{
    (void)arg;
    uint32_t tick = 0;
    uint32_t held = 0;
    bool handled = false;

    for (;;) {
        bool on;
        switch (current_status) {
        case BOARD_STATUS_READY:
            on = true;
            break;
        case BOARD_STATUS_CONNECTING:
            on = ((tick / 2u) & 1u) != 0;       /* 5 Hz */
            break;
        case BOARD_STATUS_ERROR:
            on = ((tick / 10u) & 1u) != 0;      /* 1 Hz */
            break;
        case BOARD_STATUS_BOOT:
        case BOARD_STATUS_SCANNING:
        default:
            on = ((tick / 5u) & 1u) != 0;       /* 2 Hz */
            break;
        }
        led_set(on);
        tick++;

        if (board_button_pressed()) {
            if (held < 1000u) {
                held++;
            }
            if (held >= 30u && !handled) {       /* hold three seconds */
                pro2_ble_forget_peer();
                handled = true;
            }
        } else {
            if (held > 0u && held < 30u && !handled) {
                show_ble_error_code(pro2_ble_led_error_code());
            }
            held = 0;
            handled = false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
