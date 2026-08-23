// SPDX-License-Identifier: GPL-3.0-only
#include "pro2_protocol.h"
#include "pro2_settings.h"

#include <string.h>

#define AXIS_PHYSICAL_RANGE    1600
#define AXIS_CALIBRATION_COUNT 20
#define AXIS_CENTER_MAX_DELTA  256

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int16_t read_i16_le(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void write_i16_le(uint8_t *p, int16_t value)
{
    p[0] = (uint8_t)((uint16_t)value & 0xffu);
    p[1] = (uint8_t)(((uint16_t)value >> 8) & 0xffu);
}

static uint16_t unpack12_x(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)((p[1] & 0x0fu) << 8);
}

static uint16_t unpack12_y(const uint8_t *p)
{
    return (uint16_t)((p[1] >> 4) & 0x0fu) | (uint16_t)(p[2] << 4);
}

static void pack12(uint8_t *p, uint16_t x, uint16_t y)
{
    p[0] = (uint8_t)(x & 0xffu);
    p[1] = (uint8_t)(((x >> 8) & 0x0fu) | ((y & 0x0fu) << 4));
    p[2] = (uint8_t)((y >> 4) & 0xffu);
}

static bool near_center(uint16_t value)
{
    int delta = (int)value - PRO2_AXIS_CENTER;
    if (delta < 0) {
        delta = -delta;
    }
    return delta <= AXIS_CENTER_MAX_DELTA;
}

static uint16_t normalize_axis(uint16_t value, uint16_t center)
{
    int deadzone = (int)pro2_settings_stick_deadzone();
    int delta = (int)value - (int)center;
    bool negative = delta < 0;
    int magnitude = negative ? -delta : delta;
    if (magnitude <= deadzone) {
        return PRO2_AXIS_CENTER;
    }

    int usable = AXIS_PHYSICAL_RANGE - deadzone;
    int target = negative ? PRO2_AXIS_CENTER : (4095 - PRO2_AXIS_CENTER);
    int scaled = ((magnitude - deadzone) * target + usable / 2) / usable;
    if (scaled > target) {
        scaled = target;
    }
    return (uint16_t)(negative ? PRO2_AXIS_CENTER - scaled : PRO2_AXIS_CENTER + scaled);
}

static void set_button(pro2_state_t *state, pro2_button_t button, bool pressed)
{
    uint32_t mask = 1u << (uint8_t)button;
    if (pressed) {
        state->buttons |= mask;
    } else {
        state->buttons &= ~mask;
    }
}

void pro2_parser_init(pro2_parser_t *parser)
{
    memset(parser, 0, sizeof(*parser));
    for (size_t i = 0; i < 4; ++i) {
        parser->center[i] = PRO2_AXIS_CENTER;
    }
}

void pro2_state_reset(pro2_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->lx = PRO2_AXIS_CENTER;
    state->ly = PRO2_AXIS_CENTER;
    state->rx = PRO2_AXIS_CENTER;
    state->ry = PRO2_AXIS_CENTER;
}

bool pro2_state_button(const pro2_state_t *state, pro2_button_t button)
{
    return state && button < PRO2_BUTTON_COUNT &&
           (state->buttons & (1u << (uint8_t)button)) != 0;
}

static void decode_buttons(pro2_state_t *state, uint32_t buttons)
{
    state->buttons = 0;
    set_button(state, PRO2_BUTTON_Y,       (buttons & 0x00000001u) != 0);
    set_button(state, PRO2_BUTTON_X,       (buttons & 0x00000002u) != 0);
    set_button(state, PRO2_BUTTON_B,       (buttons & 0x00000004u) != 0);
    set_button(state, PRO2_BUTTON_A,       (buttons & 0x00000008u) != 0);
    set_button(state, PRO2_BUTTON_R,       (buttons & 0x00000040u) != 0);
    set_button(state, PRO2_BUTTON_ZR,      (buttons & 0x00000080u) != 0);
    set_button(state, PRO2_BUTTON_MINUS,   (buttons & 0x00000100u) != 0);
    set_button(state, PRO2_BUTTON_PLUS,    (buttons & 0x00000200u) != 0);
    set_button(state, PRO2_BUTTON_RSTICK,  (buttons & 0x00000400u) != 0);
    set_button(state, PRO2_BUTTON_LSTICK,  (buttons & 0x00000800u) != 0);
    set_button(state, PRO2_BUTTON_HOME,    (buttons & 0x00001000u) != 0);
    set_button(state, PRO2_BUTTON_CAPTURE, (buttons & 0x00002000u) != 0);
    set_button(state, PRO2_BUTTON_C,       (buttons & 0x00004000u) != 0);
    set_button(state, PRO2_BUTTON_DDOWN,   (buttons & 0x00010000u) != 0);
    set_button(state, PRO2_BUTTON_DUP,     (buttons & 0x00020000u) != 0);
    set_button(state, PRO2_BUTTON_DRIGHT,  (buttons & 0x00040000u) != 0);
    set_button(state, PRO2_BUTTON_DLEFT,   (buttons & 0x00080000u) != 0);
    set_button(state, PRO2_BUTTON_L,       (buttons & 0x00400000u) != 0);
    set_button(state, PRO2_BUTTON_ZL,      (buttons & 0x00800000u) != 0);
    set_button(state, PRO2_BUTTON_GR,      (buttons & 0x01000000u) != 0);
    set_button(state, PRO2_BUTTON_GL,      (buttons & 0x02000000u) != 0);
}

