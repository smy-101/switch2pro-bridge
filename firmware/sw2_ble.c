//
// sw2_ble.c — BLE central for Nintendo Switch 2 controllers (Pro focus).
//
// Wire behavior, from refs/linux-switch2/PROTOCOL.md and the BlueZ switch2
// plugin (tested against the Pro Controller 2):
//   * advertise: mfg data company 0x0553, payload 01 00 03 7e 05 <pid LE16>
//   * no SMP pairing: connect -> ATT MTU -> GATT -> 0x91 command sequence
//   * commands are write-without-response on 0x0014; ACKs arrive as
//     notifications on 0x001a
//   * init order is critical: ACK CCC first, all 14 commands, input CCC
//     last. Enabling input CCCs early floods the link with ~80 reports/s
//     and the controller then drops ACKs / commands.
//   * input report 0x09 is a 63-byte GATT value, so MTU must be >= 66;
//     the controller offers 512.
//
#include "sw2_ble.h"
#include "sw2_protocol.h"
#include "log.h"
#include "hci_dump.h"

#include <stdio.h>
#include <string.h>
#include "pico/critical_section.h"
#include "btstack.h"
#include "pico/cyw43_arch.h"
#include "pico/btstack_cyw43.h"

// ------------------------------------------------------------------ state
typedef enum {
    SW2_ST_OFF,
    SW2_ST_SCANNING,
    SW2_ST_CONNECTING,
    SW2_ST_SETUP,       // LE link up: MTU, discovery, CCC writes
    SW2_ST_INIT,        // running the 0x91 command sequence
    SW2_ST_STREAMING,   // native 0x09 notifications
} sw2_state_enum;

static sw2_state_enum st = SW2_ST_OFF;
static hci_con_handle_t conn;
static bd_addr_t peer_addr;
static bd_addr_type_t peer_addr_type;
static uint16_t peer_pid;
static bool stream_native;

// decoded report handoff (BTstack -> main)
static sw2_state_t latest;
static volatile bool latest_valid;
static critical_section_t report_cs;

// ------------------------------------------------------------------ timers
static btstack_timer_source_t tmr;
static btstack_packet_callback_registration_t hci_cb_reg;

// ATT flow state
static bool mtu_pending;        // manual MTU exchange in flight
static bool mtu_skip;           // diagnostic fallback: retry without MTU
static bool discovery_pending;  // primary-service discovery in flight
static uint8_t services_seen;
static uint8_t setup_stage;     // 1 = ACK CCC write, 2 = input CCC write
static volatile uint32_t native_rx_count;
static volatile uint32_t att_tx_count;   // ATT ACL packets actually sent

// BTstack delivers GATT notifications only to listeners registered here;
// gatt_client->callback is used for query responses, not notifications.
static gatt_client_notification_t notify_listener;

// Restart the single flow timer. BTstack forbids re-adding a registered
// timer: it asserts (compiled out here) and then corrupts the timer list by
// linking the node in twice, so remove first (safe no-op when not pending).
static void timer_schedule(void (*fn)(btstack_timer_source_t *), uint32_t ms) {
    btstack_run_loop_remove_timer(&tmr);
    btstack_run_loop_set_timer(&tmr, ms);
    btstack_run_loop_set_timer_handler(&tmr, fn);
    btstack_run_loop_add_timer(&tmr);
}

// ------------------------------------------------------------------ helpers
const char *sw2_ble_state_name(void) {
    switch (st) {
        case SW2_ST_OFF: return "off";
        case SW2_ST_SCANNING: return "scanning";
        case SW2_ST_CONNECTING: return "connecting";
        case SW2_ST_SETUP: return "setup";
        case SW2_ST_INIT: return "init";
        case SW2_ST_STREAMING: return "streaming";
    }
    return "?";
}
bool sw2_ble_is_streaming(void) { return st == SW2_ST_STREAMING; }
bool sw2_ble_use_native_reports(void) { return stream_native; }
uint16_t sw2_ble_peer_pid(void) { return peer_pid; }

// Vendor status report (cmd.h 0xF9): numeric state + last battery sample.
uint8_t sw2_ble_state_code(void) { return (uint8_t)st; }
bool sw2_ble_battery(uint16_t *mv, uint8_t *charging) {
    if (!latest_valid) return false;
    critical_section_enter_blocking(&report_cs);
    if (mv) *mv = latest.batt_mv;
    if (charging) *charging = latest.charging;
    critical_section_exit(&report_cs);
    return true;
}

bool sw2_ble_take_state(sw2_state_t *out) {
    if (!latest_valid) return false;
    critical_section_enter_blocking(&report_cs);
    *out = latest;
    latest_valid = false;
    critical_section_exit(&report_cs);
    return true;
}

static void store_report(const sw2_state_t *s) {
    critical_section_enter_blocking(&report_cs);
    latest = *s;
    latest_valid = true;
    critical_section_exit(&report_cs);
}

static void reset_session(void) {
    conn = 0;
    stream_native = false;
    mtu_pending = false;
    discovery_pending = false;
    setup_stage = 0;
    services_seen = 0;
    native_rx_count = 0;
    sw2_cal_set_defaults(&sw2_cal_l);
    sw2_cal_set_defaults(&sw2_cal_r);
}

// forward decls
static void start_scan(void);
static void run_next_act(void);
static void send_current_act(void);
static void fatal(const char *why);
static void setup_watchdog_cb(btstack_timer_source_t *t);
static void setup_discovery_start(void);
static void mtu_watchdog_cb(btstack_timer_source_t *t);
static void sw2_packet_handler(uint8_t packet_type, uint16_t channel,
                               uint8_t *packet, uint16_t size);

