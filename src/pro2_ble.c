// SPDX-License-Identifier: GPL-3.0-only
#include "pro2_ble.h"

#include <ctype.h>
#include <string.h>

#include "board.h"
#include "bflb_efuse.h"
#include "bflb_mtimer.h"
#include "FreeRTOS.h"
#include "task.h"
#include "bluetooth.h"
#include "conn.h"
#include "conn_internal.h"
#include "gatt.h"
#include "hci_driver.h"
#include "hci_core.h"
#include "keys.h"
#include "btble_lib_api.h"
#include "bluetooth/crypto.h"
#include "easyflash.h"
#include "net/buf.h"

#include "board_status.h"
#include "debug_log.h"
#include "pro2_protocol.h"
#include "usb_nintendo.h"

#define PEER_KEY                 "pro2_peer"
#define LOCAL_ADDRESS_KEY        "pro2_local"
#define PAIRING_RECORD_KEY       "pro2_pair"
#define SETUP_TIMEOUT_MS         8000
#define ACK_SUBSCRIBE_TIMEOUT_MS 2000
#define COMMAND_ACK_TIMEOUT_MS   1500
#define FIRST_FD2_TIMEOUT_MS     3000
#define PEER_MTU_GRACE_MS        400
#define SECURITY_TIMEOUT_MS      2500
#define SCAN_CANCEL_SETTLE_MS    40
#define RECONNECT_BACKOFF_MS     800
#define MIN_COMMAND_ATT_MTU      31u /* opcode + handle + 28-byte init_06 */
#define STABLE_REPORT_COUNT      180
#define DIAGNOSTIC_VERSION       10u
#define ACK_RESPONSE_MAX         64u

#define LOCAL_ADDR_RESTORED      (1u << 0)
#define LOCAL_ADDR_CAPTURED      (1u << 1)
#define LOCAL_ADDR_DERIVED       (1u << 2)
#define LOCAL_ADDR_SAVED         (1u << 3)
#define LOCAL_ADDR_CONTROLLER_OK (1u << 4)
#define LOCAL_ADDR_HOST_OK       (1u << 5)
#define MTU_READY_FROM_PEER       (1u << 6)
#define MTU_READY_AFTER_REQUEST   (1u << 7)

#define AUTO_CONNECT_SELECTED     (1u << 0)
#define AUTO_CONNECT_ARMED        (1u << 1)
#define AUTO_CONNECT_START_FAILED (1u << 2)
#define PAIRING_REQUIRED           (1u << 3)
#define PAIRING_STARTED            (1u << 4)
#define PAIRING_ADDRESS_OK         (1u << 5)
#define PAIRING_CHALLENGE_OK       (1u << 6)
#define PAIRING_COMMITTED          (1u << 7)

enum {
    PAIRING_STEP_NONE = 0,
    PAIRING_STEP_ADDRESS,
    PAIRING_STEP_KEY,
    PAIRING_STEP_CHALLENGE,
    PAIRING_STEP_FINALIZE,
    PAIRING_STEP_COMPLETE,
};

static struct bt_uuid_128 uuid_fd2 =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xab7de9be,0x89fe,0x49ad,0x828f,0x118f09df7fd2));
static struct bt_uuid_128 uuid_ack =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xc765a961,0xd9d8,0x4d36,0xa20a,0x5315b111836a));
static struct bt_uuid_128 uuid_command =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x649d4ac9,0x8eb7,0x4e6c,0xaf44,0x1ea54fe5f005));
static struct bt_uuid_128 uuid_rumble =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xcc483f51,0x9258,0x427d,0xa939,0x630c31f72b05));

typedef struct {
    uint16_t declaration;
    uint16_t value;
    uint16_t end;
    uint16_t ccc;
    uint8_t properties;
} characteristic_t;

typedef struct {
    char name[32];
    bool company_match;
} scan_info_t;

static struct bt_conn *connection;
static TaskHandle_t setup_task_handle;
static TaskHandle_t connect_task_handle;
static TaskHandle_t scan_retry_task_handle;
static struct bt_gatt_discover_params discover_params;
static struct bt_gatt_subscribe_params ack_subscribe;
static struct bt_gatt_subscribe_params fd2_subscribe;
static struct bt_gatt_exchange_params mtu_exchange_params;
static struct bt_gatt_write_params command_write_params;
static characteristic_t ch_fd2, ch_ack, ch_command, ch_rumble;
static pro2_parser_t parser;
static pro2_state_t controller_state;
static bt_addr_le_t saved_peer;
static bt_addr_le_t pending_peer;
static bool saved_peer_valid;
static volatile bool scanning;
static volatile bool connecting;
static volatile bool stack_ready;
static volatile bool ready;
static volatile bool auto_connect_enabled;
static volatile bool pending_pairing_required;
static volatile bool connection_pairing_required;
static volatile bool saved_ltk_valid;
static uint32_t report_count;
static characteristic_t *range_target;
static characteristic_t *descriptor_target;
static volatile bool awaiting_first_fd2;
static volatile bool local_disconnect;
static volatile bool ack_ccc_completed;
static volatile uint32_t ack_notification_count;
static volatile uint32_t ack_response_generation;
static volatile uint16_t ack_response_len;
static volatile uint8_t ack_response[ACK_RESPONSE_MAX];
static volatile bool mtu_exchange_completed;
static volatile uint8_t mtu_exchange_att_error;
static volatile bool command_write_pending;
static volatile uint8_t command_write_att_error;
static volatile bool security_completed;
static volatile uint8_t security_level;
static volatile uint8_t security_error;
static uint8_t saved_ltk[16];

typedef struct {
    uint8_t magic[4];
    bt_addr_le_t peer;
    uint8_t ltk[16];
} pairing_record_t;

typedef struct {
    volatile uint8_t stage;
    volatile uint8_t last_error;
    volatile int32_t last_code;
    volatile uint32_t scan_reports;
    volatile uint32_t candidates;
    volatile uint32_t connect_attempts;
    volatile uint32_t connect_successes;
    volatile uint32_t disconnects;
    volatile uint32_t fd2_reports;
    volatile uint32_t ack_reports;
    volatile uint16_t att_mtu;
    volatile uint8_t init_index;
    volatile uint8_t last_disconnect_reason;
    volatile uint8_t peer_type;
    volatile uint8_t peer[6];
    volatile uint8_t local_address_flags;
    volatile bool last_candidate_directed;
    volatile bool directed_candidate_seen;
    volatile uint8_t auto_connect_flags;
    volatile uint8_t pairing_step;
} diagnostic_state_t;

static diagnostic_state_t diagnostics;

typedef struct {
    volatile bool captured;
    volatile bool saved_match;
    volatile bool candidate_match;
    volatile uint8_t event_type;
    volatile uint8_t event_type_mask;
    volatile int8_t rssi;
    volatile uint8_t peer_type;
    volatile uint8_t peer[6];
    volatile uint8_t payload_len;
    volatile uint8_t payload_copied;
    volatile uint8_t payload[32];
    volatile uint32_t matching_reports;
} advertisement_diagnostic_t;

static advertisement_diagnostic_t advertisement_diagnostic;

/* sqlCRT's SDK fork keeps two application pools in the global pool table.
 * This BLE-only bridge does not consume them, but they must be instantiated. */
struct net_buf_pool hid_tx_pool;
struct net_buf_pool hid_rx_pool;

typedef struct {
    const uint8_t *data;
    uint8_t len;
} init_command_t;

static const uint8_t init_00[] = {0x03,0x91,0x01,0x0d,0,0x08,0,0,0x01,0,0xff,0xff,0xff,0xff,0xff,0xff};
static const uint8_t init_01[] = {0x07,0x91,0x01,0x01,0,0,0,0};
static const uint8_t init_02[] = {0x16,0x91,0x01,0x01,0,0,0,0};
static const uint8_t init_03[] = {0x15,0x91,0x01,0x03,0,0x01,0,0,0};
static const uint8_t init_04[] = {0x0c,0x91,0x01,0x02,0,0x04,0,0,0xff,0,0,0};
static const uint8_t init_05[] = {0x11,0x91,0x01,0x03,0,0,0,0};
static const uint8_t init_06[] = {0x0a,0x91,0x01,0x08,0,0x14,0,0,0x01,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x35,0,0x46,0,0,0,0,0,0,0,0};
static const uint8_t init_07[] = {0x0c,0x91,0x01,0x04,0,0x04,0,0,0xff,0,0,0};
static const uint8_t init_08[] = {0x03,0x91,0x01,0x0a,0,0x04,0,0,0x09,0,0,0};
static const uint8_t init_09[] = {0x10,0x91,0x01,0x01,0,0,0,0};
static const uint8_t init_10[] = {0x01,0x91,0x01,0x0c,0,0,0,0};
static const uint8_t init_11[] = {0x01,0x91,0x01,0x01,0,0x04,0,0,0,0,0,0};
static const uint8_t init_12[] = {0x09,0x91,0x01,0x07,0,0x08,0,0,0x01,0,0,0,0,0,0,0};
static const uint8_t init_13[] = {0x02,0x91,0x01,0x04,0,0x08,0,0,0x09,0x7e,0,0,0xa8,0x30,0x01,0};
static const uint8_t init_14[] = {0x02,0x91,0x01,0x04,0,0x08,0,0,0x09,0x7e,0,0,0xe8,0x30,0x01,0};