static void update_calibration(pro2_parser_t *parser, const pro2_state_t *state,
                               const uint16_t axes[4])
{
    if (parser->calibrated) {
        return;
    }
    if (state->buttons != 0 || !near_center(axes[0]) || !near_center(axes[1]) ||
        !near_center(axes[2]) || !near_center(axes[3])) {
        parser->samples = 0;
        memset(parser->sum, 0, sizeof(parser->sum));
        return;
    }
    for (size_t i = 0; i < 4; ++i) {
        parser->sum[i] += axes[i];
    }
    parser->samples++;
    if (parser->samples >= AXIS_CALIBRATION_COUNT) {
        for (size_t i = 0; i < 4; ++i) {
            parser->center[i] = (uint16_t)(parser->sum[i] / parser->samples);
        }
        parser->calibrated = true;
    }
}

int pro2_parse_fd2(pro2_parser_t *parser, pro2_state_t *state,
                   const uint8_t *data, size_t len)
{
    if (!parser || !state || !data || len < 16) {
        return -1;
    }

    bool had_motion = state->motion_valid;
    int16_t accel[3];
    int16_t gyro[3];
    memcpy(accel, state->accel, sizeof(accel));
    memcpy(gyro, state->gyro, sizeof(gyro));
    pro2_state_reset(state);
    if (had_motion) {
        state->motion_valid = true;
        memcpy(state->accel, accel, sizeof(accel));
        memcpy(state->gyro, gyro, sizeof(gyro));
    }

    decode_buttons(state, read_le32(data + 4));
    uint16_t axes[4] = {
        unpack12_x(data + 10), unpack12_y(data + 10),
        unpack12_x(data + 13), unpack12_y(data + 13),
    };
    update_calibration(parser, state, axes);
    if (parser->calibrated) {
        state->lx = normalize_axis(axes[0], parser->center[0]);
        state->ly = normalize_axis(axes[1], parser->center[1]);
        state->rx = normalize_axis(axes[2], parser->center[2]);
        state->ry = normalize_axis(axes[3], parser->center[3]);
    }

    if (len >= PRO2_FD2_MIN_REPORT_SIZE) {
        for (size_t i = 0; i < 3; ++i) {
            state->accel[i] = read_i16_le(data + 48 + i * 2);
            state->gyro[i] = read_i16_le(data + 54 + i * 2);
        }
        state->motion_valid = true;
    }
    return 0;
}

