// SPDX-License-Identifier: GPL-3.0-only
#include "usb_nintendo.h"

#include <string.h>

#include "board.h"
#include "bl616_glb.h"
#include "compiler/compiler_ld.h"
#include "FreeRTOS.h"
#include "task.h"
#include "usbd_core.h"
#include "usbd_hid.h"

#include "debug_log.h"
#include "pro2_ble.h"
#include "pro2_protocol.h"
#include "pro2_rumble.h"
#include "pro2_settings.h"
#include "vendor_protocol.h"

#define HID_EP_IN       0x81
/* BL616 maps both directions of one endpoint number to the same VDMA/FIFO.
 * Keep HID OUT off endpoint 1 so rumble cannot overwrite a pending input. */
#define HID_EP_OUT      0x03
#define VENDOR_EP_IN    0x82
#define VENDOR_EP_OUT   0x02
#define MS_VENDOR_CODE  0xcd
#define LEGACY_MS_VENDOR_CODE 0x20
#define MS_CORE_BYPASS_VENDOR_CODE 0xfe
#define MS_OS_20_DESCRIPTOR_SIZE 0xb2u
#define MS_OS_10_COMPAT_ID_SIZE 0x28u
#define MS_OS_10_PROPERTY_SIZE 0x8eu
#define CONFIG_SIZE     64
#define INPUT_PERIOD_MS 4u
#define VENDOR_IN_CHUNK_SIZE 64u

#ifdef CONFIG_USB_HS
#define VENDOR_EP_MPS            512
#define VENDOR_EP_MPS_BYTES      0x00,0x02
#define OTHER_VENDOR_MPS_BYTES   0x40,0x00
#define HID_POLL_INTERVAL        0x01
#define OTHER_HID_POLL_INTERVAL  0x01
#else
#define VENDOR_EP_MPS            64
#define VENDOR_EP_MPS_BYTES      0x40,0x00
#define OTHER_VENDOR_MPS_BYTES   0x00,0x02
#define HID_POLL_INTERVAL        0x01
#define OTHER_HID_POLL_INTERVAL  0x01
#endif

static const uint8_t device_descriptor[] = {
    0x12,0x01,0x01,0x02, 0x00,0x00,0x00,0x40,
    /* 0x061a identifies the corrected four-axis generic report. */
    0x7e,0x05, 0x69,0x20, 0x1a,0x06,
    0x01,0x02,0x03,0x01,
};

static const uint8_t config_descriptor[] = {
    0x09,0x02, CONFIG_SIZE,0x00, 0x02,0x01,0x04,0x80,0xfa,
    /* Interface 0: Switch 2 Pro HID. */
    0x09,0x04,0x00,0x00,0x02,0x03,0x00,0x00,0x05,
    0x09,0x21,0x01,0x01,0x00,0x01,0x22,0x63,0x00,
    0x07,0x05,HID_EP_IN, 0x03,0x40,0x00,HID_POLL_INTERVAL,
    0x07,0x05,HID_EP_OUT,0x03,0x40,0x00,HID_POLL_INTERVAL,
    /* Interface 1: Switch 2 vendor transport, bound to WinUSB. */
    0x09,0x04,0x01,0x00,0x02,0xff,0x00,0x00,0x07,
    0x07,0x05,VENDOR_EP_IN, 0x02,VENDOR_EP_MPS_BYTES,0x00,
    0x07,0x05,VENDOR_EP_OUT,0x02,VENDOR_EP_MPS_BYTES,0x00,
};

static const uint8_t other_speed_descriptor[] = {
    0x09,0x07, CONFIG_SIZE,0x00, 0x02,0x01,0x04,0x80,0xfa,
    0x09,0x04,0x00,0x00,0x02,0x03,0x00,0x00,0x05,
    0x09,0x21,0x01,0x01,0x00,0x01,0x22,0x63,0x00,
    0x07,0x05,HID_EP_IN, 0x03,0x40,0x00,OTHER_HID_POLL_INTERVAL,
    0x07,0x05,HID_EP_OUT,0x03,0x40,0x00,OTHER_HID_POLL_INTERVAL,
    0x09,0x04,0x01,0x00,0x02,0xff,0x00,0x00,0x07,
    0x07,0x05,VENDOR_EP_IN, 0x02,OTHER_VENDOR_MPS_BYTES,0x00,
    0x07,0x05,VENDOR_EP_OUT,0x02,OTHER_VENDOR_MPS_BYTES,0x00,
};

static const uint8_t qualifier_descriptor[] = {
    0x0a,0x06,0x00,0x02,0x00,0x00,0x00,0x40,0x01,0x00,
};

static const uint8_t hid_report_descriptor[] = {
    /* One Gamepad collection carries both Nintendo-native and web reports. */
    0x05,0x01,0x09,0x05,0xa1,0x01,
    /* Native 0x05: retained byte-for-byte for Steam's Nintendo path. */
    0x85,0x05,0x05,0xff,0x09,0x01,0x15,0x00,0x26,0xff,0x00,
    0x95,0x3f,0x75,0x08,0x81,0x02,
    /* Generic 0x09: browser-visible buttons and four 12-bit axes. */
    0x85,0x09,
    0x05,0x09,0x19,0x01,0x29,0x15,0x25,0x01,0x95,0x15,0x75,0x01,0x81,0x02,
    0x95,0x01,0x75,0x03,0x81,0x03,
    0x05,0x01,0x09,0x01,0xa1,0x00,0x09,0x30,0x09,0x31,0x09,0x32,0x09,0x33,
    0x26,0xff,0x0f,0x95,0x04,0x75,0x0c,0x81,0x02,0xc0,
    0x05,0xff,0x09,0x02,0x26,0xff,0x00,0x95,0x36,0x75,0x08,0x81,0x03,
    /* Nintendo vibration output and private diagnostic feature report. */
    0x85,0x02,0x09,0x01,0x95,0x3f,0x91,0x02,
    0x85,0x7f,0x09,0x03,0x95,0x3f,0xb1,0x02,
    0xc0,
};

