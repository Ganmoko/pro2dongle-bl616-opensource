// SPDX-License-Identifier: GPL-3.0-only
#include "vendor_protocol.h"

#include <string.h>

static const uint8_t controller_serial[16] = {
    'H','E','J','7','1','0','0','1','1','2','1','2','4','7',0,0,
};

static void write_bytes(uint8_t *out, size_t out_len, size_t offset,
                        const uint8_t *data, size_t data_len);

/*
 * Factory-data bytes captured from a Switch 2 Pro Controller.  The native
 * controller serial remains in the emulated flash response; the bridge uses a
 * separate USB serial so Windows does not merge it with a real wired Pro2.
 * Unused factory flash is erased (0xff), not zero-filled.
 */
static void build_device_info(uint8_t *data, size_t data_len)
{
    static const uint8_t identity[] = {
        0x01,0x00,                         /* 0x13000: controller type */
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, /* 0x13002: serial */
        0x7e,0x05,                         /* 0x13012: VID 057e */
        0x69,0x20,                         /* 0x13014: PID 2069 */
        0x01,0x06,0x01,                    /* 0x13016: model data */
        0x23,0x23,0x23,                    /* body colour */
        0xa0,0xa0,0xa0,                    /* buttons colour */
        0xe6,0xe6,0xe6,                    /* highlight colour */
        0x32,0x32,0x32,                    /* grip colour */
    };

    memset(data, 0xff, data_len);
    write_bytes(data, data_len, 0, identity, sizeof(identity));
    write_bytes(data, data_len, 2, controller_serial,
                sizeof(controller_serial));
}

bool vendor_protocol_is_diagnostic_query(const uint8_t *command,
                                         size_t command_len)
{
    return command && command_len >= PRO2_DIAGNOSTIC_QUERY_MAGIC_SIZE &&
           !memcmp(command, PRO2_DIAGNOSTIC_QUERY_MAGIC,
                   PRO2_DIAGNOSTIC_QUERY_MAGIC_SIZE);
}

static void write_bytes(uint8_t *out, size_t out_len, size_t offset,
                        const uint8_t *data, size_t data_len)
{
    if (offset >= out_len) {
        return;
    }
    if (data_len > out_len - offset) {
        data_len = out_len - offset;
    }
    memcpy(out + offset, data, data_len);
}

static void pack12_pair(uint8_t *out, uint16_t x, uint16_t y)
{
    out[0] = (uint8_t)x;
    out[1] = (uint8_t)(((x >> 8) & 0x0fu) | ((y & 0x0fu) << 4));
    out[2] = (uint8_t)(y >> 4);
}

static size_t build_ack(const uint8_t *cmd, size_t cmd_len,
                        uint8_t *reply, size_t len)
{
    memset(reply, 0, len);
    reply[0] = cmd[0];
    if (len > 1) reply[1] = 0x01;
    if (len > 2 && cmd_len > 2) reply[2] = cmd[2];
    if (len > 3 && cmd_len > 3) reply[3] = cmd[3];
    if (len > 4 && cmd_len > 4) reply[4] = cmd[4];
    if (len > 5) reply[5] = 0xf8;
    return len;
}

static size_t flash_length(uint32_t address)
{
    if (address == 0x13040u) return 0x10;
    if (address == 0x13100u) return 0x18;
    if (address == 0x13060u) return 0x20;
    return 0x40;
}