// ------------------------------------------------------------------ scanning
static bool adv_matches(const uint8_t *adv, uint8_t len, uint16_t *pid_out) {
    uint8_t i = 0;
    while (i + 1 < len) {
        uint8_t field_len = adv[i];
        if (field_len == 0 || (uint32_t)i + 1 + field_len > len) break;
        uint8_t type = adv[i + 1];
        const uint8_t *data = &adv[i + 2];
        uint8_t data_len = (uint8_t)(field_len - 1);
        if (type == 0xFF && data_len >= 9) {
            uint16_t company = (uint16_t)(data[0] | (data[1] << 8));
            if (company == SW2_ADV_COMPANY_ID &&
                memcmp(&data[2], SW2_ADV_MAGIC, sizeof(SW2_ADV_MAGIC)) == 0) {
                *pid_out = (uint16_t)(data[7] | (data[8] << 8));
                return true;
            }
        }
        i = (uint8_t)(i + 1 + field_len);
    }
    return false;
}

static void rescan_timer_cb(btstack_timer_source_t *t) { (void)t; start_scan(); }

static void start_scan(void) {
    reset_session();
    st = SW2_ST_SCANNING;
    LOG("[sw2] scanning...");
    gap_set_scan_parameters(1, 0x0030, 0x0030);
    gap_start_scan();
}

static void handle_advertising_report(const uint8_t *packet) {
    uint8_t adv_type = gap_event_advertising_report_get_advertising_event_type(packet);
    uint8_t addr_type = gap_event_advertising_report_get_address_type(packet);
    bd_addr_t addr;
    gap_event_advertising_report_get_address(packet, addr);
    uint8_t len = gap_event_advertising_report_get_data_length(packet);
    const uint8_t *adv = gap_event_advertising_report_get_data(packet);
    int8_t rssi = gap_event_advertising_report_get_rssi(packet);

    uint16_t pid = 0;
    if (!adv_matches(adv, len, &pid)) return;

#if SW2_SCAN_DEBUG
    LOG("[sw2] adv match pid=%04x rssi=%d type=%d addr=%s",
        pid, rssi, adv_type, bd_addr_to_str(addr));
#endif

    bool known =
#if SW2_ACCEPT_ANY_NS2
        pid == SW2_PID_PRO_CONTROLLER || pid == SW2_PID_JOYCON2_LEFT ||
        pid == SW2_PID_JOYCON2_RIGHT || pid == SW2_PID_GAMECUBE;
#else
        pid == SW2_PID_PRO_CONTROLLER;
#endif
    if (!known) {
        LOG("[sw2] unknown pid %04x, ignoring", pid);
        return;
    }

    gap_stop_scan();
    memcpy(peer_addr, addr, 6);
    peer_addr_type = addr_type;
    peer_pid = pid;
    st = SW2_ST_CONNECTING;
    LOG("[sw2] connecting to %s (pid %04x)", bd_addr_to_str(addr), pid);
    gap_connect(peer_addr, peer_addr_type);
}

// ------------------------------------------------------------------ init sequence
typedef enum {
    ACT_CMD,          // command with 8-byte header + payload, wait ACK
    ACT_FLASH_READ,   // flash read command, capture data
    ACT_RAW,          // pre-built BLE frame, no ACK expected
} act_kind_t;

typedef struct {
    act_kind_t kind;
    const char *name;
    uint8_t cmd, subcmd;
    uint8_t payload[24];
    uint8_t payload_len;
    uint32_t addr;    // flash read address
    uint8_t read_len; // flash read length
} act_t;