#define SIZE_CHECK(name, condition) typedef char name[(condition) ? 1 : -1]
SIZE_CHECK(device_descriptor_size_must_be_18, sizeof(device_descriptor) == 18);
SIZE_CHECK(config_descriptor_size_must_be_64, sizeof(config_descriptor) == CONFIG_SIZE);
SIZE_CHECK(other_speed_descriptor_size_must_be_64,
           sizeof(other_speed_descriptor) == CONFIG_SIZE);
SIZE_CHECK(hid_report_descriptor_size_must_be_99,
           sizeof(hid_report_descriptor) == 99);

/* Microsoft OS 1.0 descriptors used by Windows' legacy WinUSB probe. */
static const uint8_t ms_os_10_string_descriptor[] = {
    0x12,0x03,
    'M',0,'S',0,'F',0,'T',0,'1',0,'0',0,'0',0,MS_VENDOR_CODE,0,
};

static const uint8_t ms_os_10_compat_id_descriptor[] = {
    0x28,0x00,0x00,0x00, 0x00,0x01, 0x04,0x00,
    0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x01, 'W','I','N','U','S','B',0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,
};

static const uint8_t ms_os_10_property_descriptor[] = {
    0x8e,0x00,0x00,0x00, 0x00,0x01, 0x05,0x00, 0x01,0x00,
    0x84,0x00,0x00,0x00, 0x01,0x00,0x00,0x00, 0x28,0x00,
    'D',0,'e',0,'v',0,'i',0,'c',0,'e',0,
    'I',0,'n',0,'t',0,'e',0,'r',0,'f',0,
    'a',0,'c',0,'e',0,'G',0,'U',0,'I',0,'D',0,0,0,
    0x4e,0x00,0x00,0x00,
    '{',0,'6',0,'F',0,'1',0,'3',0,'7',0,'2',0,'5',0,'E',0,
    '-',0,'E',0,'F',0,'0',0,'E',0,'-',0,'4',0,'F',0,'D',0,
    '3',0,'-',0,'A',0,'E',0,'5',0,'F',0,'-',0,'B',0,'2',0,
    'D',0,'E',0,'9',0,'8',0,'9',0,'E',0,'C',0,'8',0,'2',0,
    '5',0,'}',0,0,0,
};

static const uint8_t *ms_os_10_properties[] = {
    NULL,
    ms_os_10_property_descriptor,
};

/*
 * Registering this structure makes CherryUSB return string index 0xee.  Its
 * private vendor code deliberately differs from the advertised code so the
 * application handler below can serve and count both OS 1.0 and 2.0 probes.
 */
static const struct usb_msosv1_descriptor ms_os_10 = {
    .string = ms_os_10_string_descriptor,
    .vendor_code = MS_CORE_BYPASS_VENDOR_CODE,
    .compat_id = ms_os_10_compat_id_descriptor,
    .comp_id_property = ms_os_10_properties,
};

/*
 * Microsoft OS 2.0 descriptor set copied from the ESP32 reference firmware.
 * The registry property gives interface 1 a stable device-interface GUID;
 * Windows needs the complete set to install WinUSB automatically and expose
 * the Switch 2 initialization transport to Steam.
 */
static const uint8_t ms_os_20_descriptor[] = {
    /* Microsoft OS 2.0 descriptor set header. */
    0x0a,0x00,0x00,0x00, 0x00,0x00,0x03,0x06, 0xb2,0x00,
    /* Configuration subset header. */
    0x08,0x00,0x01,0x00, 0x00,0x00,0xa8,0x00,
    /* Function subset header for interface 1. */
    0x08,0x00,0x02,0x00, 0x01,0x00,0xa0,0x00,
    /* Compatible ID: WINUSB. */
    0x14,0x00,0x03,0x00, 'W','I','N','U','S','B',0,0,
    0,0,0,0,0,0,0,0,
    /* REG_MULTI_SZ DeviceInterfaceGUIDs property. */
    0x84,0x00,0x04,0x00, 0x07,0x00,0x2a,0x00,
    'D',0,'e',0,'v',0,'i',0,'c',0,'e',0,
    'I',0,'n',0,'t',0,'e',0,'r',0,'f',0,
    'a',0,'c',0,'e',0,'G',0,'U',0,'I',0,'D',0,'s',0,0,0,
    0x50,0x00,
    '{',0,'6',0,'F',0,'1',0,'3',0,'7',0,'2',0,'5',0,'E',0,
    '-',0,'E',0,'F',0,'0',0,'E',0,'-',0,'4',0,'F',0,'D',0,
    '3',0,'-',0,'A',0,'E',0,'5',0,'F',0,'-',0,'B',0,'2',0,
    'D',0,'E',0,'9',0,'8',0,'9',0,'E',0,'C',0,'8',0,'2',0,
    '5',0,'}',0,0,0,0,0,
};

