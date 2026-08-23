// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdbool.h>

typedef enum {
    BOARD_STATUS_BOOT,
    BOARD_STATUS_SCANNING,
    BOARD_STATUS_CONNECTING,
    BOARD_STATUS_READY,
    BOARD_STATUS_ERROR,
} board_status_t;

void board_status_init(void);
void board_status_set(board_status_t status);
bool board_button_pressed(void);
void board_status_task(void *arg);
