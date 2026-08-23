// SPDX-License-Identifier: GPL-3.0-only
#include "pro2_settings.h"

#include <string.h>

#ifndef PRO2_HOST_TEST
#include "debug_log.h"
#include "easyflash.h"
#endif

#define SETTINGS_KEY "pro2_cfg"
#define SETTINGS_STORAGE_VERSION 1u

typedef struct {
    uint8_t magic[4];
    uint8_t version;
    uint8_t rumble_percent;
    uint16_t stick_deadzone;
    uint8_t reserved[8];
} pro2_settings_record_t;

typedef char settings_record_size_must_be_16[
    sizeof(pro2_settings_record_t) == 16 ? 1 : -1];

static pro2_settings_record_t settings = {
    .magic = {'P', '2', 'S', 'C'},
    .version = SETTINGS_STORAGE_VERSION,
    .rumble_percent = PRO2_SETTINGS_DEFAULT_RUMBLE,
    .stick_deadzone = PRO2_SETTINGS_DEFAULT_DEADZONE,
};
static bool settings_dirty;

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static void put_le16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

#ifndef PRO2_HOST_TEST
static bool record_valid(const pro2_settings_record_t *record)
{
    return record &&
           !memcmp(record->magic, "P2SC", sizeof(record->magic)) &&
           record->version == SETTINGS_STORAGE_VERSION &&
           record->rumble_percent <= PRO2_SETTINGS_MAX_RUMBLE &&
           record->stick_deadzone <= PRO2_SETTINGS_MAX_DEADZONE;
}
#endif

void pro2_settings_reset_defaults(void)
{
    memset(&settings, 0, sizeof(settings));
    memcpy(settings.magic, "P2SC", sizeof(settings.magic));
    settings.version = SETTINGS_STORAGE_VERSION;
    settings.rumble_percent = PRO2_SETTINGS_DEFAULT_RUMBLE;
    settings.stick_deadzone = PRO2_SETTINGS_DEFAULT_DEADZONE;
    settings_dirty = true;
}

void pro2_settings_init(void)
{
    pro2_settings_reset_defaults();
#ifndef PRO2_HOST_TEST
    pro2_settings_record_t stored;
    size_t stored_len = ef_get_env_blob(SETTINGS_KEY, &stored,
                                        sizeof(stored), NULL);
    if (stored_len == sizeof(stored) && record_valid(&stored)) {
        settings = stored;
        LOG_INF("[CFG] loaded: deadzone=%u rumble=%u%%\r\n",
                settings.stick_deadzone, settings.rumble_percent);
    } else {
        LOG_INF("[CFG] using defaults: deadzone=%u rumble=%u%%\r\n",
                settings.stick_deadzone, settings.rumble_percent);
    }
#endif
    settings_dirty = false;
}

bool pro2_settings_apply_wire(const uint8_t *data, size_t len)
{
    if (!data || len < 8u || memcmp(data, "P2CA", 4) ||
        data[4] != PRO2_SETTINGS_PROTOCOL_VERSION) {
        return false;
    }
    uint8_t rumble_percent = data[5];
    uint16_t stick_deadzone = read_le16(data + 6);
    if (rumble_percent > PRO2_SETTINGS_MAX_RUMBLE ||
        stick_deadzone > PRO2_SETTINGS_MAX_DEADZONE) {
        return false;
    }
    if (settings.rumble_percent != rumble_percent ||
        settings.stick_deadzone != stick_deadzone) {
        settings.rumble_percent = rumble_percent;
        settings.stick_deadzone = stick_deadzone;
        settings_dirty = true;
    }
    return true;
}

bool pro2_settings_save(void)
{
#ifndef PRO2_HOST_TEST
    if (ef_set_env_blob(SETTINGS_KEY, &settings, sizeof(settings)) != 0) {
        LOG_ERR("[CFG] save failed\r\n");
        return false;
    }
    LOG_INF("[CFG] saved: deadzone=%u rumble=%u%%\r\n",
            settings.stick_deadzone, settings.rumble_percent);
#endif
    settings_dirty = false;
    return true;
}

bool pro2_settings_dirty(void)
{
    return settings_dirty;
}

uint16_t pro2_settings_stick_deadzone(void)
{
    return settings.stick_deadzone;
}

uint8_t pro2_settings_rumble_percent(void)
{
    return settings.rumble_percent;
}

size_t pro2_settings_build_report(uint8_t *report, size_t report_max,
                                  pro2_web_command_t last_command,
                                  pro2_web_status_t last_status,
                                  uint8_t action_sequence)
{
    if (!report || report_max < PRO2_SETTINGS_REPORT_SIZE) return 0;
    memset(report, 0, PRO2_SETTINGS_REPORT_SIZE);
    memcpy(report, "P2CF", 4);
    report[4] = PRO2_SETTINGS_PROTOCOL_VERSION;
    report[5] = SETTINGS_STORAGE_VERSION;
    report[6] = settings.rumble_percent;
    put_le16(report + 7, settings.stick_deadzone);
    /* Capabilities: config, rumble test, pairing reset, BLE rate and input. */
    report[9] = 0x1fu;
    report[10] = (uint8_t)last_command;
    report[11] = (uint8_t)last_status;
    report[12] = settings_dirty ? 1u : 0u;
    report[13] = action_sequence;
    memcpy(report + 16, "Pro2 BL616 Web Config v1", 24);
    return PRO2_SETTINGS_REPORT_SIZE;
}