static const uint8_t bos_descriptor_bytes[] = {
    0x05,0x0f,0x21,0x00,0x01,
    0x1c,0x10,0x05,0x00,
    0xd8,0xdd,0x60,0xdf,0x45,0x89,0x4c,0xc7,
    0x9c,0xd2,0x65,0x9d,0x9e,0x64,0x8a,0x9f,
    0x00,0x00,0x03,0x06, 0xb2,0x00,MS_VENDOR_CODE,0x00,
};
SIZE_CHECK(ms_os_20_descriptor_size_must_be_178,
           sizeof(ms_os_20_descriptor) == MS_OS_20_DESCRIPTOR_SIZE);
SIZE_CHECK(ms_os_10_string_descriptor_size_must_be_18,
           sizeof(ms_os_10_string_descriptor) == 18);
SIZE_CHECK(ms_os_10_compat_id_size_must_be_40,
           sizeof(ms_os_10_compat_id_descriptor) == MS_OS_10_COMPAT_ID_SIZE);
SIZE_CHECK(ms_os_10_property_size_must_be_142,
           sizeof(ms_os_10_property_descriptor) == MS_OS_10_PROPERTY_SIZE);
SIZE_CHECK(bos_descriptor_size_must_be_33, sizeof(bos_descriptor_bytes) == 33);

static const struct usb_bos_descriptor bos_descriptor = {
    .string = bos_descriptor_bytes,
    .string_len = sizeof(bos_descriptor_bytes),
};

static const char *strings[] = {
    (const char[]){0x09,0x04},
    "Nintendo Co., Ltd.",
    "Nintendo Switch Pro Controller",
    /* New device instance: Windows otherwise retains HID preparsed data for
     * the older report-0x09 layout under the same composite-device serial. */
    "P2DG-BL616-0002",
    "Nintendo Switch Pro Controller",
    "HID Interface",
    "",
    "Nintendo Switch 2 bulk",
};

static const uint8_t *device_cb(uint8_t speed) { (void)speed; return device_descriptor; }
static const uint8_t *config_cb(uint8_t speed) { (void)speed; return config_descriptor; }
static const uint8_t *qualifier_cb(uint8_t speed) { (void)speed; return qualifier_descriptor; }
static const uint8_t *other_speed_cb(uint8_t speed) { (void)speed; return other_speed_descriptor; }
static const char *string_cb(uint8_t speed, uint8_t index)
{
    (void)speed;
    return index < sizeof(strings) / sizeof(strings[0]) ? strings[index] : NULL;
}

static const struct usb_descriptor descriptors = {
    .device_descriptor_callback = device_cb,
    .config_descriptor_callback = config_cb,
    .device_quality_descriptor_callback = qualifier_cb,
    .other_speed_descriptor_callback = other_speed_cb,
    .string_descriptor_callback = string_cb,
    .msosv1_descriptor = &ms_os_10,
    .bos_descriptor = &bos_descriptor,
};

static struct usbd_interface hid_interface;
static struct usbd_interface vendor_interface;
static struct usbd_endpoint hid_in_endpoint;
static struct usbd_endpoint hid_out_endpoint;
static struct usbd_endpoint vendor_in_endpoint;
static struct usbd_endpoint vendor_out_endpoint;

static USB_NOCACHE_RAM_SECTION uint8_t hid_in[64];
static USB_NOCACHE_RAM_SECTION uint8_t hid_out[64];
/* BL616 Full-Speed VDMA needs protocol replies submitted one max-packet at a
 * time.  An 80-byte flash reply is therefore scheduled as 64 + 16. */
static USB_NOCACHE_RAM_SECTION uint8_t vendor_in[VENDOR_IN_CHUNK_SIZE];
static USB_NOCACHE_RAM_SECTION uint8_t vendor_out[VENDOR_EP_MPS];
static uint8_t pending_vendor_out[VENDOR_EP_MPS];
static size_t pending_vendor_out_len;
static uint8_t latest_input_05[64];
static uint8_t latest_input_09[64];
static uint8_t feature_report[64];
typedef enum {
    FEATURE_DIAGNOSTIC_BLE,
    FEATURE_DIAGNOSTIC_USB,
    FEATURE_DIAGNOSTIC_VENDOR,
    FEATURE_DIAGNOSTIC_RUMBLE,
    FEATURE_DIAGNOSTIC_ADVERTISEMENT,
    FEATURE_CONFIG,
} feature_diagnostic_kind_t;
static feature_diagnostic_kind_t feature_diagnostic_kind;
typedef enum {
    FEATURE_ACTION_NONE,
    FEATURE_ACTION_SAVE,
    FEATURE_ACTION_RESET,
    FEATURE_ACTION_FORGET_PEER,
} feature_action_t;
static volatile feature_action_t feature_pending_action;
static volatile pro2_web_command_t feature_last_command;
static volatile pro2_web_status_t feature_last_status;
static volatile uint8_t feature_action_sequence;
static uint8_t vendor_reply[PRO2_VENDOR_REPLY_MAX];
static uint8_t last_vendor_command[20];
static uint8_t last_vendor_reply[20];
static uint16_t last_vendor_command_len;
static uint16_t last_vendor_reply_len;
static bool last_vendor_command_valid;
static bool last_vendor_reply_valid;
static uint32_t vendor_commands_dropped_busy;
static volatile bool configured;
static volatile uint8_t usb_led_stage;
static volatile bool hid_busy;
static volatile bool input_pending;
static volatile bool generic_pending;
static volatile bool steam_native_mode;
static volatile bool vendor_busy;
static volatile bool vendor_out_armed;
static size_t vendor_reply_len;
static size_t vendor_reply_offset;
static uint8_t input_sequence;
static uint8_t generic_sequence;