#define INIT_CMD(x) { (x), (uint8_t)sizeof(x) }
static const init_command_t init_commands[] = {
    INIT_CMD(init_00), INIT_CMD(init_01), INIT_CMD(init_02), INIT_CMD(init_03),
    INIT_CMD(init_04), INIT_CMD(init_05), INIT_CMD(init_06), INIT_CMD(init_07),
    INIT_CMD(init_08), INIT_CMD(init_09), INIT_CMD(init_10), INIT_CMD(init_11),
    INIT_CMD(init_12), INIT_CMD(init_13), INIT_CMD(init_14),
};

static void start_scan(void);
static void start_reconnect(void);

static const struct bt_le_conn_param initial_conn_param = {
    /* Match the working ESP bridge from the first connection event. */
    .interval_min = 6,
    .interval_max = 6,
    .latency = 0,
    .timeout = 400,
};

static void disable_auto_connect(void)
{
    if (auto_connect_enabled) {
        int err = bt_conn_create_auto_stop();
        if (err) {
            LOG_WRN("[BLE] whitelist auto connect stop failed: %d\r\n", err);
        }
    }
    auto_connect_enabled = false;
    diagnostics.auto_connect_flags &= (uint8_t)~AUTO_CONNECT_ARMED;

    if (stack_ready) {
        int err = bt_le_whitelist_clear();
        if (err) {
            LOG_WRN("[BLE] whitelist clear failed: %d\r\n", err);
        }
    }
}

static int enable_auto_connect(void)
{
    if (!saved_peer_valid) return -1;
    diagnostics.auto_connect_flags |= AUTO_CONNECT_SELECTED;
    if (auto_connect_enabled) return 0;

    int err = bt_le_whitelist_clear();
    if (!err) err = bt_le_whitelist_add(&saved_peer);
    if (!err) err = bt_conn_create_auto_le(&initial_conn_param);
    if (err) {
        diagnostics.auto_connect_flags &= (uint8_t)~AUTO_CONNECT_ARMED;
        diagnostics.auto_connect_flags |= AUTO_CONNECT_START_FAILED;
        LOG_ERR("[BLE] whitelist auto connect start failed: %d\r\n", err);
        return err;
    }

    auto_connect_enabled = true;
    diagnostics.auto_connect_flags |= AUTO_CONNECT_ARMED;
    diagnostics.auto_connect_flags &= (uint8_t)~AUTO_CONNECT_START_FAILED;
    LOG_INF("[BLE] whitelist auto connect armed for saved peer\r\n");
    return 0;
}

static void diagnostic_set_stage(pro2_ble_stage_t stage)
{
    diagnostics.stage = (uint8_t)stage;
}

static void diagnostic_set_error(pro2_ble_error_t error, int32_t code)
{
    diagnostics.last_error = (uint8_t)error;
    diagnostics.last_code = code;
}

static void diagnostic_set_peer(const bt_addr_le_t *addr)
{
    if (!addr) return;
    diagnostics.peer_type = addr->type;
    for (size_t i = 0; i < sizeof(addr->a.val); ++i) {
        diagnostics.peer[i] = addr->a.val[i];
    }
}

static void disconnect_after_error(void)
{
    if (connection) {
        /* A local disconnect clears BT_CONN_AUTO_CONNECT in the stack. Keep
         * our mirror honest so the retry task re-arms it after teardown. */
        disable_auto_connect();
        local_disconnect = true;
        bt_conn_disconnect(connection, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    } else {
        local_disconnect = false;
    }
}

static void disconnect_with_error(pro2_ble_error_t error, int32_t code)
{
    diagnostic_set_error(error, code);
    disconnect_after_error();
}

static void put_le16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static bool advertised_host_address(const struct net_buf_simple *buf,
                                    uint8_t host[6])
{
    if (!buf || !host) return false;
    size_t offset = 0;
    while (offset < buf->len) {
        uint8_t field_len = buf->data[offset];
        if (!field_len) break;
        size_t field_end = offset + 1u + field_len;
        if (field_end > buf->len || field_len < 1u) break;
        if (buf->data[offset + 1u] == BT_DATA_MANUFACTURER_DATA &&
            field_len >= 21u && buf->data[offset + 2u] == 0x53u &&
            buf->data[offset + 3u] == 0x05u) {
            memcpy(host, buf->data + offset + 14u, 6u);
            return true;
        }
        offset = field_end;
    }
    return false;
}

static bool address_is_all_zero(const uint8_t address[6])
{
    for (size_t i = 0; i < 6u; ++i) {
        if (address[i]) return false;
    }
    return true;
}

static void capture_advertisement(const bt_addr_le_t *addr, int8_t rssi,
                                  uint8_t event_type,
                                  const struct net_buf_simple *buf,
                                  bool saved_match, bool candidate_match)
{
    if (!addr || !buf) return;
    advertisement_diagnostic.captured = true;
    advertisement_diagnostic.saved_match = saved_match;
    advertisement_diagnostic.candidate_match = candidate_match;
    advertisement_diagnostic.event_type = event_type;
    if (event_type < 8u) {
        advertisement_diagnostic.event_type_mask |= (uint8_t)(1u << event_type);
    }
    advertisement_diagnostic.rssi = rssi;
    advertisement_diagnostic.peer_type = addr->type;
    memcpy((void *)advertisement_diagnostic.peer, addr->a.val,
           sizeof(advertisement_diagnostic.peer));
    advertisement_diagnostic.payload_len =
        buf->len > UINT8_MAX ? UINT8_MAX : (uint8_t)buf->len;
    size_t copied = buf->len;
    if (copied > sizeof(advertisement_diagnostic.payload)) {
        copied = sizeof(advertisement_diagnostic.payload);
    }
    advertisement_diagnostic.payload_copied = (uint8_t)copied;
    memset((void *)advertisement_diagnostic.payload, 0,
           sizeof(advertisement_diagnostic.payload));
    memcpy((void *)advertisement_diagnostic.payload, buf->data, copied);
    advertisement_diagnostic.matching_reports++;

    uint8_t advertised_host[6];
    pending_pairing_required =
        advertised_host_address(buf, advertised_host) &&
        address_is_all_zero(advertised_host);
    if (pending_pairing_required) {
        diagnostics.auto_connect_flags |= PAIRING_REQUIRED;
    } else {
        diagnostics.auto_connect_flags &= (uint8_t)~PAIRING_REQUIRED;
    }
}

static bool contains_casefold(const char *text, const char *needle)
{
    if (!text || !needle || !*needle) return false;
    for (; *text; ++text) {
        const char *a = text;
        const char *b = needle;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) {
            ++a; ++b;
        }
        if (!*b) return true;
    }
    return false;
}

static bool scan_data_cb(struct bt_data *data, void *user_data)
{
    scan_info_t *info = user_data;
    if (data->type == BT_DATA_NAME_SHORTENED || data->type == BT_DATA_NAME_COMPLETE) {
        size_t n = data->data_len < sizeof(info->name) - 1 ?
                   data->data_len : sizeof(info->name) - 1;
        memcpy(info->name, data->data, n);
        info->name[n] = '\0';
    } else if (data->type == BT_DATA_MANUFACTURER_DATA && data->data_len >= 2) {
        uint16_t company = (uint16_t)data->data[0] | ((uint16_t)data->data[1] << 8);
        if (company == 0x0553u) info->company_match = true;
    }
    return true;
}

static bool candidate(const scan_info_t *info)
{
    return info->company_match || contains_casefold(info->name, "switch") ||
           contains_casefold(info->name, "nintendo") ||
           contains_casefold(info->name, "pro controller") ||
           contains_casefold(info->name, "pro2");
}

static void scan_retry_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(RECONNECT_BACKOFF_MS));
    scan_retry_task_handle = NULL;
    if (!connection && !connecting && !scanning) start_reconnect();
    vTaskDelete(NULL);
}

static void schedule_scan_retry(void)
{
    if (connection || connecting || scanning || auto_connect_enabled ||
        scan_retry_task_handle) return;
    if (xTaskCreate(scan_retry_task, "pro2_rescan", 1024, NULL,
                    configMAX_PRIORITIES - 3, &scan_retry_task_handle) != pdPASS) {
        scan_retry_task_handle = NULL;
        if (!saved_peer_valid) start_scan();
    }
}

