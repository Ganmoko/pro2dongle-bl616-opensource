// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PRO2_SETTINGS_PROTOCOL_VERSION 1u
#define PRO2_SETTINGS_REPORT_SIZE      63u
#define PRO2_SETTINGS_DEFAULT_DEADZONE 64u
#define PRO2_SETTINGS_MAX_DEADZONE     512u
#define PRO2_SETTINGS_DEFAULT_RUMBLE   100u
#define PRO2_SETTINGS_MAX_RUMBLE       150u

typedef enum {
    PRO2_WEB_COMMAND_NONE = 0,
    PRO2_WEB_COMMAND_APPLY,
    PRO2_WEB_COMMAND_SAVE,
    PRO2_WEB_COMMAND_RESET,
    PRO2_WEB_COMMAND_FORGET_PEER,
} pro2_web_command_t;

typedef enum {
    PRO2_WEB_STATUS_IDLE = 0,
    PRO2_WEB_STATUS_PENDING,
    PRO2_WEB_STATUS_OK,
    PRO2_WEB_STATUS_INVALID,
    PRO2_WEB_STATUS_BUSY,
    PRO2_WEB_STATUS_FLASH_ERROR,
} pro2_web_status_t;

void pro2_settings_init(void);
void pro2_settings_reset_defaults(void);
bool pro2_settings_apply_wire(const uint8_t *data, size_t len);
bool pro2_settings_save(void);
bool pro2_settings_dirty(void);
uint16_t pro2_settings_stick_deadzone(void);
uint8_t pro2_settings_rumble_percent(void);
size_t pro2_settings_build_report(uint8_t *report, size_t report_max,
                                  pro2_web_command_t last_command,
                                  pro2_web_status_t last_status,
                                  uint8_t action_sequence);