typedef struct {
    volatile int32_t last_start_result;
    volatile uint32_t input_updates;
    volatile uint32_t periodic_ticks;
    volatile uint32_t hid_starts;
    volatile uint32_t hid_completions;
    volatile uint32_t hid_failures;
    volatile uint32_t hid_last_complete_bytes;
    volatile uint32_t hid_out_reports;
    volatile uint32_t vendor_out_reports;
    volatile uint32_t vendor_in_starts;
    volatile uint32_t vendor_in_completions;
    volatile uint32_t ms_os_10_compat_requests;
    volatile uint32_t ms_os_10_property_requests;
    volatile uint32_t ms_os_20_descriptor_requests;
} usb_diagnostics_t;

static usb_diagnostics_t usb_diagnostics;

static size_t build_vendor_diagnostic_report(uint8_t *report,
                                             size_t report_max);
static void process_vendor_command(const uint8_t *command, size_t command_len);
static void try_send_input(void);

static void feature_complete(pro2_web_command_t command,
                             pro2_web_status_t status)
{
    feature_last_command = command;
    feature_last_status = status;
    feature_action_sequence++;
}

static void feature_queue(pro2_web_command_t command,
                          feature_action_t action)
{
    if (feature_pending_action != FEATURE_ACTION_NONE) {
        return;
    }
    feature_last_command = command;
    feature_last_status = PRO2_WEB_STATUS_PENDING;
    feature_pending_action = action;
}

static void process_feature_action(void)
{
    feature_action_t action = feature_pending_action;
    if (action == FEATURE_ACTION_NONE) return;

    switch (action) {
    case FEATURE_ACTION_SAVE:
        feature_complete(PRO2_WEB_COMMAND_SAVE,
                         pro2_settings_save() ? PRO2_WEB_STATUS_OK
                                              : PRO2_WEB_STATUS_FLASH_ERROR);
        break;
    case FEATURE_ACTION_RESET:
        pro2_settings_reset_defaults();
        feature_complete(PRO2_WEB_COMMAND_RESET,
                         pro2_settings_save() ? PRO2_WEB_STATUS_OK
                                              : PRO2_WEB_STATUS_FLASH_ERROR);
        break;
    case FEATURE_ACTION_FORGET_PEER:
        pro2_ble_forget_peer();
        feature_complete(PRO2_WEB_COMMAND_FORGET_PEER, PRO2_WEB_STATUS_OK);
        break;
    default:
        break;
    }
    feature_pending_action = FEATURE_ACTION_NONE;
}

static void arm_vendor_out(void)
{
    if (!configured || vendor_busy || vendor_out_armed) return;
    if (usbd_ep_start_read(0, VENDOR_EP_OUT, vendor_out,
                           sizeof(vendor_out)) >= 0) {
        vendor_out_armed = true;
    }
}

static int ms_os_vendor_handler(uint8_t busid, struct usb_setup_packet *setup,
                                uint8_t **data, uint32_t *len)
{
    (void)busid;
    /* Windows caches the Microsoft OS descriptor vendor code per USB identity.
     * Older bridge builds advertised 0x20 while the working reference profile
     * advertises 0xcd.  Serve both so an existing Windows devnode can migrate
     * to the reference profile and install WinUSB without a manual Zadig pass. */
    if (!setup ||
        (setup->bRequest != MS_VENDOR_CODE &&
         setup->bRequest != LEGACY_MS_VENDOR_CODE)) {
        return -1;
    }

    switch (setup->wIndex) {
    case 0x0004:
        usb_diagnostics.ms_os_10_compat_requests++;
        *data = (uint8_t *)ms_os_10_compat_id_descriptor;
        *len = sizeof(ms_os_10_compat_id_descriptor);
        return 0;
    case 0x0005:
        usb_diagnostics.ms_os_10_property_requests++;
        *data = (uint8_t *)ms_os_10_property_descriptor;
        *len = sizeof(ms_os_10_property_descriptor);
        return 0;
    case 0x0007:
        usb_diagnostics.ms_os_20_descriptor_requests++;
        *data = (uint8_t *)ms_os_20_descriptor;
        *len = sizeof(ms_os_20_descriptor);
        return 0;
    default:
        return -1;
    }
}

static void put_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static void stamp_input_report(uint8_t report[64])
{
    if (report[0] == PRO2_USB_GENERIC_INPUT_REPORT_ID) {
        /* Byte 63 is constant padding in report 0x09, so the browser ignores
         * this diagnostic sequence counter instead of treating it as input. */
        report[63] = generic_sequence++;
    } else {
        report[1] = input_sequence++;
    }
}