static void connect_selected_task(void *arg)
{
    (void)arg;
    /* The working ESP reconnect path waits until scan cancellation has
     * settled before starting the initiator.  Keep HCI connection creation
     * out of the BL616 scan callback for the same reason. */
    vTaskDelay(pdMS_TO_TICKS(SCAN_CANCEL_SETTLE_MS));
    if (connecting && !connection) {
        if (saved_peer_valid) {
            diagnostic_set_stage(PRO2_BLE_STAGE_AUTO_CONNECT);
            int err = enable_auto_connect();
            if (err) {
                diagnostic_set_error(PRO2_BLE_ERROR_CONNECT_START, err);
                connecting = false;
            }
        } else if (!bt_conn_create_le(&pending_peer, &initial_conn_param)) {
            diagnostic_set_error(PRO2_BLE_ERROR_CONNECT_START, -1);
            connecting = false;
        }
    }
    connect_task_handle = NULL;
    if (!connection && !connecting) schedule_scan_retry();
    vTaskDelete(NULL);
}

static void device_found(const bt_addr_le_t *addr, int8_t rssi, uint8_t evtype,
                         struct net_buf_simple *buf)
{
    scan_info_t info = {0};
    bool saved_match = false;
    char address[BT_ADDR_LE_STR_LEN];
    diagnostics.scan_reports++;
    if (connection || connecting) return;
    if (saved_peer_valid) {
        if (bt_addr_cmp(&addr->a, &saved_peer.a)) return;
        saved_match = true;
    } else {
        struct net_buf_simple parsed = *buf;
        bt_data_parse(&parsed, scan_data_cb, &info);
        if (!candidate(&info)) return;
    }
    capture_advertisement(addr, rssi, evtype, buf, saved_match,
                          candidate(&info));
    diagnostics.candidates++;
    diagnostics.last_candidate_directed = evtype == BT_LE_ADV_DIRECT_IND;
    if (diagnostics.last_candidate_directed) {
        diagnostics.directed_candidate_seen = true;
    }
    diagnostic_set_peer(addr);
    bt_addr_le_to_str(addr, address, sizeof(address));
    LOG_INF("[BLE] candidate %s rssi=%d name=%s\r\n", address, rssi, info.name);
    int err = bt_le_scan_stop();
    if (err) {
        diagnostic_set_error(PRO2_BLE_ERROR_SCAN_STOP, err);
        return;
    }
    scanning = false;
    connecting = true;
    if (!saved_peer_valid) diagnostics.connect_attempts++;
    diagnostic_set_stage(PRO2_BLE_STAGE_CONNECTING);
    board_status_set(BOARD_STATUS_CONNECTING);
    bt_addr_le_copy(&pending_peer, addr);
    if (connect_task_handle ||
        xTaskCreate(connect_selected_task, "pro2_connect", 1024, NULL,
                    configMAX_PRIORITIES - 2, &connect_task_handle) != pdPASS) {
        diagnostic_set_error(PRO2_BLE_ERROR_CONNECT_START, -1);
        connecting = false;
        connect_task_handle = NULL;
        schedule_scan_retry();
    }
}

static void start_scan(void)
{
    static const struct bt_le_scan_param param = {
        .type = BT_LE_SCAN_TYPE_ACTIVE,
        .filter_dup = BT_LE_SCAN_FILTER_DUPLICATE,
        .interval = BT_GAP_SCAN_FAST_INTERVAL,
        .window = BT_GAP_SCAN_FAST_WINDOW,
    };
    if (connection || connecting || scanning || auto_connect_enabled) return;
    board_status_set(BOARD_STATUS_SCANNING);
    int err = bt_le_scan_start(&param, device_found);
    scanning = err == 0;
    diagnostic_set_stage(PRO2_BLE_STAGE_SCANNING);
    if (err) diagnostic_set_error(PRO2_BLE_ERROR_SCAN_START, err);
    LOG_INF("[BLE] scan %s (%d)\r\n", scanning ? "started" : "failed", err);
}

static void start_reconnect(void)
{
    if (connection || connecting || scanning || auto_connect_enabled) return;
    /* Scan once in the host before arming whitelist initiation. Besides
     * matching the working reference flow, this preserves the exact wake
     * advertisement for USB-only diagnostics. */
    start_scan();
}

static characteristic_t *match_characteristic(const struct bt_uuid *uuid)
{
    if (!bt_uuid_cmp(uuid, &uuid_fd2.uuid)) return &ch_fd2;
    if (!bt_uuid_cmp(uuid, &uuid_ack.uuid)) return &ch_ack;
    if (!bt_uuid_cmp(uuid, &uuid_command.uuid)) return &ch_command;
    if (!bt_uuid_cmp(uuid, &uuid_rumble.uuid)) return &ch_rumble;
    return NULL;
}

static uint8_t discover_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           struct bt_gatt_discover_params *params)
{
    (void)conn;
    if (!attr) {
        if (range_target) {
            range_target->end = 0xffff;
            range_target = NULL;
        }
        memset(params, 0, sizeof(*params));
        if (setup_task_handle) xTaskNotifyGive(setup_task_handle);
        return BT_GATT_ITER_STOP;
    }
    if (range_target) {
        range_target->end = (uint16_t)(attr->handle - 1u);
        range_target = NULL;
    }
    struct bt_gatt_chrc *chrc = attr->user_data;
    characteristic_t *found = match_characteristic(chrc->uuid);
    if (found) {
        found->declaration = attr->handle;
        found->value = chrc->value_handle;
        found->properties = chrc->properties;
        range_target = found;
    }
    return BT_GATT_ITER_CONTINUE;
}

static uint8_t descriptor_cb(struct bt_conn *conn,
                             const struct bt_gatt_attr *attr,
                             struct bt_gatt_discover_params *params)
{
    (void)conn;
    if (!attr) {
        memset(params, 0, sizeof(*params));
        if (setup_task_handle) xTaskNotifyGive(setup_task_handle);
        return BT_GATT_ITER_STOP;
    }
    if (descriptor_target && !bt_uuid_cmp(attr->uuid, BT_UUID_GATT_CCC)) {
        descriptor_target->ccc = attr->handle;
    }
    return BT_GATT_ITER_CONTINUE;
}

static bool discover_ccc(characteristic_t *ch)
{
    if (!connection || !ch || !ch->value || ch->end <= ch->value) {
        diagnostic_set_error(PRO2_BLE_ERROR_CCC_MISSING,
                             ch ? ch->value : 0);
        return false;
    }
    memset(&discover_params, 0, sizeof(discover_params));
    descriptor_target = ch;
    discover_params.uuid = BT_UUID_GATT_CCC;
    discover_params.func = descriptor_cb;
    discover_params.start_handle = (uint16_t)(ch->value + 1u);
    discover_params.end_handle = ch->end;
    discover_params.type = BT_GATT_DISCOVER_DESCRIPTOR;
    diagnostic_set_stage(PRO2_BLE_STAGE_DESCRIPTOR_DISCOVERY);
    int err = bt_gatt_discover(connection, &discover_params);
    if (err) {
        descriptor_target = NULL;
        diagnostic_set_error(PRO2_BLE_ERROR_DESCRIPTOR_START, err);
        return false;
    }
    if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SETUP_TIMEOUT_MS))) {
        descriptor_target = NULL;
        diagnostic_set_error(PRO2_BLE_ERROR_DESCRIPTOR_TIMEOUT, ch->value);
        return false;
    }
    descriptor_target = NULL;
    if (!ch->ccc) {
        diagnostic_set_error(PRO2_BLE_ERROR_CCC_MISSING, ch->value);
        return false;
    }
    return true;
}

static uint8_t ack_notify(struct bt_conn *conn,
                          struct bt_gatt_subscribe_params *params,
                          const void *data, uint16_t length)
{
    (void)conn;
    if (!data) {
        if (!params->value) {
            params->value_handle = 0;
            return BT_GATT_ITER_STOP;
        }
        /* This SDK enables BFLB_BLE_PATCH_NOTIFY_WRITE_CCC_RSP and calls the
           notify callback with NULL after the CCC write response. */
        ack_ccc_completed = true;
        if (setup_task_handle) xTaskNotifyGive(setup_task_handle);
        return BT_GATT_ITER_CONTINUE;
    }
    size_t copied = length;
    if (copied > sizeof(ack_response)) copied = sizeof(ack_response);
    memcpy((void *)ack_response, data, copied);
    ack_response_len = (uint16_t)copied;
    ack_response_generation++;
    ack_notification_count++;
    diagnostics.ack_reports++;
    if (setup_task_handle) xTaskNotifyGive(setup_task_handle);
    return BT_GATT_ITER_CONTINUE;
}

