// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pro2_protocol.h"

#define PRO2_USB_DIAGNOSTIC_REPORT_SIZE 64u

void usb_nintendo_init(void);
bool usb_nintendo_ready(void);
uint8_t usb_nintendo_led_stage(void);
void usb_nintendo_submit_state(const pro2_state_t *state,
                               uint32_t timestamp_us);
size_t usb_nintendo_build_diagnostic_report(uint8_t *report, size_t report_max);