static void try_send_input(void)
{
    bool send_generic;
    if (!configured || hid_busy) return;
    send_generic = !steam_native_mode;
    if (send_generic) {
        input_pending = false;
        if (!generic_pending) return;
        memcpy(hid_in, latest_input_09, sizeof(hid_in));
        generic_pending = false;
    } else {
        generic_pending = false;
        if (!input_pending) return;
        memcpy(hid_in, latest_input_05, sizeof(hid_in));
        input_pending = false;
    }
    stamp_input_report(hid_in);
    hid_busy = true;
    int ret = usbd_ep_start_write(0, HID_EP_IN, hid_in, sizeof(hid_in));
    usb_diagnostics.last_start_result = ret;
    if (ret < 0) {
        usb_diagnostics.hid_failures++;
        hid_busy = false;
        if (send_generic) {
            generic_pending = true;
        } else {
            input_pending = true;
        }
    } else {
        usb_diagnostics.hid_starts++;
    }
}

static void hid_in_cb(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid; (void)ep;
    usb_diagnostics.hid_completions++;
    usb_diagnostics.hid_last_complete_bytes = nbytes;
    hid_busy = false;
    try_send_input();
}

static void hid_out_cb(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid; (void)ep;
    if (nbytes) {
        usb_diagnostics.hid_out_reports++;
        pro2_rumble_submit_usb(hid_out, nbytes);
    }
    usbd_ep_start_read(0, HID_EP_OUT, hid_out, sizeof(hid_out));
}

static void try_send_vendor(void)
{
    if (vendor_busy || vendor_reply_offset >= vendor_reply_len || !configured) return;
    size_t n = vendor_reply_len - vendor_reply_offset;
    if (n > sizeof(vendor_in)) n = sizeof(vendor_in);
    memcpy(vendor_in, vendor_reply + vendor_reply_offset, n);
    vendor_reply_offset += n;
    vendor_busy = true;
    if (usbd_ep_start_write(0, VENDOR_EP_IN, vendor_in, n) < 0) {
        vendor_reply_offset -= n;
        vendor_busy = false;
    } else {
        usb_diagnostics.vendor_in_starts++;
    }
}

static void vendor_in_cb(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid; (void)ep; (void)nbytes;
    usb_diagnostics.vendor_in_completions++;
    vendor_busy = false;
    try_send_vendor();
    if (!vendor_busy && pending_vendor_out_len) {
        size_t pending_len = pending_vendor_out_len;
        pending_vendor_out_len = 0;
        process_vendor_command(pending_vendor_out, pending_len);
    }
    arm_vendor_out();
}

static void process_vendor_command(const uint8_t *command, size_t command_len)
{
    bool diagnostic = vendor_protocol_is_diagnostic_query(command, command_len) ||
                      (command_len >= 4 && !memcmp(command, "P2DU", 4)) ||
                      (command_len >= 4 && !memcmp(command, "P2DV", 4)) ||
                      (command_len >= 4 && !memcmp(command, "P2DR", 4)) ||
                      (command_len >= 4 && !memcmp(command, "P2DA", 4));
    /* Steam consumes the native Nintendo report by fixed byte offsets and
     * misreads the browser-oriented 0x09 report.  Its first real MI_01 command
     * is an unambiguous host-mode selection: stop 0x09 and keep only 0x05.
     * Diagnostic queries remain side-effect free. */
    if (!diagnostic && command_len && !steam_native_mode) {
        steam_native_mode = true;
        generic_pending = false;
        input_pending = true;
        try_send_input();
    }
    /* Once the last chunk has been scheduled, receipt of the next protocol
     * command proves that the host consumed the preceding reply even if a
     * pipe close or tight host loop suppressed/delayed our IN callback. */
    if (vendor_busy && vendor_reply_offset >= vendor_reply_len) {
        vendor_busy = false;
    }
    /* A host can close MI_01 while an IN transfer is still pending.  Let a
     * later diagnostic query replace that abandoned transfer so the saved
     * Steam trace remains readable without a physical replug. */
    if (diagnostic && vendor_busy) {
        vendor_busy = false;
        vendor_reply_len = 0;
        vendor_reply_offset = 0;
    }
    if (vendor_busy) {
        if (!pending_vendor_out_len) {
            pending_vendor_out_len = command_len;
            if (pending_vendor_out_len > sizeof(pending_vendor_out)) {
                pending_vendor_out_len = sizeof(pending_vendor_out);
            }
            memcpy(pending_vendor_out, command, pending_vendor_out_len);
        } else {
            vendor_commands_dropped_busy++;
        }
        return;
    }
    if (!diagnostic) {
        last_vendor_command_len = command_len > UINT16_MAX ? UINT16_MAX : command_len;
        memset(last_vendor_command, 0, sizeof(last_vendor_command));
        size_t command_copy = command_len;
        if (command_copy > sizeof(last_vendor_command)) {
            command_copy = sizeof(last_vendor_command);
        }
        memcpy(last_vendor_command, command, command_copy);
        last_vendor_command_valid = command_len != 0;
        last_vendor_reply_len = 0;
        last_vendor_reply_valid = false;
        memset(last_vendor_reply, 0, sizeof(last_vendor_reply));
    }
    if (vendor_protocol_is_diagnostic_query(command, command_len)) {
        vendor_reply_len = pro2_ble_build_diagnostic_report(
            vendor_reply, sizeof(vendor_reply));
    } else if (command_len >= 4 && !memcmp(command, "P2DU", 4)) {
        vendor_reply_len = usb_nintendo_build_diagnostic_report(
            vendor_reply, sizeof(vendor_reply));
    } else if (command_len >= 4 && !memcmp(command, "P2DV", 4)) {
        vendor_reply_len = build_vendor_diagnostic_report(
            vendor_reply, sizeof(vendor_reply));
    } else if (command_len >= 4 && !memcmp(command, "P2DR", 4)) {
        vendor_reply_len = pro2_rumble_build_diagnostic_report(
            vendor_reply, sizeof(vendor_reply));
    } else if (command_len >= 4 && !memcmp(command, "P2DA", 4)) {
        vendor_reply_len = pro2_ble_build_advertisement_report(
            vendor_reply, sizeof(vendor_reply));
    } else {
        vendor_reply_len = vendor_protocol_build_reply(command, command_len,
                                                       vendor_reply,
                                                       sizeof(vendor_reply));
        last_vendor_reply_len = vendor_reply_len;
        size_t reply_copy = vendor_reply_len;
        if (reply_copy > sizeof(last_vendor_reply)) {
            reply_copy = sizeof(last_vendor_reply);
        }
        memcpy(last_vendor_reply, vendor_reply, reply_copy);
        last_vendor_reply_valid = vendor_reply_len != 0;
    }
    vendor_reply_offset = 0;
    try_send_vendor();
}

