// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PRO2_USB_REPORT_SIZE              64u
#define PRO2_USB_NATIVE_INPUT_REPORT_ID   0x05u
#define PRO2_USB_GENERIC_INPUT_REPORT_ID  0x09u
#define PRO2_USB_INPUT_REPORT_ID          PRO2_USB_GENERIC_INPUT_REPORT_ID
#define PRO2_USB_OUTPUT_REPORT_ID  0x02u
#define PRO2_FD2_MIN_REPORT_SIZE   60u
#define PRO2_MOTION_SAMPLE_SIZE    12u
#define PRO2_AXIS_CENTER           2048u

typedef enum {
    PRO2_BUTTON_B = 0,
    PRO2_BUTTON_A,
    PRO2_BUTTON_Y,
    PRO2_BUTTON_X,
    PRO2_BUTTON_R,
    PRO2_BUTTON_ZR,
    PRO2_BUTTON_PLUS,
    PRO2_BUTTON_RSTICK,
    PRO2_BUTTON_DDOWN,
    PRO2_BUTTON_DRIGHT,
    PRO2_BUTTON_DLEFT,
    PRO2_BUTTON_DUP,
    PRO2_BUTTON_L,
    PRO2_BUTTON_ZL,
    PRO2_BUTTON_MINUS,
    PRO2_BUTTON_LSTICK,
    PRO2_BUTTON_HOME,
    PRO2_BUTTON_CAPTURE,
    PRO2_BUTTON_GR,
    PRO2_BUTTON_GL,
    PRO2_BUTTON_C,
    PRO2_BUTTON_COUNT
} pro2_button_t;

typedef struct {
    uint32_t buttons;
    uint16_t lx;
    uint16_t ly;
    uint16_t rx;
    uint16_t ry;
    bool motion_valid;
    int16_t accel[3];
    int16_t gyro[3];
} pro2_state_t;

typedef struct {
    bool calibrated;
    uint8_t samples;
    uint32_t sum[4];
    uint16_t center[4];
} pro2_parser_t;

void pro2_parser_init(pro2_parser_t *parser);
void pro2_state_reset(pro2_state_t *state);
bool pro2_state_button(const pro2_state_t *state, pro2_button_t button);
int pro2_parse_fd2(pro2_parser_t *parser, pro2_state_t *state,
                   const uint8_t *data, size_t len);
void pro2_make_usb_report(const pro2_state_t *state, uint8_t sequence,
                          uint32_t timestamp_us,
                          uint8_t report[PRO2_USB_REPORT_SIZE]);
void pro2_make_usb_report_05(const pro2_state_t *state, uint8_t sequence,
                             uint32_t timestamp_us,
                             uint8_t report[PRO2_USB_REPORT_SIZE]);
void pro2_make_usb_report_09(const pro2_state_t *state, uint8_t sequence,
                             uint32_t timestamp_us,
                             uint8_t report[PRO2_USB_REPORT_SIZE]);