static uint8_t fd2_notify(struct bt_conn *conn,
                          struct bt_gatt_subscribe_params *params,
                          const void *data, uint16_t length)
{
    if (!data && !params->value) {
        params->value_handle = 0;
        return BT_GATT_ITER_STOP;
    }
    if (data) {
        diagnostics.fd2_reports++;
        if (awaiting_first_fd2 && setup_task_handle) {
            xTaskNotifyGive(setup_task_handle);
        }
    }
    if (!data || !ready || pro2_parse_fd2(&parser, &controller_state, data, length)) {
        return BT_GATT_ITER_CONTINUE;
    }
    usb_nintendo_submit_state(&controller_state,
                              (uint32_t)bflb_mtimer_get_time_us());
    if (++report_count == STABLE_REPORT_COUNT) {
        static const struct bt_le_conn_param fast = {
            .interval_min = 6, .interval_max = 6, .latency = 0, .timeout = 400,
        };
        int err = bt_conn_le_param_update(conn, &fast);
        LOG_INF("[BLE] request 7.5 ms interval (%d)\r\n", err);
    }
    return BT_GATT_ITER_CONTINUE;
}

static int subscribe(characteristic_t *ch, struct bt_gatt_subscribe_params *params,
                     bt_gatt_notify_func_t callback)
{
    if (!ch->value || !ch->ccc) return -1;
    memset(params, 0, sizeof(*params));
    params->notify = callback;
    params->value_handle = ch->value;
    params->ccc_handle = ch->ccc;
    params->value = BT_GATT_CCC_NOTIFY;
    return bt_gatt_subscribe(connection, params);
}

static void mtu_exchange_cb(struct bt_conn *conn, uint8_t err,
                            struct bt_gatt_exchange_params *params)
{
    (void)params;
    mtu_exchange_att_error = err;
    if (conn) diagnostics.att_mtu = bt_gatt_get_mtu(conn);
    mtu_exchange_completed = true;
    if (setup_task_handle) xTaskNotifyGive(setup_task_handle);
}

static bool exchange_command_mtu(void)
{
    TickType_t grace_start = xTaskGetTickCount();
    TickType_t grace_timeout = pdMS_TO_TICKS(PEER_MTU_GRACE_MS);
    while (connection) {
        diagnostics.att_mtu = bt_gatt_get_mtu(connection);
        if (diagnostics.att_mtu >= MIN_COMMAND_ATT_MTU) {
            diagnostics.local_address_flags |= MTU_READY_FROM_PEER;
            LOG_INF("[BLE] peer negotiated ATT MTU before local request: %u\r\n",
                    diagnostics.att_mtu);
            return true;
        }
        TickType_t elapsed = xTaskGetTickCount() - grace_start;
        if (elapsed >= grace_timeout) break;
        TickType_t remaining = grace_timeout - elapsed;
        TickType_t delay = pdMS_TO_TICKS(20);
        vTaskDelay(delay < remaining ? delay : remaining);
    }
    if (!connection) return false;

    while (ulTaskNotifyTake(pdTRUE, 0)) {}
    memset(&mtu_exchange_params, 0, sizeof(mtu_exchange_params));
    mtu_exchange_params.func = mtu_exchange_cb;
    mtu_exchange_completed = false;
    mtu_exchange_att_error = 0;
    diagnostics.att_mtu = bt_gatt_get_mtu(connection);
    diagnostic_set_stage(PRO2_BLE_STAGE_MTU_EXCHANGE);

    int err = bt_gatt_exchange_mtu(connection, &mtu_exchange_params);
    if (err) {
        diagnostics.att_mtu = bt_gatt_get_mtu(connection);
        if (diagnostics.att_mtu >= MIN_COMMAND_ATT_MTU) return true;
        LOG_ERR("[BLE] MTU exchange start failed: %d (mtu=%u)\r\n",
                err, diagnostics.att_mtu);
        diagnostic_set_error(PRO2_BLE_ERROR_MTU_EXCHANGE, err);
        return false;
    }

    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(SETUP_TIMEOUT_MS);
    while (!mtu_exchange_completed && connection) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeout) break;
        ulTaskNotifyTake(pdTRUE, timeout - elapsed);
    }
    if (!connection) return false;

    diagnostics.att_mtu = bt_gatt_get_mtu(connection);
    if (!mtu_exchange_completed) {
        if (diagnostics.att_mtu >= MIN_COMMAND_ATT_MTU) return true;
        LOG_ERR("[BLE] MTU exchange timeout (mtu=%u)\r\n", diagnostics.att_mtu);
        diagnostic_set_error(PRO2_BLE_ERROR_MTU_EXCHANGE, -3);
        return false;
    }
    if (diagnostics.att_mtu >= MIN_COMMAND_ATT_MTU) {
        diagnostics.local_address_flags |= MTU_READY_AFTER_REQUEST;
        if (mtu_exchange_att_error) {
            LOG_WRN("[BLE] MTU request returned err=%u but negotiated MTU is usable: %u\r\n",
                    mtu_exchange_att_error, diagnostics.att_mtu);
        } else {
            LOG_INF("[BLE] ATT MTU negotiated: %u\r\n", diagnostics.att_mtu);
        }
        return true;
    }
    if (mtu_exchange_att_error) {
        TickType_t post_error_start = xTaskGetTickCount();
        while (connection &&
               xTaskGetTickCount() - post_error_start < grace_timeout) {
            diagnostics.att_mtu = bt_gatt_get_mtu(connection);
            if (diagnostics.att_mtu >= MIN_COMMAND_ATT_MTU) {
                diagnostics.local_address_flags |= MTU_READY_FROM_PEER;
                LOG_WRN("[BLE] peer MTU became usable after local err=%u: %u\r\n",
                        mtu_exchange_att_error, diagnostics.att_mtu);
                return true;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    if (mtu_exchange_att_error || diagnostics.att_mtu < MIN_COMMAND_ATT_MTU) {
        int32_t code = mtu_exchange_att_error ? mtu_exchange_att_error
                                              : diagnostics.att_mtu;
        LOG_ERR("[BLE] MTU exchange unusable: err=%u mtu=%u\r\n",
                mtu_exchange_att_error, diagnostics.att_mtu);
        diagnostic_set_error(PRO2_BLE_ERROR_MTU_EXCHANGE, code);
        return false;
    }
    return false;
}

static void command_write_cb(struct bt_conn *conn, uint8_t err,
                             struct bt_gatt_write_params *params)
{
    (void)conn;
    (void)params;
    command_write_att_error = err;
    command_write_pending = false;
    if (setup_task_handle) xTaskNotifyGive(setup_task_handle);
}

static int write_command(const init_command_t *command)
{
    if (ch_command.properties & BT_GATT_CHRC_WRITE_WITHOUT_RESP) {
        return bt_gatt_write_without_response(connection, ch_command.value,
                                              command->data, command->len, false);
    }
    if (ch_command.properties & BT_GATT_CHRC_WRITE) {
        memset(&command_write_params, 0, sizeof(command_write_params));
        command_write_params.func = command_write_cb;
        command_write_params.handle = ch_command.value;
        command_write_params.data = command->data;
        command_write_params.length = command->len;
        command_write_att_error = 0;
        command_write_pending = true;
        int err = bt_gatt_write(connection, &command_write_params);
        if (err) {
            command_write_pending = false;
            return err;
        }

        TickType_t start = xTaskGetTickCount();
        TickType_t timeout = pdMS_TO_TICKS(COMMAND_ACK_TIMEOUT_MS);
        while (command_write_pending && connection) {
            TickType_t elapsed = xTaskGetTickCount() - start;
            if (elapsed >= timeout) break;
            ulTaskNotifyTake(pdTRUE, timeout - elapsed);
        }
        if (command_write_pending) {
            command_write_pending = false;
            return -3;
        }
        if (command_write_att_error) return -(int)command_write_att_error;
        return 0;
    }
    return -2;
}

static struct bt_keys *install_pairing_ltk(const bt_addr_le_t *peer,
                                           const uint8_t ltk[16])
{
    if (!peer || !ltk) return NULL;
    struct bt_keys *keys = bt_keys_get_type(BT_KEYS_LTK_P256,
                                             BT_ID_DEFAULT, peer);
    if (!keys) return NULL;
    keys->enc_size = sizeof(keys->ltk.val);
    keys->flags |= BT_KEYS_SC;
    memset(keys->ltk.rand, 0, sizeof(keys->ltk.rand));
    memset(keys->ltk.ediv, 0, sizeof(keys->ltk.ediv));
    memcpy(keys->ltk.val, ltk, sizeof(keys->ltk.val));
    return keys;
}

static bool persist_pairing_ltk(const uint8_t ltk[16])
{
    if (!connection || !ltk) return false;
    pairing_record_t record = {0};
    memcpy(record.magic, "P2LK", sizeof(record.magic));
    bt_addr_le_copy(&record.peer, bt_conn_get_dst(connection));
    memcpy(record.ltk, ltk, sizeof(record.ltk));
    if (ef_set_env_blob(PAIRING_RECORD_KEY, &record, sizeof(record))) {
        return false;
    }
    memcpy(saved_ltk, ltk, sizeof(saved_ltk));
    saved_ltk_valid = true;
    struct bt_keys *keys = install_pairing_ltk(&record.peer, ltk);
    if (keys) connection->le.keys = keys;
    return keys != NULL;
}

static void restore_pairing_ltk(void)
{
    pairing_record_t record = {0};
    size_t stored = ef_get_env_blob(PAIRING_RECORD_KEY, &record,
                                    sizeof(record), NULL);
    saved_ltk_valid = stored == sizeof(record) &&
                      !memcmp(record.magic, "P2LK", sizeof(record.magic)) &&
                      saved_peer_valid &&
                      !bt_addr_le_cmp(&record.peer, &saved_peer);
    if (!saved_ltk_valid) return;
    memcpy(saved_ltk, record.ltk, sizeof(saved_ltk));
    if (!install_pairing_ltk(&saved_peer, saved_ltk)) {
        saved_ltk_valid = false;
        LOG_ERR("[BLE] unable to restore Pro2 LTK into key database\r\n");
    } else {
        LOG_INF("[BLE] restored proprietary Pro2 LTK\r\n");
    }
}

static bool establish_saved_encryption(void)
{
    if (!connection || !saved_ltk_valid || connection_pairing_required) {
        return true;
    }
    struct bt_keys *keys = install_pairing_ltk(bt_conn_get_dst(connection),
                                                saved_ltk);
    if (!keys) {
        diagnostic_set_error(PRO2_BLE_ERROR_SECURITY_START, -1);
        return false;
    }
    connection->le.keys = keys;
    if (bt_conn_get_security(connection) >= BT_SECURITY_L2) return true;

    /* BL616 connection objects are reused without clearing this field. */
    if (connection->required_sec_level >= BT_SECURITY_L2) {
        connection->required_sec_level = BT_SECURITY_L1;
    }
    security_completed = false;
    security_level = BT_SECURITY_L1;
    security_error = 0;
    diagnostic_set_stage(PRO2_BLE_STAGE_SECURING);
    int err = bt_conn_set_security(connection, BT_SECURITY_L2);
    if (err) {
        LOG_ERR("[BLE] saved-LTK encryption start failed: %d\r\n", err);
        diagnostic_set_error(PRO2_BLE_ERROR_SECURITY_START, err);
        return false;
    }

    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(SECURITY_TIMEOUT_MS);
    while (!security_completed && connection) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeout) break;
        ulTaskNotifyTake(pdTRUE, timeout - elapsed);
    }
    if (!connection) return false;
    if (!security_completed) {
        LOG_ERR("[BLE] saved-LTK encryption timeout\r\n");
        diagnostic_set_error(PRO2_BLE_ERROR_SECURITY_TIMEOUT, 0);
        return false;
    }
    if (security_error || security_level < BT_SECURITY_L2) {
        LOG_ERR("[BLE] saved-LTK encryption failed: level=%u err=%u\r\n",
                security_level, security_error);
        diagnostic_set_error(PRO2_BLE_ERROR_SECURITY_CHANGE,
                             ((int32_t)security_level << 8) | security_error);
        return false;
    }
    LOG_INF("[BLE] saved-LTK link encryption active\r\n");
    return true;
}