static void vendor_out_cb(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid; (void)ep;
    vendor_out_armed = false;
    if (nbytes) usb_diagnostics.vendor_out_reports++;
    process_vendor_command(vendor_out, nbytes);
    /* EP 0x02 OUT and 0x82 IN share BL616's endpoint-2 VDMA/FIFO.  Keep them
     * serialized: arm the next OUT only after every IN chunk has completed. */
    arm_vendor_out();
}

static void usb_event(uint8_t busid, uint8_t event)
{
    (void)busid;
    switch (event) {
    case USBD_EVENT_INIT:
        if (usb_led_stage < 1u) usb_led_stage = 1u;
        break;
    case USBD_EVENT_CONNECTED:
        if (usb_led_stage < 2u) usb_led_stage = 2u;
        break;
    case USBD_EVENT_CONFIGURED:
        usb_led_stage = 4u;
        configured = true;
        hid_busy = false;
        vendor_busy = false;
        vendor_out_armed = false;
        vendor_reply_len = 0;
        vendor_reply_offset = 0;
        pending_vendor_out_len = 0;
        input_pending = false;
        generic_pending = true;
        steam_native_mode = false;
        usbd_ep_start_read(0, HID_EP_OUT, hid_out, sizeof(hid_out));
        arm_vendor_out();
        LOG_INF("[USB] Nintendo device configured\r\n");
        break;
    case USBD_EVENT_RESET:
        if (usb_led_stage < 3u) usb_led_stage = 3u;
        configured = false;
        hid_busy = false;
        steam_native_mode = false;
        vendor_busy = false;
        vendor_out_armed = false;
        break;
    case USBD_EVENT_DISCONNECTED:
        configured = false;
        hid_busy = false;
        steam_native_mode = false;
        vendor_busy = false;
        vendor_out_armed = false;
        break;
    case USBD_EVENT_SUSPEND:
        configured = false;
        hid_busy = false;
        vendor_busy = false;
        vendor_out_armed = false;
        break;
    case USBD_EVENT_RESUME:
        configured = true;
        arm_vendor_out();
        break;
    default:
        break;
    }
}

void usbd_hid_get_report(uint8_t busid, uint8_t intf, uint8_t report_id,
                         uint8_t report_type, uint8_t **data, uint32_t *len)
{
    (void)busid; (void)intf; (void)report_type;
    if (report_id == PRO2_USB_NATIVE_INPUT_REPORT_ID) {
        *data = latest_input_05;
        *len = sizeof(latest_input_05);
    } else if (report_id == PRO2_USB_GENERIC_INPUT_REPORT_ID) {
        *data = latest_input_09;
        *len = sizeof(latest_input_09);
    } else if (report_id == 0x7f) {
        uint8_t diagnostic[PRO2_USB_DIAGNOSTIC_REPORT_SIZE];
        if (feature_diagnostic_kind == FEATURE_DIAGNOSTIC_USB) {
            usb_nintendo_build_diagnostic_report(diagnostic,
                                                  sizeof(diagnostic));
        } else if (feature_diagnostic_kind == FEATURE_DIAGNOSTIC_VENDOR) {
            build_vendor_diagnostic_report(diagnostic, sizeof(diagnostic));
        } else if (feature_diagnostic_kind == FEATURE_DIAGNOSTIC_RUMBLE) {
            pro2_rumble_build_diagnostic_report(diagnostic,
                                                 sizeof(diagnostic));
        } else if (feature_diagnostic_kind == FEATURE_DIAGNOSTIC_ADVERTISEMENT) {
            pro2_ble_build_advertisement_report(diagnostic,
                                                 sizeof(diagnostic));
        } else if (feature_diagnostic_kind == FEATURE_CONFIG) {
            pro2_settings_build_report(diagnostic, sizeof(diagnostic),
                                       feature_last_command,
                                       feature_last_status,
                                       feature_action_sequence);
        } else {
            pro2_ble_build_diagnostic_report(diagnostic, sizeof(diagnostic));
        }
        feature_report[0] = 0x7f;
        memcpy(feature_report + 1, diagnostic, sizeof(feature_report) - 1);
        *data = feature_report;
        *len = sizeof(feature_report);
    } else {
        *len = 0;
    }
}

