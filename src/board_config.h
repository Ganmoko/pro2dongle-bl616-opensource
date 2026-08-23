// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "bflb_gpio.h"

#define BOARD_NAME          "LCTech BL616 QFN32"
#define BOARD_LED_PIN       GPIO_PIN_27
#define BOARD_BUTTON_PIN    GPIO_PIN_2
#define BOARD_LED_ACTIVE_LOW 1
#define BOARD_BUTTON_ACTIVE_HIGH 1
#define BOARD_USB_NATIVE_TYPEC 1

/* Short aliases used by the board-status module. */
#define LED_PIN          BOARD_LED_PIN
#define BOOT_BUTTON_PIN  BOARD_BUTTON_PIN
#define LED_ACTIVE_LOW   BOARD_LED_ACTIVE_LOW
