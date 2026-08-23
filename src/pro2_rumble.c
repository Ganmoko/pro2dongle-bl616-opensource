// SPDX-License-Identifier: GPL-3.0-only
#include "pro2_rumble.h"
#include "pro2_settings.h"

#include <string.h>

static uint16_t map_amplitude(uint32_t value)
{
    uint64_t scaled = (uint64_t)value * 1023u *
                      pro2_settings_rumble_percent();
    uint64_t mapped = (scaled + 1450000u) / 2900000u;
    return (uint16_t)(mapped > 1023u ? 1023u : mapped);
}

static void pack_vibration(uint16_t lf_freq, uint16_t lf_amp,
                           uint16_t hf_freq, uint16_t hf_amp,
                           uint8_t out[5])
{
    uint64_t v = (uint64_t)(lf_freq & 0x1ffu) |
                 ((uint64_t)(lf_amp & 0x3ffu) << 10) |
                 ((uint64_t)(hf_freq & 0x1ffu) << 20) |
                 ((uint64_t)(hf_amp & 0x3ffu) << 30);
    for (size_t i = 0; i < 5; ++i) out[i] = (uint8_t)(v >> (i * 8));
}

void pro2_rumble_make_zero(uint8_t out[5])
{
    pack_vibration(0x0e1, 0, 0x1e1, 0, out);
}

static void decode_frame(const uint8_t *p, uint8_t out[5])
{
    uint16_t hf = (uint16_t)p[0] | (uint16_t)((p[1] & 3u) << 8);
    uint32_t ha = ((uint32_t)(p[1] & 0xfcu) << 4) |
                  ((uint32_t)(p[2] & 0x0fu) << 12);
    uint16_t lf = (uint16_t)((p[2] & 0xf0u) >> 4) |
                  (uint16_t)((p[3] & 0x3fu) << 4);
    uint32_t la = (uint32_t)(p[3] & 0xc0u) | ((uint32_t)p[4] << 8);
    pack_vibration(lf, map_amplitude(la), hf, map_amplitude(ha), out);
}

bool pro2_rumble_decode_usb(const uint8_t *report, size_t len,
                            uint8_t left[5], uint8_t right[5])
{
    if (!report || !left || !right || len < 0x17u || report[0] != 0x02u ||
        (report[1] & 0xf0u) != 0x50u) {
        return false;
    }
    static const uint8_t neutral[5] = {0x87,0x01,0x20,0x11,0x00};
    if (!memcmp(report + 2, neutral, 5) && !memcmp(report + 0x12, neutral, 5)) {
        pro2_rumble_make_zero(left);
        pro2_rumble_make_zero(right);
    } else {
        decode_frame(report + 2, left);
        decode_frame(report + 0x12, right);
    }
    return true;
}

static void motor_block(uint8_t *out, uint8_t id, const uint8_t first[5],
                        const uint8_t zero[5])
{
    out[0] = (uint8_t)(0x50u | (id & 0x0fu));
    memcpy(out + 1, first, 5);
    memcpy(out + 6, zero, 5);
    memcpy(out + 11, zero, 5);
}

void pro2_rumble_build_packet(uint8_t packet_id, const uint8_t left[5],
                              const uint8_t right[5], uint8_t out[33])
{
    uint8_t zero[5];
    pro2_rumble_make_zero(zero);
    memset(out, 0, 33);
    motor_block(out + 1, packet_id, left, zero);
    motor_block(out + 17, packet_id, right, zero);
}

#ifndef PRO2_HOST_TEST
#include "FreeRTOS.h"
#include "task.h"
#include "bflb_mtimer.h"
#include "pro2_ble.h"

static uint8_t active_left[5];
static uint8_t active_right[5];
static volatile uint64_t active_until;
static volatile uint8_t stop_packets;
static uint8_t packet_id;
static volatile uint32_t usb_reports;
static volatile uint32_t decoded_reports;
static volatile uint32_t rejected_reports;
static volatile uint32_t write_attempts;
static volatile uint32_t write_successes;
static volatile uint32_t write_failures;
static volatile int32_t last_write_result;
static uint8_t last_usb_len;
static uint8_t last_usb_prefix[16];
static uint8_t last_ble_prefix[12];