// 0x0a/0x08 extended vibration setup (payload length 0x14 = 20, per the
// working BlueZ plugin; PROTOCOL.md lists an extra 0x00 that this unit
// does not need).
static const uint8_t vib_setup[20] = {
    0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x35, 0x00, 0x46,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// The controller-side BLE bring-up, in linux-switch2's established order
// (switch2-init.c, NS2_INIT_BLE_* steps): transport preamble with the measured
// 0x94 feature mask, report selection, then the ordinary 0x27 controller
// feature set — the same hand-off the kernel driver performs after the flash
// reads. Everything is ACK-driven: the next step is only sent after the
// previous one's notification arrived on 0x001a.
static const act_t init_acts[] = {
    {ACT_CMD, "init-enable", SW2_CMD_INIT, SW2_SUBCMD_INIT_ENABLE,
     {0x01, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, 8, 0, 0},
    {ACT_CMD, "ble-setup1", SW2_CMD_BLE_SETUP1, 0x01, {0}, 0, 0, 0},
    {ACT_CMD, "ble-setup2", SW2_CMD_BLE_SETUP2, 0x01, {0}, 0, 0, 0},
    {ACT_CMD, "ble-pair", SW2_CMD_BLE_PAIR, 0x03, {0x00}, 1, 0, 0},
    {ACT_CMD, "feat-set-preamble", SW2_CMD_FEATSEL, SW2_SUBCMD_FEATSEL_SET,
     {SW2_FEATURE_BLE_PREAMBLE, 0, 0, 0}, 4, 0, 0},
    {ACT_CMD, "ble-motion", SW2_CMD_BLE_MOTION, 0x03, {0}, 0, 0, 0},
    {ACT_CMD, "vib-setup", SW2_CMD_VIBRATION, 0x08, {0}, 20, 0, 0},
    {ACT_CMD, "feat-enable-preamble", SW2_CMD_FEATSEL, SW2_SUBCMD_FEATSEL_ENABLE,
     {SW2_FEATURE_BLE_PREAMBLE, 0, 0, 0}, 4, 0, 0},
    {ACT_CMD, "select-report", SW2_CMD_INIT, SW2_SUBCMD_INIT_SELECT_REPORT,
     {SW2_REPORT_ID_PRO_NATIVE, 0, 0, 0}, 4, 0, 0},
    {ACT_CMD, "fw-info", SW2_CMD_FW_INFO, SW2_SUBCMD_FW_INFO_GET, {0}, 0, 0, 0},
    {ACT_CMD, "nfc-off", SW2_CMD_NFC, 0x0c, {0}, 0, 0, 0},
    {ACT_CMD, "nfc-on", SW2_CMD_NFC, 0x01, {0, 0, 0, 0}, 4, 0, 0},
    {ACT_CMD, "player-led", SW2_CMD_LED, SW2_SUBCMD_LED_SET,
     {0x01, 0, 0, 0, 0, 0, 0, 0}, 8, 0, 0},
    {ACT_FLASH_READ, "cal-L-factory", SW2_CMD_FLASH, SW2_SUBCMD_FLASH_READ,
     {0}, 0, SW2_FLASH_CAL_L_FACTORY, 9},
    {ACT_FLASH_READ, "cal-R-factory", SW2_CMD_FLASH, SW2_SUBCMD_FLASH_READ,
     {0}, 0, SW2_FLASH_CAL_R_FACTORY, 9},
    // Generated by the driver, not an 8-byte switch2 command: no reply.
    {ACT_RAW, "ble-raw-selector", SW2_CMD_NFC, 0x01,
     {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x30},
     11, 0, 0},
    {ACT_CMD, "feat-set", SW2_CMD_FEATSEL, SW2_SUBCMD_FEATSEL_SET,
     {SW2_FEATURE_PRO_CONTROLLER, 0, 0, 0}, 4, 0, 0},
    {ACT_CMD, "feat-enable", SW2_CMD_FEATSEL, SW2_SUBCMD_FEATSEL_ENABLE,
     {SW2_FEATURE_PRO_CONTROLLER, 0, 0, 0}, 4, 0, 0},
};
#define INIT_ACT_COUNT (sizeof(init_acts) / sizeof(init_acts[0]))
static uint8_t act_idx;
static uint8_t cmd_buf[64];

static void fatal(const char *why) {
    LOG("[sw2] FATAL: %s — disconnect, retry in 1s", why);
    if (conn) gap_disconnect(conn);
    st = SW2_ST_OFF;
    reset_session();
    timer_schedule(rescan_timer_cb, 1000);
}

static void guard_next_cb(btstack_timer_source_t *t) { (void)t; run_next_act(); }

static void cmd_timeout_cb(btstack_timer_source_t *t) {
    (void)t;
    if (st == SW2_ST_INIT) fatal("command reply timeout");
}

// The write-without-response API does not queue: it returns GATT_CLIENT_BUSY
// when the ACL buffer is momentarily unavailable. Retry instead of waiting
// for an ACK that was never going to arrive.
static void cmd_retry_cb(btstack_timer_source_t *t) {
    (void)t;
    if (st == SW2_ST_INIT) send_current_act();
}

static void setup_watchdog_cb(btstack_timer_source_t *t) {
    (void)t;
    if (st == SW2_ST_SETUP)
        fatal("ATT setup stalled");
}

static void stream_watchdog_cb(btstack_timer_source_t *t) {
    (void)t;
    if (st == SW2_ST_STREAMING && native_rx_count == 0)
        LOG("[sw2] streaming but no native report yet");
}

static void send_current_act(void) {
    const act_t *a = &init_acts[act_idx];
    uint8_t plen = a->payload_len;

    if (a->kind == ACT_RAW) {
        LOG("[sw2] step %u/%u %s (raw len %u)",
            (unsigned)(act_idx + 1), (unsigned)INIT_ACT_COUNT, a->name,
            (unsigned)plen);
        uint8_t status = gatt_client_write_value_of_characteristic_without_response(
            conn, SW2_HANDLE_COMMAND_VALUE, plen, (uint8_t *)a->payload);
        if (status == GATT_CLIENT_BUSY) {
            timer_schedule(cmd_retry_cb, 5);
            return;
        }
        if (status != ERROR_CODE_SUCCESS)
            LOG("[sw2] raw write status=%02x", status);
        timer_schedule(guard_next_cb, SW2_CMD_GUARD_MS);   // no ACK expected
        return;
    }

    if (a->kind == ACT_FLASH_READ) {
        cmd_buf[8] = a->read_len;
        cmd_buf[9] = 0x7e;
        cmd_buf[10] = 0;
        cmd_buf[11] = 0;
        cmd_buf[12] = (uint8_t)a->addr;
        cmd_buf[13] = (uint8_t)(a->addr >> 8);
        cmd_buf[14] = (uint8_t)(a->addr >> 16);
        cmd_buf[15] = (uint8_t)(a->addr >> 24);
        plen = 8;
    } else if (a->cmd == SW2_CMD_VIBRATION && plen == sizeof(vib_setup)) {
        memcpy(&cmd_buf[8], vib_setup, plen);
    } else if (plen) {
        memcpy(&cmd_buf[8], a->payload, plen);
    }

    cmd_buf[0] = a->cmd;
    cmd_buf[1] = SW2_CMD_DIR_HOST;
    cmd_buf[2] = SW2_CMD_TRANSPORT_BT;
    cmd_buf[3] = a->subcmd;
    cmd_buf[4] = 0x00;
    cmd_buf[5] = plen;
    cmd_buf[6] = 0x00;
    cmd_buf[7] = 0x00;

    uint16_t mtu = 23;
    (void)gatt_client_get_mtu(conn, &mtu);
    if (mtu < 23) mtu = 23;
    if ((uint16_t)(8 + plen) > (uint16_t)(mtu - 3)) {
        LOG("[sw2] step %u/%u %s skipped (%u B > MTU %u-3)",
            (unsigned)(act_idx + 1), (unsigned)INIT_ACT_COUNT, a->name,
            (unsigned)(8 + plen), (unsigned)mtu);
        timer_schedule(guard_next_cb, SW2_CMD_GUARD_MS);
        return;
    }

    LOG("[sw2] step %u/%u %s (%02x/%02x len %u)",
        (unsigned)(act_idx + 1), (unsigned)INIT_ACT_COUNT, a->name,
        a->cmd, a->subcmd, (unsigned)plen);
    uint8_t status = gatt_client_write_value_of_characteristic_without_response(
        conn, SW2_HANDLE_COMMAND_VALUE, (uint16_t)(8 + plen), cmd_buf);
    if (status == GATT_CLIENT_BUSY) {
        timer_schedule(cmd_retry_cb, 5);
        return;
    }
    if (status != ERROR_CODE_SUCCESS)
        LOG("[sw2] command write status=%02x", status);
    timer_schedule(cmd_timeout_cb, SW2_CMD_TIMEOUT_MS);
}

static void run_next_act(void) {
    if (st != SW2_ST_INIT) return;
    act_idx++;
    if (act_idx >= INIT_ACT_COUNT) {
        static uint16_t on = 0x0001;
        st = SW2_ST_SETUP;
        setup_stage = 2;
        LOG("[sw2] init complete; enabling native input CCC");
        uint8_t status = gatt_client_write_characteristic_descriptor_using_descriptor_handle(
            sw2_packet_handler, conn, SW2_HANDLE_NATIVE_INPUT_CCC, 2,
            (uint8_t *)&on);
        if (status != ERROR_CODE_SUCCESS) {
            fatal("native CCC write failed");
            return;
        }
        timer_schedule(setup_watchdog_cb, SW2_SETUP_SILENT_MS);
        return;
    }
    send_current_act();
}

// ------------------------------------------------------- notification inputs
static void handle_cmd_reply(const uint8_t *data, uint16_t len) {
#if SW2_SCAN_DEBUG
    log_hexdump("[sw2] cmd reply", data, len > 24 ? 24 : len);
#endif
    if (st != SW2_ST_INIT) return;
    const act_t *a = &init_acts[act_idx];
    if (len < 8) { fatal("short reply"); return; }

    // The controller echoes the command in byte 0. Byte 3 normally carries
    // the subcommand, but some firmware replies put 0 there, so only the
    // command byte is used for routing; ACKs are strictly sequential.
    if (data[0] != a->cmd) {
        LOG("[sw2] stray reply %02x (waiting %02x) — ignored", data[0], a->cmd);
        return;
    }
    if (data[1] != 0x01)
        LOG("[sw2] cmd %02x status %02x", data[0], data[1]);

    if (a->kind == ACT_FLASH_READ &&
        (int)len >= 8 + 8 + (int)a->read_len) {
        const uint8_t *pl = &data[8];
        uint32_t addr = (uint32_t)pl[4] | ((uint32_t)pl[5] << 8) |
                        ((uint32_t)pl[6] << 16) | ((uint32_t)pl[7] << 24);
        const uint8_t *blob = &pl[8];
        if (addr != a->addr) {
            LOG("[sw2] flash addr mismatch %08lx != %08lx",
                (unsigned long)addr, (unsigned long)a->addr);
        } else if (a->addr == SW2_FLASH_CAL_L_FACTORY) {
            LOG("[sw2] cal L factory %s",
                sw2_cal_decode_factory(blob, &sw2_cal_l) ? "ok" : "absent");
        } else if (a->addr == SW2_FLASH_CAL_R_FACTORY) {
            LOG("[sw2] cal R factory %s",
                sw2_cal_decode_factory(blob, &sw2_cal_r) ? "ok" : "absent");
        }
    } else if (a->cmd == SW2_CMD_FW_INFO && len >= 8 + 11) {
        LOG("[sw2] fw %u.%u.%u type %u dsp %u.%u.%u",
            data[8], data[9], data[10], data[11], data[16], data[17], data[18]);
    }

    timer_schedule(guard_next_cb, SW2_CMD_GUARD_MS);   // ~10ms guard
}

static void handle_input_notify(uint16_t value_handle, const uint8_t *data,
                                uint16_t len) {
    // 0x000e carries the controller's native report (Pro: 0x09). 0x000a carries
    // the common 0x05 report, which is the path the BlueZ plugin's measured
    // bring-up ends on. Accept both: whichever one the controller streams is
    // enough, and 0x05 is the documented fallback if 0x09 never shows up.
    const bool native = (value_handle == SW2_HANDLE_NATIVE_INPUT_VALUE);
    if (!native && value_handle != SW2_HANDLE_COMMON_INPUT_VALUE) return;
    static bool len_warned;
    if (len < 11) {
        if (!len_warned) {
            len_warned = true;
            LOG("[sw2] short native report len=%u", len);
        }
        return;
    }
    if (len != SW2_GATT_REPORT_LEN && !len_warned) {
        len_warned = true;
        LOG("[sw2] native report len=%u (expected 63)", len);
    }
    native_rx_count++;
    static bool first_report_logged;
    if (!first_report_logged) {
        first_report_logged = true;
        LOG("[sw2] first input report on 0x%04x (%s)", value_handle,
            native ? "native 0x09" : "common 0x05 fallback");
    }
    if (st != SW2_ST_STREAMING || !stream_native) return;
    sw2_state_t s;
    if (native) sw2_decode_pro_native(data, &s);
    else        sw2_decode_common(data, &s);
    if (native_rx_count == 1 || (native_rx_count % 100) == 0)
        LOG("[sw2] rx=%lu btn=%08lx lx=%u ly=%u rx=%u ry=%u",
            (unsigned long)native_rx_count, (unsigned long)s.buttons,
            s.raw_lx, s.raw_ly, s.raw_rx, s.raw_ry);
    store_report(&s);
}

// ---------------------------------------------------------------- rumble
// SDL encodes game rumble into the classic Switch 4-byte HD-rumble block and
// ships it as USB output report 0x10 (rumble only) or inside report 0x01
// (rumble + subcommand). The Switch 2 Pro does not speak that codec: it takes
// a 5-byte packed (freq, amp) x2 frame in output report 0x02, one frame per
// LRA, and over BLE that report goes to the vibration characteristic 0x0012
// (linux-switch2 switch2-ff.c, NS2_CTLR_TYPE_PRO branch):
//
//   [0]=0x02 [1]=0x50|seq [2..6]=frame [0x11]=0x50|seq [0x12..0x16]=frame
//
// The driver paces BLE haptics at 20 ms to keep input flowing, and caps the
// amplitude at 450/1024 for safety; both values are reused here.
#define SW2_RUMBLE_TICK_MS   20
#define SW2_RUMBLE_HOLD_MS   120     // keep repeating after the last host report
#define SW2_RUMBLE_MAX_AMP   450
#define SW2_RUMBLE_HI_FREQ   0x187   // driver's ERM-emulation bands
#define SW2_RUMBLE_LO_FREQ   0x112

static btstack_timer_source_t rumble_tmr;
static uint8_t rumble_raw[8];
static uint32_t rumble_ms;
static uint8_t rumble_seq;
static bool rumble_playing;

static void rumble_tick_cb(btstack_timer_source_t *t);

static void rumble_schedule(uint32_t ms) {
    btstack_run_loop_remove_timer(&rumble_tmr);
    btstack_run_loop_set_timer(&rumble_tmr, ms);
    btstack_run_loop_set_timer_handler(&rumble_tmr, rumble_tick_cb);
    btstack_run_loop_add_timer(&rumble_tmr);
}

// Classic block: [0..3] high band, [4..7] low band; both bands carry the same
// stereo pair. 0x40 in the low-band amplitude byte is the zero point.
static uint16_t rumble_frame(const uint8_t raw[8], uint8_t out[5]) {
    unsigned hi = raw[1] & 0xfe;
    int lo = (int)(raw[3] | ((raw[2] & 0x80) << 1)) - 0x40;
    unsigned lo_mag = (unsigned)(lo < 0 ? -lo : lo) * 2;

    if (lo_mag > 0xff) lo_mag = 0xff;

    unsigned mag = hi > lo_mag ? hi : lo_mag;      // 0..254ish, 0 = silence
    if (mag == 0) return 0;

    unsigned amp = mag * SW2_RUMBLE_MAX_AMP / 254;
    if (amp > SW2_RUMBLE_MAX_AMP) amp = SW2_RUMBLE_MAX_AMP;

    out[0] = (uint8_t)(SW2_RUMBLE_HI_FREQ & 0xff);
    out[1] = (uint8_t)(((SW2_RUMBLE_HI_FREQ >> 8) & 0x03) | ((amp & 0x3f) << 2));
    out[2] = (uint8_t)(((amp >> 6) & 0x0f) | ((SW2_RUMBLE_LO_FREQ & 0x0f) << 4));
    out[3] = (uint8_t)(((SW2_RUMBLE_LO_FREQ >> 4) & 0x3f) | ((amp & 0x03) << 6));
    out[4] = (uint8_t)((amp >> 2) & 0xff);
    return amp;
}

static void rumble_send(const uint8_t frame[5]) {
    uint16_t mtu = 23;
    (void)gatt_client_get_mtu(conn, &mtu);
    if (mtu < 66) {   // 63-byte value + 3-byte ATT header
        LOG("[sw2] rumble skipped (MTU %u < 66)", (unsigned)mtu);
        return;
    }
    // The controller's GATT values are 63 bytes; the rumble payload ends at
    // 0x16, so dropping the report's trailing padding byte keeps everything
    // that matters and stays inside the characteristic's value length.
    uint8_t pkt[63];
    memset(pkt, 0, sizeof pkt);
    pkt[0] = 0x02;
    pkt[1] = (uint8_t)(0x50 | (rumble_seq & 0x0f));
    memcpy(&pkt[2], frame, 5);
    pkt[0x11] = pkt[1];
    memcpy(&pkt[0x12], frame, 5);
    rumble_seq = (uint8_t)((rumble_seq + 1) & 0x0f);
    uint8_t s = gatt_client_write_value_of_characteristic_without_response(
        conn, SW2_HANDLE_VIBRATION_VALUE, sizeof pkt, pkt);
    if (s != ERROR_CODE_SUCCESS && s != GATT_CLIENT_BUSY)
        LOG("[sw2] rumble write status=%02x", s);
}

static void rumble_tick_cb(btstack_timer_source_t *t) {
    (void)t;
    if (st != SW2_ST_STREAMING || !conn) {
        rumble_playing = false;
        return;
    }
    uint8_t frame[5];
    uint16_t amp = rumble_frame(rumble_raw, frame);
    if (amp == 0 ||
        (uint32_t)(to_ms_since_boot(get_absolute_time()) - rumble_ms) > SW2_RUMBLE_HOLD_MS) {
        if (rumble_playing) {
            static const uint8_t stop[5] = {0, 0, 0, 0, 0};
            rumble_playing = false;
            rumble_send(stop);
        }
        return;
    }
    if (!rumble_playing) {
        rumble_playing = true;
        LOG("[sw2] rumble on (amp %u/450)", (unsigned)amp);
    }
    rumble_send(frame);
    rumble_schedule(SW2_RUMBLE_TICK_MS);
}

static volatile bool rumble_arm;
static volatile uint8_t led_mask;
static volatile bool led_pending;

void sw2_ble_set_rumble(const uint8_t *usb_rumble8) {
    if (st != SW2_ST_STREAMING) return;   // also covers the pre-init window
    memcpy(rumble_raw, usb_rumble8, sizeof rumble_raw);
    rumble_ms = to_ms_since_boot(get_absolute_time());
    rumble_arm = true;
}

void sw2_ble_set_player_led(uint8_t mask) {
    led_mask = mask;
    led_pending = true;
}

// 0x09/0x07 with the same 4-LED bitmask Steam sends for a Switch Pro
// (linux-switch2 switch2_set_player_id / switch2_player_pattern).
static void led_flush(void) {
    uint8_t cmd[16];
    memset(cmd, 0, sizeof cmd);
    cmd[0] = SW2_CMD_LED;
    cmd[1] = SW2_CMD_DIR_HOST;
    cmd[2] = SW2_CMD_TRANSPORT_BT;
    cmd[3] = SW2_SUBCMD_LED_SET;
    cmd[5] = 8;                    // payload length
    cmd[8] = led_mask;
    uint8_t s = gatt_client_write_value_of_characteristic_without_response(
        conn, SW2_HANDLE_COMMAND_VALUE, sizeof cmd, cmd);
    if (s != ERROR_CODE_SUCCESS && s != GATT_CLIENT_BUSY)
        LOG("[sw2] led write status=%02x", s);
}

// BTstack run-loop state: everything here is armed from the main loop, never
// from the TinyUSB set_report callback.
void sw2_ble_poll(void) {
    if (st != SW2_ST_STREAMING || !conn) return;
    if (led_pending) {
        led_pending = false;
        led_flush();
    }
    if (!rumble_arm) return;
    rumble_arm = false;
    if (!rumble_playing)
        rumble_schedule(SW2_RUMBLE_TICK_MS);
}

// ------------------------------------------------------- ATT setup
// Order proven by the BlueZ switch2 plugin:
//   MTU -> primary discovery -> ACK CCC -> 14 init commands -> input CCC.
static void setup_discovery_start(void) {
    services_seen = 0;
    discovery_pending = true;
    LOG("[sw2] discovery probe: primary services");
    gatt_client_discover_primary_services(sw2_packet_handler, conn);
    timer_schedule(setup_watchdog_cb, SW2_SETUP_SILENT_MS);
}

static void discovery_start_cb(btstack_timer_source_t *t) {
    (void)t;
    if (st == SW2_ST_SETUP) setup_discovery_start();
}

// The BlueZ plugin enables all five input CCCs after init. Native 0x000e is
// the one we decode, but enabling the others matches the tested sequence and
// avoids controller firmwares that gate the stream on the full set.
static void enable_extra_cccs(void) {
    static const uint16_t ccc[] = {0x000b, 0x001f, 0x0023, 0x0027};
    static uint16_t on = 0x0001;
    for (unsigned i = 0; i < sizeof(ccc) / sizeof(ccc[0]); i++) {
        uint8_t s = gatt_client_write_value_of_characteristic_without_response(
            conn, ccc[i], 2, (uint8_t *)&on);
        if (s != ERROR_CODE_SUCCESS)
            LOG("[sw2] ccc %04x write status=%02x", ccc[i], s);
    }
}

static void extra_ccc_cb(btstack_timer_source_t *t) {
    (void)t;
    if (st != SW2_ST_STREAMING) return;
    enable_extra_cccs();
    timer_schedule(stream_watchdog_cb, 2000);
}

static void ack_ccc_write(void) {
    static uint16_t on = 0x0001;
    setup_stage = 1;
    LOG("[sw2] enabling ACK CCC (0x001b)");
    uint8_t status = gatt_client_write_characteristic_descriptor_using_descriptor_handle(
        sw2_packet_handler, conn, SW2_HANDLE_COMMAND_REPLY_CCC, 2, (uint8_t *)&on);
    if (status != ERROR_CODE_SUCCESS) {
        fatal("ACK CCC write failed");
        return;
    }
    timer_schedule(setup_watchdog_cb, SW2_SETUP_SILENT_MS);
}

static void mtu_start_cb(btstack_timer_source_t *t) {
    (void)t;
    if (st != SW2_ST_SETUP || !mtu_pending) return;
    LOG("[sw2] requesting ATT MTU exchange (can_send_acl=%u)",
        hci_can_send_acl_packet_now(conn) ? 1u : 0u);
    uint32_t tx_before = att_tx_count;
    gatt_client_send_mtu_negotiation(sw2_packet_handler, conn);
    uint32_t tx_delta = att_tx_count - tx_before;
    LOG("[sw2] gatt MTU tx=%u reserved=%u can_send=%u",
        (unsigned)tx_delta, hci_is_packet_buffer_reserved() ? 1u : 0u,
        hci_can_send_acl_packet_now(conn) ? 1u : 0u);
    if (tx_delta == 0) {
        // The GATT client did not hand the request to HCI. Release any
        // buffer it left reserved, then send the same ATT PDU raw so we can
        // tell "controller ignores MTU" apart from "GATT client never sent".
        if (hci_is_packet_buffer_reserved()) {
            LOG("[sw2] releasing stuck GATT packet buffer");
            hci_release_packet_buffer();
        }
        // No raw fallback: l2cap_send_connectionless() is the broadcast
        // (connectionless) channel API, not a way to inject an ATT PDU on an
        // established link, and it wedges the connection. Let the MTU watchdog
        // retire this attempt and retry the session instead.
        LOG("[sw2] gatt MTU request never reached HCI — waiting for watchdog");
    }
    timer_schedule(mtu_watchdog_cb, SW2_MTU_WAIT_MS);
}

static void mtu_watchdog_cb(btstack_timer_source_t *t) {
    (void)t;
    if (st == SW2_ST_SETUP && mtu_pending) {
        mtu_pending = false;
        mtu_skip = true;
        fatal("MTU exchange unanswered; retry without MTU");
    }
}

static void sw2_packet_handler(uint8_t packet_type, uint16_t channel,
                               uint8_t *packet, uint16_t size) {
    (void)channel; (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                LOG("[sw2] btstack up");
                start_scan();
            }
            break;

        case GAP_EVENT_ADVERTISING_REPORT:
            if (st == SW2_ST_SCANNING) handle_advertising_report(packet);
            break;

        case HCI_EVENT_LE_META:
            if (hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_COMPLETE) {
                uint8_t status = hci_subevent_le_connection_complete_get_status(packet);
                if (st == SW2_ST_CONNECTING) {
                    if (status != ERROR_CODE_SUCCESS) {
                        LOG("[sw2] connect failed status=%02x", status);
                        start_scan();
                        break;
                    }
                    conn = hci_subevent_le_connection_complete_get_connection_handle(packet);
                    st = SW2_ST_SETUP;
                    setup_stage = 0;
                    LOG("[sw2] connected handle 0x%04x", conn);
                    // Connection params (7.5ms / 0 / 420ms) are already set
                    // for the outgoing link by gap_set_connection_parameters().
                    // Do NOT send an L2CAP Connection Parameter Update
                    // Request here: that PDU is Peripheral-only (Core Vol 3/A
                    // 4.20); sending it as Central wedges the Pro.
                    //
                    // Exchange MTU first: without it the 63-byte reports are
                    // truncated to 20 bytes.
                    if (mtu_skip) {
                        LOG("[sw2] MTU skipped — discovery at MTU 23 in %u ms",
                            SW2_MTU_SETTLE_MS);
                        timer_schedule(discovery_start_cb, SW2_MTU_SETTLE_MS);
                    } else {
                        mtu_pending = true;
                        LOG("[sw2] ATT MTU exchange in %u ms", SW2_MTU_SETTLE_MS);
                        timer_schedule(mtu_start_cb, SW2_MTU_SETTLE_MS);
                    }
                } else if (status != ERROR_CODE_SUCCESS && conn == 0) {
                    LOG("[sw2] connection attempt failed");
                    start_scan();
                }
            } else if (hci_event_le_meta_get_subevent_code(packet) ==
                       HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE) {
                LOG("[sw2] conn interval now %u x1.25ms",
                    hci_subevent_le_connection_update_complete_get_conn_interval(packet));
            }
            break;

        case GATT_EVENT_SERVICE_QUERY_RESULT: {
            if (st != SW2_ST_SETUP) break;
            gatt_client_service_t svc;
            gatt_event_service_query_result_get_service(packet, &svc);
            services_seen++;
            if (svc.uuid16) {
                LOG("[sw2] svc %04x  %04x..%04x",
                    svc.uuid16, svc.start_group_handle, svc.end_group_handle);
            } else {
                LOG("[sw2] svc 128b %02x%02x%02x%02x..  %04x..%04x %s",
                    svc.uuid128[0], svc.uuid128[1], svc.uuid128[2], svc.uuid128[3],
                    svc.start_group_handle, svc.end_group_handle,
                    memcmp(svc.uuid128, SW2_UUID_SERVICE, 16) == 0 ? "<- Nintendo controller" : "");
            }
            break;
        }

        case GATT_EVENT_QUERY_COMPLETE: {
            uint8_t status = gatt_event_query_complete_get_att_status(packet);
            hci_con_handle_t h = gatt_event_query_complete_get_handle(packet);

            if (st == SW2_ST_SETUP && discovery_pending) {
                discovery_pending = false;
                if (status != ERROR_CODE_SUCCESS) {
                    LOG("[sw2] discovery query status=%02x", status);
                    fatal("discovery failed");
                    break;
                }
                LOG("[sw2] discovery complete: %u service(s)%s", services_seen,
                    services_seen ? " — ATT alive" : "");
                ack_ccc_write();
                break;
            }

            if (status != ERROR_CODE_SUCCESS) {
                LOG("[sw2] gatt query status=%02x (con 0x%04x)", status, h);
                if (st == SW2_ST_SETUP && mtu_pending) {
                    mtu_pending = false;
                    mtu_skip = true;
                    fatal("MTU exchange rejected; retry without MTU");
                } else if (st == SW2_ST_SETUP && setup_stage) {
                    fatal("CCC write rejected");
                } else if (st == SW2_ST_INIT) {
                    fatal("gatt write failed");
                }
                break;
            }

            if (st == SW2_ST_SETUP && setup_stage == 1) {
                setup_stage = 0;
                LOG("[sw2] ACK CCC on; starting init sequence");
                act_idx = 0;
                st = SW2_ST_INIT;
                send_current_act();
                break;
            }
            if (st == SW2_ST_SETUP && setup_stage == 2) {
                setup_stage = 0;
                stream_native = true;
                st = SW2_ST_STREAMING;
                LOG("[sw2] native CCC on — streaming (native 0x09)");
                timer_schedule(extra_ccc_cb, 20);
                break;
            }
            break;
        }

        case GATT_EVENT_MTU:
            LOG("[sw2] ATT MTU -> %u", (unsigned)gatt_event_mtu_get_MTU(packet));
            mtu_pending = false;
            if (st == SW2_ST_SETUP) setup_discovery_start();
            break;

        case GATT_EVENT_NOTIFICATION: {
            uint16_t vh = gatt_event_notification_get_value_handle(packet);
            uint16_t vlen = gatt_event_notification_get_value_length(packet);
            const uint8_t *val = gatt_event_notification_get_value(packet);
            if (vh == SW2_HANDLE_COMMAND_REPLY_VALUE) handle_cmd_reply(val, vlen);
            else handle_input_notify(vh, val, vlen);
            break;
        }

        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            hci_con_handle_t h =
                hci_event_disconnection_complete_get_connection_handle(packet);
            if (h == conn && conn) {
                LOG("[sw2] disconnected — rescan in 1s");
                st = SW2_ST_OFF;
                reset_session();
                timer_schedule(rescan_timer_cb, 1000);
            }
            break;
        }

        default:
            break;
    }
}