static bool pairing_command(uint8_t step, const uint8_t *data, uint8_t len,
                            uint8_t subcommand, uint8_t *response,
                            size_t *response_len)
{
    init_command_t command = { data, len };
    diagnostics.pairing_step = step;
    diagnostic_set_stage(PRO2_BLE_STAGE_PAIRING);
    uint32_t response_before = ack_response_generation;
    ack_response_len = 0;

    int err = write_command(&command);
    if (err) {
        LOG_ERR("[BLE] pairing step %u write failed: %d\r\n", step, err);
        diagnostic_set_error(PRO2_BLE_ERROR_PAIRING_WRITE,
                             ((int32_t)step << 16) | (uint16_t)err);
        return false;
    }

    TickType_t start = xTaskGetTickCount();
    TickType_t timeout = pdMS_TO_TICKS(COMMAND_ACK_TIMEOUT_MS);
    while (ack_response_generation == response_before && connection) {
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeout) break;
        ulTaskNotifyTake(pdTRUE, timeout - elapsed);
    }
    if (!connection) return false;
    if (ack_response_generation == response_before) {
        LOG_ERR("[BLE] pairing step %u response timeout\r\n", step);
        diagnostic_set_error(PRO2_BLE_ERROR_PAIRING_ACK_TIMEOUT, step);
        return false;
    }

    size_t copied = ack_response_len;
    if (copied < 8u || ack_response[0] != 0x15u ||
        ack_response[1] != 0x01u || ack_response[2] != 0x01u ||
        ack_response[3] != subcommand) {
        int32_t code = ((int32_t)step << 24) |
                       ((int32_t)copied << 16) |
                       ((int32_t)ack_response[0] << 8) |
                       ack_response[3];
        LOG_ERR("[BLE] pairing step %u invalid response len=%u cmd=%02x/%02x\r\n",
                step, (unsigned)copied, ack_response[0], ack_response[3]);
        diagnostic_set_error(PRO2_BLE_ERROR_PAIRING_RESPONSE, code);
        return false;
    }

    if (response && response_len) {
        size_t capacity = *response_len;
        if (copied > capacity) copied = capacity;
        memcpy(response, (const void *)ack_response, copied);
        *response_len = copied;
    }
    return true;
}

static bool run_proprietary_pairing(void)
{
    uint8_t response[ACK_RESPONSE_MAX];
    size_t response_len;
    bt_addr_le_t local_identity = {0};
    diagnostics.auto_connect_flags |= PAIRING_STARTED;
    diagnostics.auto_connect_flags &=
        (uint8_t)~(PAIRING_ADDRESS_OK | PAIRING_CHALLENGE_OK |
                   PAIRING_COMMITTED);

    if (bt_get_local_public_address(&local_identity)) {
        diagnostic_set_error(PRO2_BLE_ERROR_PAIRING_RESPONSE,
                             PAIRING_STEP_ADDRESS);
        return false;
    }

    uint8_t address_command[22] = {
        0x15,0x91,0x01,0x01,0x00,0x0e,0x00,0x00,0x00,0x02
    };
    memcpy(address_command + 10, local_identity.a.val, 6u);
    memcpy(address_command + 16, local_identity.a.val, 6u);
    /* The console supplies its two adjacent controller identities.  The
     * dongle has one public identity, so keep it first (the address used in
     * reconnect advertisements) and provide the adjacent identity second. */
    address_command[16] ^= 1u;
    response_len = sizeof(response);
    if (!pairing_command(PAIRING_STEP_ADDRESS, address_command,
                         sizeof(address_command), 0x01u,
                         response, &response_len)) {
        return false;
    }
    if (response_len < 17u || response[8] != 0x01u ||
        response[10] != 0x01u) {
        diagnostic_set_error(PRO2_BLE_ERROR_PAIRING_RESPONSE,
                             (PAIRING_STEP_ADDRESS << 16) |
                             (int32_t)response_len);
        return false;
    }
    diagnostics.auto_connect_flags |= PAIRING_ADDRESS_OK;

    uint8_t a1[16];
    uint8_t a2[16];
    int err = bt_rand(a1, sizeof(a1));
    if (!err) err = bt_rand(a2, sizeof(a2));
    if (err) {
        LOG_ERR("[BLE] pairing random generation failed: %d\r\n", err);
        diagnostic_set_error(PRO2_BLE_ERROR_PAIRING_RANDOM, err);
        return false;
    }

    uint8_t key_command[25] = {
        0x15,0x91,0x01,0x04,0x00,0x11,0x00,0x00,0x00
    };
    memcpy(key_command + 9, a1, sizeof(a1));
    response_len = sizeof(response);
    if (!pairing_command(PAIRING_STEP_KEY, key_command, sizeof(key_command),
                         0x04u, response, &response_len)) {
        return false;
    }
    if (response_len < 25u || response[8] != 0x01u) {
        diagnostic_set_error(PRO2_BLE_ERROR_PAIRING_RESPONSE,
                             (PAIRING_STEP_KEY << 16) |
                             (int32_t)response_len);
        return false;
    }

    uint8_t ltk[16];
    for (size_t i = 0; i < sizeof(ltk); ++i) {
        ltk[i] = a1[i] ^ response[9u + i];
    }

    uint8_t challenge_command[25] = {
        0x15,0x91,0x01,0x02,0x00,0x11,0x00,0x00,0x00
    };
    memcpy(challenge_command + 9, a2, sizeof(a2));
    response_len = sizeof(response);
    if (!pairing_command(PAIRING_STEP_CHALLENGE, challenge_command,
                         sizeof(challenge_command), 0x02u,
                         response, &response_len)) {
        return false;
    }
    if (response_len < 25u || response[8] != 0x01u) {
        diagnostic_set_error(PRO2_BLE_ERROR_PAIRING_RESPONSE,
                             (PAIRING_STEP_CHALLENGE << 16) |
                             (int32_t)response_len);
        return false;
    }

    uint8_t key_be[16];
    uint8_t challenge_be[16];
    uint8_t expected_b2[16];
    for (size_t i = 0; i < 16u; ++i) {
        key_be[i] = ltk[15u - i];
        challenge_be[i] = a2[15u - i];
    }
    err = bt_encrypt_be(key_be, challenge_be, expected_b2);
    if (err || memcmp(expected_b2, response + 9, sizeof(expected_b2))) {
        LOG_ERR("[BLE] pairing challenge verification failed: %d\r\n", err);
        diagnostic_set_error(PRO2_BLE_ERROR_PAIRING_CRYPTO,
                             err ? err : PAIRING_STEP_CHALLENGE);
        return false;
    }
    diagnostics.auto_connect_flags |= PAIRING_CHALLENGE_OK;

    response_len = sizeof(response);
    if (!pairing_command(PAIRING_STEP_FINALIZE, init_03, sizeof(init_03),
                         0x03u, response, &response_len)) {
        return false;
    }
    if (response_len < 9u || response[8] != 0x01u) {
        diagnostic_set_error(PRO2_BLE_ERROR_PAIRING_RESPONSE,
                             (PAIRING_STEP_FINALIZE << 16) |
                             (int32_t)response_len);
        return false;
    }

    if (!persist_pairing_ltk(ltk)) {
        LOG_ERR("[BLE] pairing committed but LTK persistence failed\r\n");
        diagnostic_set_error(PRO2_BLE_ERROR_PAIRING_RESPONSE,
                             (PAIRING_STEP_FINALIZE << 16) | 1);
        return false;
    }

    diagnostics.pairing_step = PAIRING_STEP_COMPLETE;
    diagnostics.auto_connect_flags |= PAIRING_COMMITTED;
    LOG_INF("[BLE] proprietary Pro2 pairing committed\r\n");
    return true;
}

