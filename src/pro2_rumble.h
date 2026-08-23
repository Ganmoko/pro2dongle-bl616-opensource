// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PRO2_RUMBLE_BLE_SIZE 5u
#define PRO2_RUMBLE_PACKET_SIZE 33u
#define PRO2_RUMBLE_DIAGNOSTIC_REPORT_SIZE 64u

bool pro2_rumble_decode_usb(const uint8_t *report, size_t len,
                            uint8_t left[5], uint8_t right[5]);
void pro2_rumble_make_zero(uint8_t out[5]);
void pro2_rumble_build_packet(uint8_t packet_id, const uint8_t left[5],
                              const uint8_t right[5], uint8_t out[33]);

#ifndef PRO2_HOST_TEST
void pro2_rumble_init(void);
void pro2_rumble_submit_usb(const uint8_t *report, size_t len);
size_t pro2_rumble_build_diagnostic_report(uint8_t *report,
                                           size_t report_max);
#endif