// ------------------------------------------------------- HCI/ATT trace
// Bring-up aid: dump ATT ACL packets (both directions) until streaming and
// the few LE events that matter. This shows whether a request actually left
// the chip and whether the controller answered, without a full hcidump.
static void sw2_hci_dump_message(int log_level, const char *fmt, va_list ap) {
    if (log_level < HCI_DUMP_LOG_LEVEL_ERROR) return;
    char buf[160];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    LOG("[bt] %s", buf);
}

static void sw2_hci_dump_packet(uint8_t packet_type, uint8_t in,
                                uint8_t *packet, uint16_t len) {
    if (packet_type == HCI_ACL_DATA_PACKET) {
        if (st == SW2_ST_STREAMING) return;   // too noisy once reports flow
        if (len < 9) return;
        uint16_t handle = little_endian_read_16(packet, 0) & 0x0fff;
        uint16_t cid = little_endian_read_16(packet, 6);
        if (cid != 0x0004) return;            // ATT only
        if (!in) att_tx_count++;
        LOG("[hci] acl %s h=%04x len=%u att=%02x %02x %02x %02x",
            in ? "in" : "out", handle, len, packet[8],
            len > 9 ? packet[9] : 0, len > 10 ? packet[10] : 0,
            len > 11 ? packet[11] : 0);
        return;
    }
    if (packet_type == HCI_EVENT_PACKET && len >= 3) {
        uint8_t ev = packet[0];
        if (ev == HCI_EVENT_LE_META) {
            LOG("[hci] ev LE_META sub=%02x", packet[2]);
        } else if (ev == HCI_EVENT_DISCONNECTION_COMPLETE && len >= 6) {
            LOG("[hci] ev DISCONNECT status=%02x h=%04x reason=%02x",
                packet[2], little_endian_read_16(packet, 3), packet[5]);
        }
    }
}