static void put_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

void pro2_rumble_submit_usb(const uint8_t *report, size_t len)
{
    uint8_t left[5], right[5], zero[5];
    taskENTER_CRITICAL();
    usb_reports++;
    last_usb_len = len > UINT8_MAX ? UINT8_MAX : (uint8_t)len;
    memset(last_usb_prefix, 0, sizeof(last_usb_prefix));
    if (report) {
        size_t copy_len = len;
        if (copy_len > sizeof(last_usb_prefix)) copy_len = sizeof(last_usb_prefix);
        memcpy(last_usb_prefix, report, copy_len);
    }
    taskEXIT_CRITICAL();
    if (!pro2_rumble_decode_usb(report, len, left, right)) {
        taskENTER_CRITICAL();
        rejected_reports++;
        taskEXIT_CRITICAL();
        return;
    }
    pro2_rumble_make_zero(zero);
    taskENTER_CRITICAL();
    decoded_reports++;
    memcpy(active_left, left, 5);
    memcpy(active_right, right, 5);
    if (!memcmp(left, zero, 5) && !memcmp(right, zero, 5)) {
        active_until = 0;
        stop_packets = 3;
    } else {
        active_until = bflb_mtimer_get_time_us() + 180000u;
        stop_packets = 0;
    }
    taskEXIT_CRITICAL();
}

static void rumble_task(void *arg)
{
    (void)arg;
    uint8_t left[5], right[5], packet[33];
    for (;;) {
        bool send = false;
        uint64_t now = bflb_mtimer_get_time_us();
        taskENTER_CRITICAL();
        if (active_until && now <= active_until) {
            memcpy(left, active_left, 5);
            memcpy(right, active_right, 5);
            send = true;
        } else if (active_until) {
            active_until = 0;
            stop_packets = 3;
        }
        if (!send && stop_packets) {
            pro2_rumble_make_zero(left);
            pro2_rumble_make_zero(right);
            stop_packets--;
            send = true;
        }
        taskEXIT_CRITICAL();
        if (send) {
            int result;
            pro2_rumble_build_packet(packet_id++, left, right, packet);
            result = pro2_ble_write_rumble(packet, sizeof(packet));
            taskENTER_CRITICAL();
            write_attempts++;
            last_write_result = result;
            memcpy(last_ble_prefix, packet, sizeof(last_ble_prefix));
            if (result == 0) {
                write_successes++;
            } else {
                write_failures++;
            }
            taskEXIT_CRITICAL();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void pro2_rumble_init(void)
{
    pro2_rumble_make_zero(active_left);
    pro2_rumble_make_zero(active_right);
    xTaskCreate(rumble_task, "rumble", 768, NULL,
                configMAX_PRIORITIES - 3, NULL);
}

size_t pro2_rumble_build_diagnostic_report(uint8_t *report,
                                           size_t report_max)
{
    if (!report || report_max < PRO2_RUMBLE_DIAGNOSTIC_REPORT_SIZE) return 0;
    memset(report, 0, PRO2_RUMBLE_DIAGNOSTIC_REPORT_SIZE);
    memcpy(report, "P2DR", 4);
    report[4] = 1;
    taskENTER_CRITICAL();
    report[5] = (active_until ? 1u : 0u) | (stop_packets ? 2u : 0u);
    report[6] = packet_id;
    report[7] = last_usb_len;
    put_le32(report + 8, usb_reports);
    put_le32(report + 12, decoded_reports);
    put_le32(report + 16, rejected_reports);
    put_le32(report + 20, write_attempts);
    put_le32(report + 24, write_successes);
    put_le32(report + 28, write_failures);
    put_le32(report + 32, (uint32_t)last_write_result);
    memcpy(report + 36, last_usb_prefix, sizeof(last_usb_prefix));
    memcpy(report + 52, last_ble_prefix, sizeof(last_ble_prefix));
    taskEXIT_CRITICAL();
    return PRO2_RUMBLE_DIAGNOSTIC_REPORT_SIZE;
}
#endif