static void setup_task(void *arg)
{
    (void)arg;
    setup_task_handle = xTaskGetCurrentTaskHandle();
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY); /* connected */
        if (!connection) continue;
        memset(&ch_fd2, 0, sizeof(ch_fd2));
        memset(&ch_ack, 0, sizeof(ch_ack));
        memset(&ch_command, 0, sizeof(ch_command));
        memset(&ch_rumble, 0, sizeof(ch_rumble));
        memset(&mtu_exchange_params, 0, sizeof(mtu_exchange_params));
        memset(&command_write_params, 0, sizeof(command_write_params));
        ack_ccc_completed = false;
        command_write_pending = false;
        memset(&discover_params, 0, sizeof(discover_params));
        range_target = NULL;
        descriptor_target = NULL;
        if (!establish_saved_encryption()) {
            if (connection) disconnect_after_error();
            continue;
        }
        if (!exchange_command_mtu()) {
            if (connection) disconnect_after_error();
            continue;
        }
        discover_params.func = discover_cb;
        discover_params.start_handle = 1;
        discover_params.end_handle = 0xffff;
        discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;
        diagnostic_set_stage(PRO2_BLE_STAGE_CHARACTERISTIC_DISCOVERY);
        int err = bt_gatt_discover(connection, &discover_params);
        if (err) {
            LOG_ERR("[BLE] GATT discovery start failed: %d\r\n", err);
            disconnect_with_error(PRO2_BLE_ERROR_DISCOVERY_START, err);
            continue;
        }
        if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SETUP_TIMEOUT_MS))) {
            LOG_ERR("[BLE] GATT discovery timeout\r\n");
            disconnect_with_error(PRO2_BLE_ERROR_DISCOVERY_TIMEOUT, 0);
            continue;
        }
        if (!ch_fd2.value || !ch_ack.value || !ch_command.value || !ch_rumble.value) {
            int32_t missing = (!ch_fd2.value ? 1 : 0) |
                              (!ch_ack.value ? 2 : 0) |
                              (!ch_command.value ? 4 : 0) |
                              (!ch_rumble.value ? 8 : 0);
            LOG_ERR("[BLE] required Pro2 characteristics missing\r\n");
            disconnect_with_error(PRO2_BLE_ERROR_CHARACTERISTIC_MISSING, missing);
            continue;
        }
        if (!discover_ccc(&ch_ack) || !discover_ccc(&ch_fd2)) {
            LOG_ERR("[BLE] required CCC descriptor missing or discovery failed\r\n");
            disconnect_after_error();
            continue;
        }
        diagnostic_set_stage(PRO2_BLE_STAGE_ACK_SUBSCRIBE);
        while (ulTaskNotifyTake(pdTRUE, 0)) {}
        ack_ccc_completed = false;
        err = subscribe(&ch_ack, &ack_subscribe, ack_notify);
        if (err) {
            LOG_ERR("[BLE] ACK subscription failed: %d\r\n", err);
            disconnect_with_error(PRO2_BLE_ERROR_ACK_SUBSCRIBE, err);
            continue;
        }
        TickType_t ccc_start = xTaskGetTickCount();
        TickType_t ccc_timeout = pdMS_TO_TICKS(ACK_SUBSCRIBE_TIMEOUT_MS);
        while (!ack_ccc_completed && connection) {
            TickType_t elapsed = xTaskGetTickCount() - ccc_start;
            if (elapsed >= ccc_timeout) break;
            ulTaskNotifyTake(pdTRUE, ccc_timeout - elapsed);
        }
        if (!connection) continue;
        if (!ack_ccc_completed) {
            LOG_ERR("[BLE] ACK subscription confirmation timeout\r\n");
            disconnect_with_error(PRO2_BLE_ERROR_ACK_SUBSCRIBE, -3);
            continue;
        }
        bool failed = false;
        for (size_t i = 0; i < sizeof(init_commands) / sizeof(init_commands[0]); ++i) {
            diagnostics.init_index = (uint8_t)i;
            diagnostic_set_stage(PRO2_BLE_STAGE_INITIALIZING);
            if (i == 3u) {
                /* 0x15 commands are an initial-pairing transaction, not a
                 * reconnect initialisation command. */
                if (connection_pairing_required &&
                    !run_proprietary_pairing()) {
                    failed = true;
                    break;
                }
                continue;
            }
            uint32_t ack_before = ack_notification_count;
            err = write_command(&init_commands[i]);
            if (err) {
                LOG_ERR("[BLE] init %u write failed: %d\r\n", (unsigned)i, err);
                diagnostic_set_error(PRO2_BLE_ERROR_INIT_WRITE, err);
                failed = true;
                break;
            }
            TickType_t ack_start = xTaskGetTickCount();
            TickType_t ack_timeout = pdMS_TO_TICKS(COMMAND_ACK_TIMEOUT_MS);
            while (ack_notification_count == ack_before && connection) {
                TickType_t elapsed = xTaskGetTickCount() - ack_start;
                if (elapsed >= ack_timeout) break;
                ulTaskNotifyTake(pdTRUE, ack_timeout - elapsed);
            }
            if (!connection) {
                failed = true;
                break;
            }
            if (ack_notification_count == ack_before) {
                LOG_ERR("[BLE] init %u ACK timeout\r\n", (unsigned)i);
                diagnostic_set_error(PRO2_BLE_ERROR_INIT_ACK_TIMEOUT, (int32_t)i);
                failed = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        if (failed || !connection) {
            if (connection) disconnect_after_error();
            continue;
        }
        pro2_parser_init(&parser);
        pro2_state_reset(&controller_state);
        report_count = 0;
        while (ulTaskNotifyTake(pdTRUE, 0)) {}
        awaiting_first_fd2 = true;
        diagnostic_set_stage(PRO2_BLE_STAGE_FD2_SUBSCRIBE);
        err = subscribe(&ch_fd2, &fd2_subscribe, fd2_notify);
        if (err) {
            awaiting_first_fd2 = false;
            LOG_ERR("[BLE] FD2 subscription failed: %d\r\n", err);
            disconnect_with_error(PRO2_BLE_ERROR_FD2_SUBSCRIBE, err);
            continue;
        }
        diagnostic_set_stage(PRO2_BLE_STAGE_WAITING_FD2);
        if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FIRST_FD2_TIMEOUT_MS))) {
            awaiting_first_fd2 = false;
            LOG_ERR("[BLE] first FD2 report timeout\r\n");
            disconnect_with_error(PRO2_BLE_ERROR_FD2_TIMEOUT, ch_fd2.value);
            continue;
        }
        awaiting_first_fd2 = false;
        if (!connection) continue;
        bt_addr_le_copy(&saved_peer, bt_conn_get_dst(connection));
        saved_peer_valid = true;
        ef_set_env_blob(PEER_KEY, &saved_peer, sizeof(saved_peer));
        ready = true;
        diagnostic_set_error(PRO2_BLE_ERROR_NONE, 0);
        diagnostic_set_stage(PRO2_BLE_STAGE_READY);
        board_status_set(BOARD_STATUS_READY);
        LOG_INF("[BLE] Pro2 ready; FD2 input active\r\n");
    }
}

