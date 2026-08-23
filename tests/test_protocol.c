// SPDX-License-Identifier: GPL-3.0-only
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "pro2_protocol.h"
#include "pro2_rumble.h"
#include "pro2_settings.h"
#include "vendor_protocol.h"

static void pack12(uint8_t *p, uint16_t x, uint16_t y)
{
    p[0] = (uint8_t)x;
    p[1] = (uint8_t)(((x >> 8) & 0x0f) | ((y & 0x0f) << 4));
    p[2] = (uint8_t)(y >> 4);
}

static void test_settings(void)
{
    uint8_t command[8] = {'P', '2', 'C', 'A',
                          PRO2_SETTINGS_PROTOCOL_VERSION, 75, 128, 0};
    uint8_t report[PRO2_SETTINGS_REPORT_SIZE];

    pro2_settings_init();
    assert(pro2_settings_rumble_percent() == PRO2_SETTINGS_DEFAULT_RUMBLE);
    assert(pro2_settings_stick_deadzone() == PRO2_SETTINGS_DEFAULT_DEADZONE);
    assert(!pro2_settings_dirty());

    assert(pro2_settings_apply_wire(command, sizeof(command)));
    assert(pro2_settings_rumble_percent() == 75);
    assert(pro2_settings_stick_deadzone() == 128);
    assert(pro2_settings_dirty());
    assert(pro2_settings_build_report(report, sizeof(report),
                                      PRO2_WEB_COMMAND_APPLY,
                                      PRO2_WEB_STATUS_OK, 7) == sizeof(report));
    assert(!memcmp(report, "P2CF", 4));
    assert(report[4] == PRO2_SETTINGS_PROTOCOL_VERSION);
    assert(report[6] == 75 && report[7] == 128 && report[8] == 0);
    assert(report[10] == PRO2_WEB_COMMAND_APPLY);
    assert(report[11] == PRO2_WEB_STATUS_OK && report[12] == 1);
    assert(report[13] == 7);

    assert(pro2_settings_save());
    assert(!pro2_settings_dirty());
    command[5] = PRO2_SETTINGS_MAX_RUMBLE + 1u;
    assert(!pro2_settings_apply_wire(command, sizeof(command)));
    assert(pro2_settings_rumble_percent() == 75);

    pro2_settings_reset_defaults();
    assert(pro2_settings_rumble_percent() == PRO2_SETTINGS_DEFAULT_RUMBLE);
    assert(pro2_settings_stick_deadzone() == PRO2_SETTINGS_DEFAULT_DEADZONE);
}

static void test_fd2_to_usb(void)
{
    pro2_parser_t parser;
    pro2_state_t state;
    uint8_t fd2[60] = {0};
    uint8_t usb[64];
    pro2_parser_init(&parser);
    pro2_state_reset(&state);
    pack12(fd2 + 10, 2050, 2046);
    pack12(fd2 + 13, 2049, 2047);
    for (unsigned i = 0; i < 20; ++i) {
        assert(pro2_parse_fd2(&parser, &state, fd2, sizeof(fd2)) == 0);
    }
    assert(parser.calibrated);
    assert(state.lx == 2048 && state.ly == 2048);

    /* A + D-pad up + L + left paddle. */
    fd2[4] = 0x08;
    fd2[6] = 0x02;
    fd2[6] |= 0x40;
    fd2[7] = 0x02;
    fd2[48] = 0x34; fd2[49] = 0x12;
    fd2[54] = 0xcc; fd2[55] = 0xff;
    assert(pro2_parse_fd2(&parser, &state, fd2, sizeof(fd2)) == 0);
    pro2_make_usb_report(&state, 7, 0x12345678u, usb);
    assert(usb[0] == 0x09 && usb[63] == 7);
    assert((usb[1] & 0x02) != 0); /* A */
    assert((usb[2] & 0x10) != 0); /* D-pad up */
    assert((usb[1] & 0x10) != 0); /* L */
    assert((usb[3] & 0x08) != 0); /* GL */
}