void usbd_hid_set_report(uint8_t busid, uint8_t intf, uint8_t report_id,
                         uint8_t report_type, uint8_t *report,
                         uint32_t report_len)
{
    (void)busid; (void)intf; (void)report_type;
    if (!report) return;
    if (report_id == 0x7f) {
        const uint8_t *selector = report;
        size_t selector_len = report_len;
        if (selector_len && selector[0] == report_id) {
            selector++;
            selector_len--;
        }
        if (selector_len >= 4 && !memcmp(selector, "P2DU", 4)) {
            feature_diagnostic_kind = FEATURE_DIAGNOSTIC_USB;
        } else if (selector_len >= 4 && !memcmp(selector, "P2DV", 4)) {
            feature_diagnostic_kind = FEATURE_DIAGNOSTIC_VENDOR;
        } else if (selector_len >= 4 && !memcmp(selector, "P2DR", 4)) {
            feature_diagnostic_kind = FEATURE_DIAGNOSTIC_RUMBLE;
        } else if (selector_len >= 4 && !memcmp(selector, "P2DA", 4)) {
            feature_diagnostic_kind = FEATURE_DIAGNOSTIC_ADVERTISEMENT;
        } else if (selector_len >= 4 && !memcmp(selector, "P2DG", 4)) {
            feature_diagnostic_kind = FEATURE_DIAGNOSTIC_BLE;
        } else if (selector_len >= 4 && !memcmp(selector, "P2CF", 4)) {
            feature_diagnostic_kind = FEATURE_CONFIG;
        } else if (selector_len >= 4 && !memcmp(selector, "P2CA", 4)) {
            feature_diagnostic_kind = FEATURE_CONFIG;
            feature_complete(PRO2_WEB_COMMAND_APPLY,
                             pro2_settings_apply_wire(selector, selector_len)
                                 ? PRO2_WEB_STATUS_OK
                                 : PRO2_WEB_STATUS_INVALID);
        } else if (selector_len >= 4 && !memcmp(selector, "P2CS", 4)) {
            feature_diagnostic_kind = FEATURE_CONFIG;
            feature_queue(PRO2_WEB_COMMAND_SAVE, FEATURE_ACTION_SAVE);
        } else if (selector_len >= 4 && !memcmp(selector, "P2CR", 4)) {
            feature_diagnostic_kind = FEATURE_CONFIG;
            feature_queue(PRO2_WEB_COMMAND_RESET, FEATURE_ACTION_RESET);
        } else if (selector_len >= 9 && !memcmp(selector, "P2PC", 4)) {
            feature_diagnostic_kind = FEATURE_CONFIG;
            if (!memcmp(selector + 4, "CLEAR", 5)) {
                feature_queue(PRO2_WEB_COMMAND_FORGET_PEER,
                              FEATURE_ACTION_FORGET_PEER);
            } else {
                feature_complete(PRO2_WEB_COMMAND_FORGET_PEER,
                                 PRO2_WEB_STATUS_INVALID);
            }
        }
        return;
    }
    if (report_id != PRO2_USB_OUTPUT_REPORT_ID) return;
    if (report_len && report[0] == report_id) {
        pro2_rumble_submit_usb(report, report_len);
    } else if (report_len <= 63) {
        uint8_t complete[64] = {0};
        complete[0] = report_id;
        memcpy(complete + 1, report, report_len);
        pro2_rumble_submit_usb(complete, report_len + 1);
    }
}

static void usb_input_task(void *arg)
{
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(INPUT_PERIOD_MS);

    for (;;) {
        process_feature_action();
        usb_diagnostics.periodic_ticks++;
        if (steam_native_mode) {
            input_pending = true;
            generic_pending = false;
        } else {
            input_pending = false;
            generic_pending = true;
        }
        try_send_input();
        vTaskDelayUntil(&wake, period ? period : 1);
    }
}

void usb_nintendo_init(void)
{
    pro2_state_t neutral;
    pro2_state_reset(&neutral);
    pro2_make_usb_report_05(&neutral, 0, 0, latest_input_05);
    pro2_make_usb_report_09(&neutral, 0, 0, latest_input_09);
    configured = false;
    usb_led_stage = 0;
    hid_busy = false;
    input_pending = false;
    generic_pending = true;
    steam_native_mode = false;
    vendor_busy = false;
    vendor_out_armed = false;
    pending_vendor_out_len = 0;
    input_sequence = 0;
    generic_sequence = 0;
    feature_diagnostic_kind = FEATURE_DIAGNOSTIC_BLE;
    feature_pending_action = FEATURE_ACTION_NONE;
    feature_last_command = PRO2_WEB_COMMAND_NONE;
    feature_last_status = PRO2_WEB_STATUS_IDLE;
    feature_action_sequence = 0;
    last_vendor_command_len = 0;
    last_vendor_reply_len = 0;
    last_vendor_command_valid = false;
    last_vendor_reply_valid = false;
    vendor_commands_dropped_busy = 0;
    memset(last_vendor_command, 0, sizeof(last_vendor_command));
    memset(last_vendor_reply, 0, sizeof(last_vendor_reply));
    memset(&usb_diagnostics, 0, sizeof(usb_diagnostics));

    usbd_desc_register(0, &descriptors);
    usbd_add_interface(0, usbd_hid_init_intf(0, &hid_interface,
                                             hid_report_descriptor,
                                             sizeof(hid_report_descriptor)));
    vendor_interface.vendor_handler = ms_os_vendor_handler;
    usbd_add_interface(0, &vendor_interface);

    hid_in_endpoint.ep_addr = HID_EP_IN;
    hid_in_endpoint.ep_cb = hid_in_cb;
    hid_out_endpoint.ep_addr = HID_EP_OUT;
    hid_out_endpoint.ep_cb = hid_out_cb;
    vendor_in_endpoint.ep_addr = VENDOR_EP_IN;
    vendor_in_endpoint.ep_cb = vendor_in_cb;
    vendor_out_endpoint.ep_addr = VENDOR_EP_OUT;
    vendor_out_endpoint.ep_cb = vendor_out_cb;
    usbd_add_endpoint(0, &hid_in_endpoint);
    usbd_add_endpoint(0, &hid_out_endpoint);
    usbd_add_endpoint(0, &vendor_in_endpoint);
    usbd_add_endpoint(0, &vendor_out_endpoint);

    usbd_initialize(0, USB_BASE, usb_event);
    /* BL616 CherryUSB port leaves the global interrupt gate clear. */
    volatile uint32_t *device_control = (volatile uint32_t *)(USB_BASE + 0x100);
    *device_control |= (1u << 2);

    xTaskCreate(usb_input_task, "usb_input", 512, NULL,
                configMAX_PRIORITIES - 3, NULL);
}