static void connected_cb(struct bt_conn *conn, uint8_t err)
{
    if (conn->type != BT_CONN_TYPE_LE) return;
    connecting = false;
    bool was_auto_connect = auto_connect_enabled;
    if (was_auto_connect) {
        /* Whitelist auto-connect is a one-shot HCI procedure. The stack
         * clears it when a connection completes, successful or otherwise. */
        auto_connect_enabled = false;
        diagnostics.auto_connect_flags &= (uint8_t)~AUTO_CONNECT_ARMED;
        diagnostics.connect_attempts++;
    }
    if (err) {
        LOG_ERR("[BLE] connection failed: %u\r\n", err);
        diagnostic_set_error(PRO2_BLE_ERROR_CONNECT, err);
        schedule_scan_retry();
        return;
    }
    connection = conn;
    connection_pairing_required = pending_pairing_required;
    ready = false;
    diagnostics.connect_successes++;
    diagnostic_set_peer(bt_conn_get_dst(conn));
    diagnostic_set_stage(PRO2_BLE_STAGE_CONNECTED);
    board_status_set(BOARD_STATUS_CONNECTING);
    if (setup_task_handle) xTaskNotifyGive(setup_task_handle);
}

static void disconnected_cb(struct bt_conn *conn, uint8_t reason)
{
    if (conn != connection) return;
    LOG_WRN("[BLE] disconnected: %u\r\n", reason);
    diagnostics.disconnects++;
    diagnostics.last_disconnect_reason = reason;
    if (!local_disconnect) {
        diagnostic_set_error(PRO2_BLE_ERROR_DISCONNECTED, reason);
    }
    local_disconnect = false;
    connection = NULL;
    connection_pairing_required = false;
    ready = false;
    pro2_state_reset(&controller_state);
    usb_nintendo_submit_state(&controller_state,
                              (uint32_t)bflb_mtimer_get_time_us());
    connecting = false;
    awaiting_first_fd2 = false;
    memset(&ack_subscribe, 0, sizeof(ack_subscribe));
    memset(&fd2_subscribe, 0, sizeof(fd2_subscribe));
    /* Controller-level whitelist auto-connect is one-shot. Re-arm it after
     * the ACL has fully torn down, matching the ESP bridge's 800 ms retry. */
    schedule_scan_retry();
}

static void security_changed_cb(struct bt_conn *conn, bt_security_t level,
                                enum bt_security_err err)
{
    if (conn != connection) return;
    security_level = (uint8_t)level;
    security_error = (uint8_t)err;
    security_completed = true;
    if (setup_task_handle) xTaskNotifyGive(setup_task_handle);
}

static struct bt_conn_cb connection_callbacks = {
    .connected = connected_cb,
    .disconnected = disconnected_cb,
    .security_changed = security_changed_cb,
};

static bool usable_local_address(const bt_addr_t *address)
{
    bool all_zero = true;
    bool all_ff = true;
    for (size_t i = 0; i < sizeof(address->val); ++i) {
        if (address->val[i] != 0u) {
            all_zero = false;
        }
        if (address->val[i] != 0xffu) all_ff = false;
    }
    return !all_zero && !all_ff;
}

static void ensure_stable_local_address(void)
{
    bt_addr_t address = {0};
    size_t stored = ef_get_env_blob(LOCAL_ADDRESS_KEY, &address,
                                    sizeof(address), NULL);

    if (stored == sizeof(address) && usable_local_address(&address)) {
        diagnostics.local_address_flags |= LOCAL_ADDR_RESTORED;
    } else {
        bt_addr_le_t current;
        bt_get_local_public_address(&current);
        if (usable_local_address(&current.a)) {
            bt_addr_copy(&address, &current.a);
            diagnostics.local_address_flags |= LOCAL_ADDR_CAPTURED;
        } else {
            uint8_t chip_id[8] = {0};
            bflb_efuse_get_chipid(chip_id);
            memcpy(address.val, chip_id, sizeof(address.val));
            address.val[5] |= 0xc0u;
            diagnostics.local_address_flags |= LOCAL_ADDR_DERIVED;
        }
        if (ef_set_env_blob(LOCAL_ADDRESS_KEY, &address,
                            sizeof(address)) == 0) {
            diagnostics.local_address_flags |= LOCAL_ADDR_SAVED;
        }
    }

    int controller_err = bt_set_bd_addr(&address);
    if (!controller_err) {
        diagnostics.local_address_flags |= LOCAL_ADDR_CONTROLLER_OK;
    }
    int host_err = bt_set_local_public_address(address.val);
    if (!host_err) diagnostics.local_address_flags |= LOCAL_ADDR_HOST_OK;

    LOG_INF("[BLE] stable local address %02x:%02x:%02x:%02x:%02x:%02x flags=0x%02x (%d/%d)\r\n",
            address.val[5], address.val[4], address.val[3],
            address.val[2], address.val[1], address.val[0],
            diagnostics.local_address_flags, controller_err, host_err);

    bt_addr_le_t current;
    bt_get_local_public_address(&current);
    LOG_INF("[BLE] local address %02x:%02x:%02x:%02x:%02x:%02x\r\n",
            current.a.val[5], current.a.val[4], current.a.val[3],
            current.a.val[2], current.a.val[1], current.a.val[0]);
}

static void bt_ready(int err)
{
    if (err) {
        LOG_ERR("[BLE] stack init failed: %d\r\n", err);
        diagnostic_set_error(PRO2_BLE_ERROR_STACK_INIT, err);
        board_status_set(BOARD_STATUS_ERROR);
        return;
    }
    ensure_stable_local_address();
    restore_pairing_ltk();
    bt_conn_cb_register(&connection_callbacks);
    stack_ready = true;
    start_reconnect();
}

static void bluetooth_task(void *arg)
{
    (void)arg;
    diagnostic_set_stage(PRO2_BLE_STAGE_STACK_INIT);
    net_buf_init(&hid_tx_pool, 1, 64, NULL);
    net_buf_init(&hid_rx_pool, 1, 64, NULL);
    btble_controller_init(configMAX_PRIORITIES - 1);
    hci_driver_init();
    int err = bt_enable(bt_ready);
    if (err) {
        LOG_ERR("[BLE] bt_enable failed: %d\r\n", err);
        diagnostic_set_error(PRO2_BLE_ERROR_STACK_INIT, err);
        board_status_set(BOARD_STATUS_ERROR);
    }
    vTaskDelete(NULL);
}

void pro2_ble_start(void)
{
    memset(&diagnostics, 0, sizeof(diagnostics));
    memset(&advertisement_diagnostic, 0, sizeof(advertisement_diagnostic));
    stack_ready = false;
    auto_connect_enabled = false;
    pending_pairing_required = false;
    connection_pairing_required = false;
    ack_response_generation = 0;
    ack_response_len = 0;
    saved_ltk_valid = false;
    diagnostic_set_stage(PRO2_BLE_STAGE_BOOT);
    size_t stored = ef_get_env_blob(PEER_KEY, &saved_peer, sizeof(saved_peer), NULL);
    saved_peer_valid = stored == sizeof(saved_peer);
    if (saved_peer_valid) diagnostic_set_peer(&saved_peer);
    xTaskCreate(setup_task, "pro2_setup", 3072, NULL,
                configMAX_PRIORITIES - 2, NULL);
    xTaskCreate(bluetooth_task, "bluetooth", 2048, NULL,
                configMAX_PRIORITIES - 1, NULL);
}

bool pro2_ble_stack_ready(void)
{
    return stack_ready;
}

bool pro2_ble_connected(void)
{
    return ready && connection;
}

uint8_t pro2_ble_led_error_code(void)
{
    switch ((pro2_ble_error_t)diagnostics.last_error) {
    case PRO2_BLE_ERROR_NONE:
        return 0;
    case PRO2_BLE_ERROR_STACK_INIT:
    case PRO2_BLE_ERROR_SCAN_START:
    case PRO2_BLE_ERROR_SCAN_STOP:
        return 1;
    case PRO2_BLE_ERROR_CONNECT_START:
    case PRO2_BLE_ERROR_CONNECT:
        return 2;
    case PRO2_BLE_ERROR_DISCOVERY_START:
    case PRO2_BLE_ERROR_DISCOVERY_TIMEOUT:
        return 3;
    case PRO2_BLE_ERROR_CHARACTERISTIC_MISSING:
        return 4;
    case PRO2_BLE_ERROR_DESCRIPTOR_START:
    case PRO2_BLE_ERROR_DESCRIPTOR_TIMEOUT:
    case PRO2_BLE_ERROR_CCC_MISSING:
        return 5;
    case PRO2_BLE_ERROR_ACK_SUBSCRIBE:
    case PRO2_BLE_ERROR_INIT_WRITE:
    case PRO2_BLE_ERROR_INIT_ACK_TIMEOUT:
    case PRO2_BLE_ERROR_MTU_EXCHANGE:
    case PRO2_BLE_ERROR_PAIRING_RANDOM:
    case PRO2_BLE_ERROR_PAIRING_WRITE:
    case PRO2_BLE_ERROR_PAIRING_ACK_TIMEOUT:
    case PRO2_BLE_ERROR_PAIRING_RESPONSE:
    case PRO2_BLE_ERROR_PAIRING_CRYPTO:
    case PRO2_BLE_ERROR_SECURITY_START:
    case PRO2_BLE_ERROR_SECURITY_TIMEOUT:
    case PRO2_BLE_ERROR_SECURITY_CHANGE:
        return 6;
    case PRO2_BLE_ERROR_FD2_SUBSCRIBE:
    case PRO2_BLE_ERROR_FD2_TIMEOUT:
        return 7;
    case PRO2_BLE_ERROR_DISCONNECTED:
    default:
        return 8;
    }
}