static void test_usb_button_order(void)
{
    static const pro2_button_t expected_order[] = {
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
    pro2_state_t state;
    uint8_t usb[64];

    assert(sizeof(expected_order) / sizeof(expected_order[0]) == PRO2_BUTTON_COUNT);
    for (size_t usb_bit = 0; usb_bit < PRO2_BUTTON_COUNT; ++usb_bit) {
        pro2_state_reset(&state);
        state.buttons = 1u << expected_order[usb_bit];
        pro2_make_usb_report(&state, 0, 0, usb);
        uint32_t actual = (uint32_t)usb[1] | ((uint32_t)usb[2] << 8) |
                          ((uint32_t)usb[3] << 16);
        assert(actual == (1u << usb_bit));
    }
}

static void test_native_report(void)
{
    pro2_state_t state;
    uint8_t usb[64];
    pro2_state_reset(&state);
    state.buttons = (1u << PRO2_BUTTON_A) |
                    (1u << PRO2_BUTTON_DUP) |
                    (1u << PRO2_BUTTON_L);
    state.lx = 0x123;
    state.ly = 0x456;
    state.rx = 0x789;
    state.ry = 0xabc;
    state.motion_valid = true;
    state.accel[0] = 0x1234;
    state.gyro[0] = -52;

    pro2_make_usb_report_05(&state, 7, 0x12345678u, usb);
    assert(usb[0] == 0x05 && usb[1] == 7 && usb[2] == 0x20);
    assert((usb[5] & 0x08) != 0); /* A */
    assert((usb[7] & 0x02) != 0); /* D-pad up */
    assert((usb[7] & 0x40) != 0); /* L */
    assert(usb[11] == 0x23 && usb[12] == 0x61 && usb[13] == 0x45);
    assert(usb[14] == 0x89 && usb[15] == 0xc7 && usb[16] == 0xab);
    assert(usb[43] == 0x78 && usb[46] == 0x12);
    assert(usb[49] == 0x34 && usb[50] == 0x12);
    assert(usb[55] == 0xcc && usb[56] == 0xff);
}

static void test_neutral_report(void)
{
    uint8_t usb[64];
    pro2_make_usb_report(NULL, 0, 0, usb);
    assert(usb[0] == 0x09);
    assert(usb[1] == 0 && usb[2] == 0 && usb[3] == 0);
    assert(usb[4] == 0x00 && usb[5] == 0xf8 && usb[6] == 0x7f);
    assert(usb[7] == 0x00 && usb[8] == 0xf8 && usb[9] == 0x7f);
    assert(usb[63] == 0);
}

static void test_generic_axes(void)
{
    pro2_state_t state;
    uint8_t usb[64];
    pro2_state_reset(&state);
    state.lx = 0x123;
    state.ly = 0x456;
    state.rx = 0x789;
    state.ry = 0xabc;
    pro2_make_usb_report_09(&state, 9, 0, usb);
    assert(usb[4] == 0x23 && usb[5] == 0x91 && usb[6] == 0xba);
    assert(usb[7] == 0x89 && usb[8] == 0x37 && usb[9] == 0x54);
    assert(usb[63] == 9);
}

static void test_rumble(void)
{
    uint8_t report[64] = {0};
    uint8_t left[5], right[5], zero[5], packet[33];
    static const uint8_t neutral[5] = {0x87,0x01,0x20,0x11,0x00};
    report[0] = 0x02;
    report[1] = 0x50;
    memcpy(report + 2, neutral, 5);
    memcpy(report + 0x12, neutral, 5);
    assert(pro2_rumble_decode_usb(report, sizeof(report), left, right));
    pro2_rumble_make_zero(zero);
    assert(!memcmp(left, zero, 5) && !memcmp(right, zero, 5));
    pro2_rumble_build_packet(3, left, right, packet);
    assert(packet[0] == 0 && packet[1] == 0x53 && packet[17] == 0x53);
    assert(!memcmp(packet + 2, zero, 5));

    report[2] = 0x21;
    report[3] = 0x7f;
    report[4] = 0x08;
    report[5] = 0xff;
    report[6] = 0xff;
    assert(pro2_rumble_decode_usb(report, sizeof(report), left, right));
    assert(memcmp(left, zero, 5) != 0);

    uint8_t settings_command[8] = {'P', '2', 'C', 'A',
                                   PRO2_SETTINGS_PROTOCOL_VERSION, 0, 64, 0};
    assert(pro2_settings_apply_wire(settings_command, sizeof(settings_command)));
    assert(pro2_rumble_decode_usb(report, sizeof(report), left, right));
    uint64_t packed = 0;
    for (size_t i = 0; i < 5; ++i) packed |= (uint64_t)left[i] << (i * 8);
    assert(((packed >> 10) & 0x3ffu) == 0);
    assert(((packed >> 30) & 0x3ffu) == 0);
    pro2_settings_reset_defaults();
}

static void test_vendor_protocol(void)
{
    uint8_t cmd[64] = {0};
    uint8_t reply[PRO2_VENDOR_REPLY_MAX];
    memcpy(cmd, PRO2_DIAGNOSTIC_QUERY_MAGIC,
           PRO2_DIAGNOSTIC_QUERY_MAGIC_SIZE);
    assert(vendor_protocol_is_diagnostic_query(
        cmd, PRO2_DIAGNOSTIC_QUERY_MAGIC_SIZE));
    assert(!vendor_protocol_is_diagnostic_query(cmd, 3));

    memset(cmd, 0, sizeof(cmd));
    cmd[0] = 0x03; cmd[2] = 1; cmd[3] = 0x0d;
    memset(reply, 0xa5, sizeof(reply));
    assert(vendor_protocol_build_reply(cmd, 16, reply, 11) == 0);
    for (size_t i = 0; i < sizeof(reply); ++i) assert(reply[i] == 0xa5);
    assert(vendor_protocol_build_reply(cmd, 16, reply, sizeof(reply)) == 12);
    assert(reply[0] == 0x03 && reply[1] == 1 && reply[5] == 0xf8 && reply[8] == 1);

    memset(cmd, 0, sizeof(cmd));
    cmd[0] = 0x02; cmd[12] = 0x40; cmd[13] = 0x30; cmd[14] = 0x01;
    assert(vendor_protocol_build_reply(cmd, 16, reply, sizeof(reply)) == 32);
    assert(reply[8] == 0x10);
    assert(reply[16] == 0x16 && reply[31] == 0x3b);

    memset(cmd, 0, sizeof(cmd));
    cmd[0] = 0x02; cmd[12] = 0x00; cmd[13] = 0x30; cmd[14] = 0x01;
    assert(vendor_protocol_build_reply(cmd, 16, reply, sizeof(reply)) == 80);
    assert(reply[16] == 0x01 && reply[17] == 0x00);
    static const uint8_t serial[16] = {
        'H','E','J','7','1','0','0','1','1','2','1','2','4','7',0,0,
    };
    assert(!memcmp(reply + 18, serial, sizeof(serial)));
    assert(reply[34] == 0x7e && reply[35] == 0x05);
    assert(reply[36] == 0x69 && reply[37] == 0x20);
    assert(reply[38] == 0x01 && reply[39] == 0x06 && reply[40] == 0x01);
    assert(reply[41] == 0x23 && reply[44] == 0xa0);
    assert(reply[47] == 0xe6 && reply[50] == 0x32);
    for (size_t i = 53; i < 80; ++i) assert(reply[i] == 0xff);

    cmd[0] = 0x10;
    assert(vendor_protocol_build_reply(cmd, 16, reply, sizeof(reply)) == 0);
}

int main(void)
{
    test_settings();
    test_fd2_to_usb();
    test_usb_button_order();
    test_native_report();
    test_neutral_report();
    test_generic_axes();
    test_rumble();
    test_vendor_protocol();
    puts("protocol tests: PASS");
    return 0;
}