void pro2_make_usb_report_05(const pro2_state_t *state, uint8_t sequence,
                             uint32_t timestamp_us,
                             uint8_t report[PRO2_USB_REPORT_SIZE])
{
    memset(report, 0, PRO2_USB_REPORT_SIZE);
    report[0] = PRO2_USB_NATIVE_INPUT_REPORT_ID;
    report[1] = sequence;
    report[2] = 0x20;
    report[12] = 0x08;
    report[13] = 0x80;
    report[15] = 0x08;
    report[16] = 0x80;
    if (!state) {
        return;
    }

#define BTN(button) pro2_state_button(state, (button))
    if (BTN(PRO2_BUTTON_Y))  report[5] |= 0x01;
    if (BTN(PRO2_BUTTON_X))  report[5] |= 0x02;
    if (BTN(PRO2_BUTTON_B))  report[5] |= 0x04;
    if (BTN(PRO2_BUTTON_A))  report[5] |= 0x08;
    if (BTN(PRO2_BUTTON_R))  report[5] |= 0x40;
    if (BTN(PRO2_BUTTON_ZR)) report[5] |= 0x80;

    if (BTN(PRO2_BUTTON_MINUS))   report[6] |= 0x01;
    if (BTN(PRO2_BUTTON_PLUS))    report[6] |= 0x02;
    if (BTN(PRO2_BUTTON_RSTICK))  report[6] |= 0x04;
    if (BTN(PRO2_BUTTON_LSTICK))  report[6] |= 0x08;
    if (BTN(PRO2_BUTTON_HOME))    report[6] |= 0x10;
    if (BTN(PRO2_BUTTON_CAPTURE)) report[6] |= 0x20;
    if (BTN(PRO2_BUTTON_C))       report[6] |= 0x40;

    if (BTN(PRO2_BUTTON_DDOWN))  report[7] |= 0x01;
    if (BTN(PRO2_BUTTON_DUP))    report[7] |= 0x02;
    if (BTN(PRO2_BUTTON_DRIGHT)) report[7] |= 0x04;
    if (BTN(PRO2_BUTTON_DLEFT))  report[7] |= 0x08;
    if (BTN(PRO2_BUTTON_L))      report[7] |= 0x40;
    if (BTN(PRO2_BUTTON_ZL))     report[7] |= 0x80;
    if (BTN(PRO2_BUTTON_GR))     report[8] |= 0x01;
    if (BTN(PRO2_BUTTON_GL))     report[8] |= 0x02;
#undef BTN

    pack12(report + 11, state->lx, state->ly);
    pack12(report + 14, state->rx, state->ry);
    report[43] = (uint8_t)(timestamp_us & 0xffu);
    report[44] = (uint8_t)((timestamp_us >> 8) & 0xffu);
    report[45] = (uint8_t)((timestamp_us >> 16) & 0xffu);
    report[46] = (uint8_t)((timestamp_us >> 24) & 0xffu);

    if (state->motion_valid) {
        for (size_t i = 0; i < 3; ++i) {
            write_i16_le(report + 49 + i * 2, state->accel[i]);
            write_i16_le(report + 55 + i * 2, state->gyro[i]);
        }
    }
}

void pro2_make_usb_report_09(const pro2_state_t *state, uint8_t sequence,
                             uint32_t timestamp_us,
                             uint8_t report[PRO2_USB_REPORT_SIZE])
{
    (void)timestamp_us;
    memset(report, 0, PRO2_USB_REPORT_SIZE);
    report[0] = PRO2_USB_INPUT_REPORT_ID;
    /* The trailing byte is declared constant in the HID descriptor.  Keep the
     * sequence there so it cannot become a phantom button or axis in browsers. */
    report[63] = sequence;
    pack12(report + 4, PRO2_AXIS_CENTER, PRO2_AXIS_CENTER - 1u);
    pack12(report + 7, PRO2_AXIS_CENTER, PRO2_AXIS_CENTER - 1u);
    if (!state) {
        return;
    }

    /*
     * FD2 and the native controller report use a Nintendo-specific bit order.
     * Windows exposes report 0x09 as a generic gamepad until its Switch 2
     * driver has completed the vendor initialization, so emitting that raw
     * order makes R appear as L, the D-pad appear as -/+/L3/R3, and so on.
     * Keep the internal state native, but put the host-facing buttons in the
     * canonical positional order used by the standard Gamepad mapping.
     */
    static const pro2_button_t usb_button_order[] = {
        PRO2_BUTTON_B,       PRO2_BUTTON_A,
        PRO2_BUTTON_Y,       PRO2_BUTTON_X,
        PRO2_BUTTON_L,       PRO2_BUTTON_R,
        PRO2_BUTTON_ZL,      PRO2_BUTTON_ZR,
        PRO2_BUTTON_MINUS,   PRO2_BUTTON_PLUS,
        PRO2_BUTTON_LSTICK,  PRO2_BUTTON_RSTICK,
        PRO2_BUTTON_DUP,     PRO2_BUTTON_DDOWN,
        PRO2_BUTTON_DLEFT,   PRO2_BUTTON_DRIGHT,
        PRO2_BUTTON_HOME,    PRO2_BUTTON_CAPTURE,
        PRO2_BUTTON_GR,      PRO2_BUTTON_GL,
        PRO2_BUTTON_C,
    };
    uint32_t usb_buttons = 0;
    for (size_t i = 0; i < sizeof(usb_button_order) / sizeof(usb_button_order[0]); ++i) {
        if (pro2_state_button(state, usb_button_order[i])) {
            usb_buttons |= 1u << i;
        }
    }
    report[1] = (uint8_t)usb_buttons;
    report[2] = (uint8_t)(usb_buttons >> 8);
    report[3] = (uint8_t)(usb_buttons >> 16) & 0x1fu;
    pack12(report + 4, state->lx, 0x0fffu - state->ly);
    pack12(report + 7, state->rx, 0x0fffu - state->ry);
}

void pro2_make_usb_report(const pro2_state_t *state, uint8_t sequence,
                          uint32_t timestamp_us,
                          uint8_t report[PRO2_USB_REPORT_SIZE])
{
    pro2_make_usb_report_09(state, sequence, timestamp_us, report);
}