uint8_t pro2_ble_led_error_detail(void)
{
    switch ((pro2_ble_error_t)diagnostics.last_error) {
    case PRO2_BLE_ERROR_ACK_SUBSCRIBE:
        return 1;
    case PRO2_BLE_ERROR_INIT_WRITE:
        return 2;
    case PRO2_BLE_ERROR_INIT_ACK_TIMEOUT:
        return 3;
    case PRO2_BLE_ERROR_MTU_EXCHANGE:
        return 4;
    case PRO2_BLE_ERROR_PAIRING_RANDOM:
    case PRO2_BLE_ERROR_PAIRING_WRITE:
    case PRO2_BLE_ERROR_PAIRING_ACK_TIMEOUT:
    case PRO2_BLE_ERROR_PAIRING_RESPONSE:
    case PRO2_BLE_ERROR_PAIRING_CRYPTO:
    case PRO2_BLE_ERROR_SECURITY_START:
    case PRO2_BLE_ERROR_SECURITY_TIMEOUT:
    case PRO2_BLE_ERROR_SECURITY_CHANGE:
        return 5;
    default:
        return 0;
    }
}

uint8_t pro2_ble_led_error_value(void)
{
    switch ((pro2_ble_error_t)diagnostics.last_error) {
    case PRO2_BLE_ERROR_INIT_WRITE:
    case PRO2_BLE_ERROR_INIT_ACK_TIMEOUT:
        return (uint8_t)(diagnostics.init_index + 1u);
    default:
        return 0;
    }
}

int pro2_ble_write_rumble(const uint8_t *data, size_t len)
{
    if (!ready || !connection || !ch_rumble.value || !data || len > UINT16_MAX) return -1;
    return bt_gatt_write_without_response(connection, ch_rumble.value,
                                          data, (uint16_t)len, false);
}

void pro2_ble_forget_peer(void)
{
    disable_auto_connect();
    if (saved_peer_valid) {
        struct bt_keys *keys = bt_keys_find_addr(BT_ID_DEFAULT, &saved_peer);
        if (keys) bt_keys_clear(keys);
    }
    saved_peer_valid = false;
    saved_ltk_valid = false;
    memset(saved_ltk, 0, sizeof(saved_ltk));
    diagnostics.auto_connect_flags = 0;
    diagnostics.peer_type = 0;
    for (size_t i = 0; i < sizeof(diagnostics.peer); ++i) {
        diagnostics.peer[i] = 0;
    }
    diagnostic_set_error(PRO2_BLE_ERROR_NONE, 0);
    ef_del_env(PEER_KEY);
    ef_del_env(PAIRING_RECORD_KEY);
    LOG_INF("[BLE] saved peer cleared\r\n");
    if (connection) {
        local_disconnect = true;
        bt_conn_disconnect(connection, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    } else {
        if (scanning) {
            bt_le_scan_stop();
            scanning = false;
        }
        connecting = false;
        start_scan();
    }
}

size_t pro2_ble_build_diagnostic_report(uint8_t *report, size_t report_max)
{
    if (!report || report_max < PRO2_BLE_DIAGNOSTIC_REPORT_SIZE) return 0;
    memset(report, 0, PRO2_BLE_DIAGNOSTIC_REPORT_SIZE);
    memcpy(report, "P2DG", 4);
    report[4] = DIAGNOSTIC_VERSION;
    report[5] = diagnostics.stage;
    report[6] = diagnostics.last_error;
    report[7] = (saved_peer_valid ? 1u : 0u) |
                (scanning ? 2u : 0u) |
                (connecting ? 4u : 0u) |
                (connection ? 8u : 0u) |
                (ready ? 16u : 0u) |
                (awaiting_first_fd2 ? 32u : 0u) |
                (diagnostics.last_candidate_directed ? 64u : 0u) |
                (diagnostics.directed_candidate_seen ? 128u : 0u);
    put_le32(report + 8, (uint32_t)diagnostics.last_code);
    put_le32(report + 12, diagnostics.scan_reports);
    put_le32(report + 16, diagnostics.candidates);
    put_le32(report + 20, diagnostics.connect_attempts);
    put_le32(report + 24, diagnostics.connect_successes);
    put_le32(report + 28, diagnostics.disconnects);
    put_le32(report + 32, diagnostics.fd2_reports);
    put_le16(report + 36, ch_fd2.value);
    put_le16(report + 38, ch_fd2.ccc);
    put_le16(report + 40, ch_ack.value);
    put_le16(report + 42, ch_ack.ccc);
    put_le16(report + 44, ch_command.value);
    put_le16(report + 46, ch_rumble.value);
    report[48] = diagnostics.peer_type;
    for (size_t i = 0; i < sizeof(diagnostics.peer); ++i) {
        report[49 + i] = diagnostics.peer[i];
    }
    report[55] = diagnostics.init_index;
    report[56] = diagnostics.last_disconnect_reason;
    report[57] = (uint8_t)diagnostics.ack_reports;
    report[58] = (uint8_t)(diagnostics.ack_reports >> 8);
    report[59] = (uint8_t)(diagnostics.ack_reports >> 16);
    report[60] = diagnostics.local_address_flags;
    put_le16(report + 61, diagnostics.att_mtu);
    report[63] = diagnostics.auto_connect_flags;
    return PRO2_BLE_DIAGNOSTIC_REPORT_SIZE;
}

size_t pro2_ble_build_advertisement_report(uint8_t *report, size_t report_max)
{
    if (!report || report_max < PRO2_BLE_DIAGNOSTIC_REPORT_SIZE) return 0;
    memset(report, 0, PRO2_BLE_DIAGNOSTIC_REPORT_SIZE);
    memcpy(report, "P2DA", 4);
    report[4] = 3u;
    report[5] = (advertisement_diagnostic.captured ? 1u : 0u) |
                (advertisement_diagnostic.saved_match ? 2u : 0u) |
                (advertisement_diagnostic.candidate_match ? 4u : 0u) |
                (advertisement_diagnostic.event_type == BT_LE_ADV_DIRECT_IND ? 8u : 0u) |
                ((advertisement_diagnostic.event_type == BT_LE_ADV_IND ||
                  advertisement_diagnostic.event_type == BT_LE_ADV_DIRECT_IND) ? 16u : 0u);
    report[6] = advertisement_diagnostic.event_type;
    report[7] = advertisement_diagnostic.peer_type;
    report[8] = (uint8_t)advertisement_diagnostic.rssi;
    report[9] = advertisement_diagnostic.payload_len;
    report[10] = advertisement_diagnostic.payload_copied;
    report[11] = advertisement_diagnostic.event_type_mask;
    put_le32(report + 12, advertisement_diagnostic.matching_reports);
    memcpy(report + 16, (const void *)advertisement_diagnostic.peer,
           sizeof(advertisement_diagnostic.peer));
    memcpy(report + 22, (const void *)advertisement_diagnostic.payload,
           sizeof(advertisement_diagnostic.payload));
    bt_addr_le_t local_identity = {0};
    if (!bt_get_local_public_address(&local_identity)) {
        report[54] |= 1u;
        report[55] = local_identity.type;
        memcpy(report + 56, local_identity.a.val, sizeof(local_identity.a.val));
    }
    const uint8_t *payload = (const uint8_t *)advertisement_diagnostic.payload;
    if (advertisement_diagnostic.payload_copied >= 23u &&
        payload[3] >= 0x13u && payload[4] == BT_DATA_MANUFACTURER_DATA &&
        payload[5] == 0x53u && payload[6] == 0x05u) {
        bool target_nonzero = false;
        for (size_t i = 17; i < 23; ++i) {
            if (payload[i]) target_nonzero = true;
        }
        if (target_nonzero) {
            report[54] |= 2u;
            if ((report[54] & 1u) &&
                !memcmp(payload + 17, local_identity.a.val,
                        sizeof(local_identity.a.val))) {
                report[54] |= 4u;
            }
        }
    }
    report[62] = diagnostics.pairing_step;
    report[63] = diagnostics.auto_connect_flags &
                 (PAIRING_REQUIRED | PAIRING_STARTED | PAIRING_ADDRESS_OK |
                  PAIRING_CHALLENGE_OK | PAIRING_COMMITTED);
    return PRO2_BLE_DIAGNOSTIC_REPORT_SIZE;
}