static const hci_dump_t sw2_hci_dump = {
    .reset = NULL,
    .log_packet = sw2_hci_dump_packet,
    .log_message = sw2_hci_dump_message,
};

// ------------------------------------------------------------------ init
void sw2_ble_init(void) {
    critical_section_init(&report_cs);
    sw2_cal_set_defaults(&sw2_cal_l);
    sw2_cal_set_defaults(&sw2_cal_r);

    // BTstack requires explicit L2CAP and GATT-client bring-up before any
    // ATT traffic. Without them HCI scan/connect still work, but the fixed
    // ATT channel is not installed and the GATT client never gets
    // CAN_SEND_NOW, so requests are queued but never transmitted and time
    // out 30s later with ATT_ERROR_TIMEOUT (0x7f).
    l2cap_init();
    // sm_init() is NOT optional even though this bridge never pairs: the
    // GATT client starts every operation with
    // gap_reconnect_security_setup_active(), which returns true while the
    // per-connection sm_irk_lookup_state is IRK_LOOKUP_IDLE (zero-initialized
    // without SM). That silently blocks every ATT request before it reaches
    // HCI. sm_init() registers the SM connection handler, which moves the
    // state to W4_READY and then FAILED for this un-bonded public peer.
    sm_init();
    gatt_client_init();
    hci_dump_init(&sw2_hci_dump);

    // ACKs (0x001a) and input reports (0x000e) arrive as ATT notifications.
    // Without a listener BTstack drops them silently, even though the CCC is
    // enabled and the controller sends them.
    gatt_client_listen_for_characteristic_value_updates(
        &notify_listener, sw2_packet_handler, GATT_CLIENT_ANY_CONNECTION, NULL);

    // ATT MTU is capped at 512: the controller offers 512, and ATT's legal
    // maximum is 517. Without this cap BTstack advertises
    // HCI_ACL_PAYLOAD_SIZE - 4 = 1017 in the Exchange MTU request, which is
    // out of range and is silently ignored by the Pro.
    l2cap_set_max_le_mtu(512);
    LOG("[sw2] ATT client ready (l2cap+gatt, local MTU %u)",
        (unsigned)l2cap_max_le_mtu());

    // No sm_init()/pairing on purpose: this controller answers any SMP
    // pairing attempt with "Pairing Not Supported" and drops the link
    // (PROTOCOL.md §2).

    gap_set_connection_parameters(
        0x0060, 0x0030,
        SW2_CONN_INTERVAL_MIN, SW2_CONN_INTERVAL_MAX,
        SW2_CONN_LATENCY, SW2_CONN_SUPERVISION,
        0x0010, 0x0030);

    // Manual exchange on connect keeps the order MTU -> discovery explicit.
    gatt_client_mtu_enable_auto_negotiation(0);

    hci_cb_reg.callback = &sw2_packet_handler;
    hci_add_event_handler(&hci_cb_reg);

    hci_power_control(HCI_POWER_ON);
    LOG("[sw2] power on requested");
}