bool usb_nintendo_ready(void)
{
    return configured;
}

uint8_t usb_nintendo_led_stage(void)
{
    return usb_led_stage;
}

void usb_nintendo_submit_state(const pro2_state_t *state,
                               uint32_t timestamp_us)
{
    if (!state) return;
    pro2_make_usb_report_05(state, 0, timestamp_us, latest_input_05);
    pro2_make_usb_report_09(state, 0, timestamp_us, latest_input_09);
    usb_diagnostics.input_updates++;
    if (steam_native_mode) {
        input_pending = true;
        generic_pending = false;
    } else {
        input_pending = false;
        generic_pending = true;
    }
}

size_t usb_nintendo_build_diagnostic_report(uint8_t *report, size_t report_max)
{
    if (!report || report_max < PRO2_USB_DIAGNOSTIC_REPORT_SIZE) return 0;
    memset(report, 0, PRO2_USB_DIAGNOSTIC_REPORT_SIZE);
    memcpy(report, "P2DU", 4);
    report[4] = 13;
    report[5] = (configured ? 1u : 0u) |
                (hid_busy ? 2u : 0u) |
                (input_pending ? 4u : 0u) |
                (vendor_busy ? 8u : 0u) |
                (!steam_native_mode ? 0x10u : 0u) |
                0x20u |
                (steam_native_mode ? 0x40u : 0u) |
                (vendor_out_armed ? 0x80u : 0u);
    report[6] = usb_led_stage;
    report[7] = input_sequence;
    put_le32(report + 8, (uint32_t)usb_diagnostics.last_start_result);
    put_le32(report + 12, usb_diagnostics.input_updates);
    put_le32(report + 16, usb_diagnostics.periodic_ticks);
    put_le32(report + 20, usb_diagnostics.hid_starts);
    put_le32(report + 24, usb_diagnostics.hid_completions);
    put_le32(report + 28, usb_diagnostics.hid_failures);
    put_le32(report + 32, usb_diagnostics.hid_last_complete_bytes);
    put_le32(report + 36, usb_diagnostics.hid_out_reports);
    put_le32(report + 40, usb_diagnostics.vendor_out_reports);
    put_le32(report + 44, usb_diagnostics.vendor_in_starts);
    put_le32(report + 48, usb_diagnostics.vendor_in_completions);
    put_le32(report + 52, usb_diagnostics.ms_os_10_compat_requests);
    put_le32(report + 56, usb_diagnostics.ms_os_10_property_requests);
    put_le32(report + 60, usb_diagnostics.ms_os_20_descriptor_requests);
    return PRO2_USB_DIAGNOSTIC_REPORT_SIZE;
}

static size_t build_vendor_diagnostic_report(uint8_t *report,
                                             size_t report_max)
{
    if (!report || report_max < PRO2_USB_DIAGNOSTIC_REPORT_SIZE) return 0;
    memset(report, 0, PRO2_USB_DIAGNOSTIC_REPORT_SIZE);
    memcpy(report, "P2DV", 4);
    report[4] = 1;
    report[5] = (last_vendor_command_valid ? 1u : 0u) |
                (last_vendor_reply_valid ? 2u : 0u);
    report[6] = (uint8_t)last_vendor_command_len;
    report[7] = (uint8_t)(last_vendor_command_len >> 8);
    report[8] = (uint8_t)last_vendor_reply_len;
    report[9] = (uint8_t)(last_vendor_reply_len >> 8);
    report[10] = last_vendor_command[0];
    report[11] = last_vendor_command[3];
    if (last_vendor_command_len >= 16) {
        memcpy(report + 12, last_vendor_command + 12, 4);
    }
    put_le32(report + 16, usb_diagnostics.vendor_out_reports);
    put_le32(report + 20, vendor_commands_dropped_busy);
    memcpy(report + 24, last_vendor_command, sizeof(last_vendor_command));
    memcpy(report + 44, last_vendor_reply, sizeof(last_vendor_reply));
    return PRO2_USB_DIAGNOSTIC_REPORT_SIZE;
}