static size_t build_flash(const uint8_t *cmd, uint8_t *reply, size_t reply_max)
{
    uint32_t address = (uint32_t)cmd[12] | ((uint32_t)cmd[13] << 8) |
                       ((uint32_t)cmd[14] << 16) | ((uint32_t)cmd[15] << 24);
    size_t data_len = flash_length(address);
    size_t len = 0x10u + data_len;
    if (len > reply_max) return 0;
    memset(reply, 0, len);
    uint8_t *data = reply + 0x10;

    if (address == 0x13000u) {
        build_device_info(data, data_len);
    } else if (address == 0x13080u || address == 0x130c0u) {
        uint8_t calibration[9];
        memset(data, 0xff, data_len);
        pack12_pair(calibration + 0, 2048, 2048);
        pack12_pair(calibration + 3, 2048, 2048);
        pack12_pair(calibration + 6, 2048, 2048);
        write_bytes(data, data_len, 0x28, calibration, sizeof(calibration));
    } else if (address == 0x1fc040u || address == 0x1fc080u ||
               address == 0x13060u) {
        memset(data, 0xff, data_len);
    } else if (address == 0x13040u) {
        static const uint8_t block[] = {
            0x16,0xf4,0xd3,0x41,0x48,0xce,0x85,0xba,
            0xf1,0x05,0x71,0xba,0x1f,0x27,0xcb,0x3b,
        };
        memcpy(data, block, sizeof(block));
    } else if (address == 0x13100u) {
        static const uint8_t block[] = {
            0,0,0,0,0,0,0,0,0,0,0,0,0x2d,0x10,0xa7,0x3d,
            0xe7,0x49,0x35,0x3c,0xa4,0x2d,0x20,0x41,
        };
        memcpy(data, block, sizeof(block));
    }
    reply[0] = 0x02;
    reply[1] = 0x01;
    reply[2] = cmd[2];
    reply[3] = cmd[3];
    reply[5] = 0xf8;
    reply[8] = (uint8_t)data_len;
    memcpy(reply + 12, cmd + 12, 4);
    return len > PRO2_VENDOR_REPLY_MAX ? PRO2_VENDOR_REPLY_MAX : len;
}

size_t vendor_protocol_build_reply(const uint8_t *cmd, size_t cmd_len,
                                   uint8_t *reply, size_t reply_max)
{
    if (!cmd || !cmd_len || !reply || reply_max < 8) return 0;
    uint8_t c0 = cmd[0];
    uint8_t arg = cmd_len > 3 ? cmd[3] : 0;
    if (cmd_len >= 16 && c0 == 0x02) return build_flash(cmd, reply, reply_max);
    if ((c0 == 0x0c && arg == 0x02) || c0 == 0x10) return 0;

#define ACK(n) do { if ((n) > reply_max) return 0; return build_ack(cmd, cmd_len, reply, (n)); } while (0)
    if (c0 == 0x03 && arg == 0x0d) {
        if (reply_max < 12) return 0;
        build_ack(cmd, cmd_len, reply, 12); reply[8] = 1; return 12;
    }
    if (c0 == 0x15 && arg == 0x01) {
        static const uint8_t mac[] = {0x2d,0xfc,0x27,0xce,0xc6,0x38};
        if (reply_max < 17) return 0;
        build_ack(cmd, cmd_len, reply, 17);
        reply[8] = 1; reply[9] = 4; reply[10] = 1;
        memcpy(reply + 11, mac, sizeof(mac)); return 17;
    }
    if (c0 == 0x15 && arg == 0x02) {
        if (reply_max < 25) return 0;
        build_ack(cmd, cmd_len, reply, 25); reply[8] = 1; return 25;
    }
    if (c0 == 0x15 && arg == 0x03) {
        if (reply_max < 9) return 0;
        build_ack(cmd, cmd_len, reply, 9); reply[8] = 1; return 9;
    }
    if (c0 == 0x11) {
        static const uint8_t payload[] = {
            0x20,0x03,0,0,0x0a,0xe8,0x1c,0x3b,0x79,0x7d,0x8b,0x3a,
            0x0a,0xe8,0x9c,0x42,0x58,0xa0,0x0b,0x42,
            0x0a,0xe8,0x9c,0x41,0x58,0xa0,0x0b,0x41,
        };
        if (reply_max < 37) return 0;
        build_ack(cmd, cmd_len, reply, 37); reply[8] = 1;
        memcpy(reply + 9, payload, sizeof(payload)); return 37;
    }
    if (c0 == 0x01 && arg == 0x0c) {
        static const uint8_t payload[] = {0x61,0x12,0x50,0x10};
        if (reply_max < 12) return 0;
        build_ack(cmd, cmd_len, reply, 12);
        memcpy(reply + 8, payload, sizeof(payload)); return 12;
    }
    if (c0 == 0x03 && arg == 0x01) {
        if (reply_max < 16) return 0;
        build_ack(cmd, cmd_len, reply, 16);
        reply[10] = 0x40; reply[11] = 0xf0; reply[14] = 0x60; return 16;
    }
    ACK(8);
#undef ACK
}
