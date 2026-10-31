//
// sw2_ble.c — BLE central for Nintendo Switch 2 controllers (Pro focus).
//
// Wire behavior per refs/linux-switch2/PROTOCOL.md:
//   * advertise: mfg data company 0x0553, payload 01 00 03 7e 05 <pid LE16>
//   * no SMP pairing needed: connect, then GATT on fixed handles
//   * conn params 7.5ms / latency 0 / supervision 420ms are required
//   * init: §8 command sequence over handle 0x0014, replies on 0x001a
//   * input: native report first, then switch CCC to common 0x05 (§45)
//
#include "sw2_ble.h"
#include "sw2_protocol.h"
#include "log.h"

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
    SW2_ST_SETUP,       // LE link up: enabling CCCs
    SW2_ST_INIT,        // running §8 command sequence
    SW2_ST_SWITCHING,   // native -> common subscription move
    SW2_ST_STREAMING,
} sw2_state_enum;

static sw2_state_enum st = SW2_ST_OFF;
static hci_con_handle_t conn;
static bd_addr_t peer_addr;
static bd_addr_type_t peer_addr_type;
static uint16_t peer_pid;
static bool stream_native;        // decoding native reports (fallback)
static bool native_seen, common_seen;

// decoded report handoff (IRQ -> main)
static sw2_state_t latest;
static volatile bool latest_valid;
static critical_section_t report_cs;

// ------------------------------------------------------------------ timers
static btstack_timer_source_t tmr;     // command/init flow
static btstack_timer_source_t tmr_wd;  // independent watchdog

// MTU strategy, driven by hardware observation: the Switch 2 Pro never
// answers an ATT Exchange MTU request (auto negotiation then wedges every
// queued write behind BTstack's 30s ATT timeout -> status 0x7f). Proven on
// two independent pairing sessions, so the exchange is skipped by default;
// the manual-exchange path is kept dormant for later experiments.
static bool mtu_skip = true;  // skip ATT MTU exchange entirely
static bool mtu_pending;      // manual exchange in flight

// Setup-stage probe state: the Pro also ignores plain ATT write requests
// (log #3), while PROTOCOL.md documents primary discovery as working with
// bluez. Discovery doubles as an ATT-liveness probe and reveals the real
// handle layout of this specific unit.
static bool discovery_pending;
static uint8_t services_seen;

// Restart the single flow timer. BTstack forbids re-adding a registered
// timer: it asserts (compiled out here) and then corrupts the timer list by
// linking the node in twice, so remove first (safe no-op when not pending).
static void timer_schedule(void (*fn)(btstack_timer_source_t *), uint32_t ms) {
    btstack_run_loop_remove_timer(&tmr);
    btstack_run_loop_set_timer(&tmr, ms);
    btstack_run_loop_set_timer_handler(&tmr, fn);
    btstack_run_loop_add_timer(&tmr);
}
static void watchdog_schedule(void (*fn)(btstack_timer_source_t *), uint32_t ms) {
    btstack_run_loop_remove_timer(&tmr_wd);
    btstack_run_loop_set_timer(&tmr_wd, ms);
    btstack_run_loop_set_timer_handler(&tmr_wd, fn);
    btstack_run_loop_add_timer(&tmr_wd);
}

// ------------------------------------------------------------------ helpers
const char *sw2_ble_state_name(void) {
    switch (st) {
        case SW2_ST_OFF: return "off";
        case SW2_ST_SCANNING: return "scanning";
        case SW2_ST_CONNECTING: return "connecting";
        case SW2_ST_SETUP: return "setup";
        case SW2_ST_INIT: return "init";
        case SW2_ST_SWITCHING: return "switching";
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
    native_seen = common_seen = false;
    stream_native = false;
    discovery_pending = false;
    sw2_cal_set_defaults(&sw2_cal_l);
    sw2_cal_set_defaults(&sw2_cal_r);
}

// forward decls
static void start_scan(void);
static void run_next_act(void);
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
    ACT_CMD,          // command with 8-byte header + payload, wait reply
    ACT_FLASH_READ,   // flash read command, capture data
    ACT_RAW_SELECTOR, // raw 11-byte selector write, no reply
    ACT_WAIT_NATIVE,  // block until native notification arrives
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

static const uint8_t vib_setup[21] = {
    0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x35, 0x00, 0x46, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static const act_t init_acts[] = {
    {ACT_CMD, "init-enable", SW2_CMD_INIT, SW2_SUBCMD_INIT_ENABLE,
     {0x01, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, 8, 0, 0},
    {ACT_CMD, "ble-setup1", SW2_CMD_BLE_SETUP1, 0x01, {0}, 0, 0, 0},
    {ACT_CMD, "ble-setup2", SW2_CMD_BLE_SETUP2, 0x01, {0}, 0, 0, 0},
    {ACT_CMD, "ble-pair", SW2_CMD_BLE_PAIR, 0x03, {0x00}, 1, 0, 0},
    {ACT_CMD, "feat-set-boot", SW2_CMD_FEATSEL, SW2_SUBCMD_FEATSEL_SET,
     {SW2_FEATURE_BLE_BOOT_MASK, 0, 0, 0}, 4, 0, 0},
    {ACT_CMD, "ble-motion", SW2_CMD_BLE_MOTION, 0x03, {0}, 0, 0, 0},
    {ACT_CMD, "vib-setup", SW2_CMD_VIBRATION, 0x08, {0}, 21, 0, 0},
    {ACT_CMD, "feat-enable-boot", SW2_CMD_FEATSEL, SW2_SUBCMD_FEATSEL_ENABLE,
     {SW2_FEATURE_BLE_BOOT_MASK, 0, 0, 0}, 4, 0, 0},
    {ACT_CMD, "start-input", SW2_CMD_INIT, SW2_SUBCMD_INIT_SELECT_REPORT,
     {SW2_REPORT_ID_PRO_NATIVE, 0, 0, 0}, 4, 0, 0},
    {ACT_WAIT_NATIVE, "wait-native", 0, 0, {0}, 0, 0, 0},
    {ACT_CMD, "fw-info", SW2_CMD_FW_INFO, SW2_SUBCMD_FW_INFO_GET, {0}, 0, 0, 0},
    {ACT_FLASH_READ, "serial", SW2_CMD_FLASH, SW2_SUBCMD_FLASH_READ, {0}, 0, SW2_FLASH_SERIAL, 0x10},
    {ACT_FLASH_READ, "cal-L-factory", SW2_CMD_FLASH, SW2_SUBCMD_FLASH_READ, {0}, 0, SW2_FLASH_CAL_L_FACTORY, 9},
    {ACT_FLASH_READ, "cal-R-factory", SW2_CMD_FLASH, SW2_SUBCMD_FLASH_READ, {0}, 0, SW2_FLASH_CAL_R_FACTORY, 9},
    {ACT_FLASH_READ, "cal-L-user", SW2_CMD_FLASH, SW2_SUBCMD_FLASH_READ, {0}, 0, SW2_FLASH_CAL_L_USER, 11},
    {ACT_FLASH_READ, "cal-R-user", SW2_CMD_FLASH, SW2_SUBCMD_FLASH_READ, {0}, 0, SW2_FLASH_CAL_R_USER, 11},
    {ACT_RAW_SELECTOR, "raw-selector", 0, 0, {0}, 0, 0, 0},
    {ACT_CMD, "feat-set-final", SW2_CMD_FEATSEL, SW2_SUBCMD_FEATSEL_SET,
     {SW2_FEATURE_PRO_V1, 0, 0, 0}, 4, 0, 0},
    {ACT_CMD, "feat-enable-final", SW2_CMD_FEATSEL, SW2_SUBCMD_FEATSEL_ENABLE,
     {SW2_FEATURE_PRO_V1, 0, 0, 0}, 4, 0, 0},
    {ACT_CMD, "fw-info-2", SW2_CMD_FW_INFO, SW2_SUBCMD_FW_INFO_GET, {0}, 0, 0, 0},
    {ACT_CMD, "player-led", SW2_CMD_LED, SW2_SUBCMD_LED_SET,
     {0x01, 0, 0, 0, 0, 0, 0, 0}, 8, 0, 0},
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

static void wait_native_timeout_cb(btstack_timer_source_t *t) {
    (void)t;
    if (st == SW2_ST_INIT && init_acts[act_idx].kind == ACT_WAIT_NATIVE) {
        if (native_seen) run_next_act();
        else fatal("native report never arrived");
    }
}

static void cmd_timeout_cb(btstack_timer_source_t *t) {
    (void)t;
    if (st == SW2_ST_INIT) fatal("command reply timeout");
}

static void setup_watchdog_cb(btstack_timer_source_t *t) {
    (void)t;
    if (st == SW2_ST_SETUP)
        fatal("ATT silent after connect");
}

static void send_current_act(void) {
    const act_t *a = &init_acts[act_idx];
    if (a->kind == ACT_WAIT_NATIVE) {
        if (native_seen) {
            run_next_act();
        } else {
            timer_schedule(wait_native_timeout_cb, SW2_NATIVE_FIRST_WAIT_MS);
        }
        return;
    }
    if (a->kind == ACT_RAW_SELECTOR) {
        static const uint8_t sel[11] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                        0x00, 0x00, 0x03, 0x30};
        LOG("[sw2] step %u/%u raw-selector (no reply)", act_idx + 1, (unsigned)INIT_ACT_COUNT);
        gatt_client_write_value_of_characteristic_without_response(
            conn, SW2_HANDLE_COMMAND_VALUE, sizeof(sel), (uint8_t *)sel);
        timer_schedule(guard_next_cb, SW2_CMD_GUARD_MS);
        return;
    }

    // vib-setup (8 hdr + 21 payload = 29 B) cannot fit a single ATT write at
    // the default MTU 23 (max 20 B). Skip it in that case — it is an opaque
    // setup packet; the rest of the sequence still proves ATT liveness.
    if (a->kind == ACT_CMD && a->cmd == SW2_CMD_VIBRATION && a->payload_len == 21) {
        uint16_t mtu = 23;
        (void)gatt_client_get_mtu(conn, &mtu);   // stays 23 when not exchanged
        if (mtu < 8 + 21 + 3) {
            LOG("[sw2] step %u/%u vib-setup skipped (29 B > MTU %u-3)",
                act_idx + 1, (unsigned)INIT_ACT_COUNT, mtu);
            timer_schedule(guard_next_cb, SW2_CMD_GUARD_MS);
            return;
        }
    }

    uint8_t plen = a->payload_len;
    if (a->kind == ACT_FLASH_READ) {
        cmd_buf[8] = a->read_len;
        cmd_buf[9] = 0x7e;
        cmd_buf[10] = 0; cmd_buf[11] = 0;
        cmd_buf[12] = (uint8_t)a->addr;
        cmd_buf[13] = (uint8_t)(a->addr >> 8);
        cmd_buf[14] = (uint8_t)(a->addr >> 16);
        cmd_buf[15] = (uint8_t)(a->addr >> 24);
        plen = 8;
        // vib-setup carries its own payload; flash reads build it above
    } else if (a->payload_len == 21 && a->cmd == SW2_CMD_VIBRATION) {
        memcpy(&cmd_buf[8], vib_setup, 21);
    } else {
        memcpy(&cmd_buf[8], a->payload, plen);
    }
    cmd_buf[0] = a->cmd;
    cmd_buf[1] = SW2_CMD_DIR_HOST;
    cmd_buf[2] = SW2_CMD_TRANSPORT_BT;
    cmd_buf[3] = a->subcmd;
    cmd_buf[4] = 0x00;
    cmd_buf[5] = plen;
    cmd_buf[6] = 0x00; cmd_buf[7] = 0x00;

    LOG("[sw2] step %u/%u %s (%02x/%02x len %u)", act_idx + 1, (unsigned)INIT_ACT_COUNT,
        a->name, a->cmd, a->subcmd, plen);
    // WRITE COMMAND (no ATT response): the Pro has never been observed to
    // answer write requests; replies arrive as notifications on 0x001a.
    gatt_client_write_value_of_characteristic_without_response(
        conn, SW2_HANDLE_COMMAND_VALUE, (uint16_t)(8 + plen), cmd_buf);
    timer_schedule(cmd_timeout_cb, SW2_CMD_TIMEOUT_MS);
}

static void unsub_native_cb(btstack_timer_source_t *t) {
    (void)t;
    static uint16_t off = 0x0000;
    LOG("[sw2] init complete; unsubscribing native");
    gatt_client_write_characteristic_descriptor_using_descriptor_handle(
        sw2_packet_handler, conn, SW2_HANDLE_NATIVE_INPUT_CCC, 2, (uint8_t *)&off);
}

static void sub_common_cb(btstack_timer_source_t *t) {
    (void)t;
    static uint16_t on = 0x0001;
    LOG("[sw2] subscribing common 0x05");
    gatt_client_write_characteristic_descriptor_using_descriptor_handle(
        sw2_packet_handler, conn, SW2_HANDLE_COMMON_INPUT_CCC, 2, (uint8_t *)&on);
}

static void common_watchdog_cb(btstack_timer_source_t *t) {
    (void)t;
    if (st == SW2_ST_SWITCHING && !common_seen) {
        static uint16_t on = 0x0001;
        LOG("[sw2] common 0x05 silent — falling back to native reports");
        stream_native = true;
        gatt_client_write_characteristic_descriptor_using_descriptor_handle(
            sw2_packet_handler, conn, SW2_HANDLE_NATIVE_INPUT_CCC, 2, (uint8_t *)&on);
        st = SW2_ST_STREAMING;
    }
}

static void run_next_act(void) {
    if (st != SW2_ST_INIT) return;
    // Advance on entry: every caller has just finished act `act_idx` (reply
    // guard, native-report gate) except the sequence start, which calls
    // send_current_act() directly — advancing there too is what kept the
    // sequence stuck on step 1 forever.
    act_idx++;
    if (act_idx >= INIT_ACT_COUNT) {
        st = SW2_ST_SWITCHING;
        timer_schedule(unsub_native_cb, 250);
        watchdog_schedule(common_watchdog_cb,
                          250 + SW2_NATIVE_TO_COMMON_DELAY_MS + SW2_COMMON_WAIT_MS);
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

    // Replies echo the request command/subcommand (PROTOCOL.md §6.1: request
    // 03 91 01 0a -> reply 03 01 01 0a; the Valve driver matches the same
    // way). Ignore anything else arriving on the reply handle — advancing on
    // a stray notification would skip a step.
    if (data[0] != a->cmd || data[3] != a->subcmd) {
        LOG("[sw2] stray reply %02x/%02x (waiting %02x/%02x) — ignored",
            data[0], data[3], a->cmd, a->subcmd);
        return;
    }

    if (a->kind == ACT_FLASH_READ && (uint32_t)(len - 8) >= 8u + a->read_len) {
        const uint8_t *pl = &data[8];
        uint32_t addr = (uint32_t)pl[4] | ((uint32_t)pl[5] << 8) |
                        ((uint32_t)pl[6] << 16) | ((uint32_t)pl[7] << 24);
        const uint8_t *blob = &pl[8];
        if (addr != a->addr) {
            LOG("[sw2] flash addr mismatch %08lx != %08lx",
                (unsigned long)addr, (unsigned long)a->addr);
        } else if (a->addr == SW2_FLASH_SERIAL) {
            log_hexdump("[sw2] serial", blob, a->read_len);
        } else if (a->addr == SW2_FLASH_CAL_L_FACTORY) {
            LOG("[sw2] cal L factory %s", sw2_cal_decode_factory(blob, &sw2_cal_l) ? "ok" : "absent");
        } else if (a->addr == SW2_FLASH_CAL_R_FACTORY) {
            LOG("[sw2] cal R factory %s", sw2_cal_decode_factory(blob, &sw2_cal_r) ? "ok" : "absent");
        } else if (a->addr == SW2_FLASH_CAL_L_USER) {
            if (sw2_cal_decode_user(blob, &sw2_cal_l)) LOG("[sw2] cal L USER applied");
        } else if (a->addr == SW2_FLASH_CAL_R_USER) {
            if (sw2_cal_decode_user(blob, &sw2_cal_r)) LOG("[sw2] cal R USER applied");
        }
    } else if (a->kind == ACT_CMD && a->cmd == SW2_CMD_FW_INFO && len >= 8 + 11) {
        LOG("[sw2] fw %u.%u.%u type %u dsp %u.%u.%u",
            data[8], data[9], data[10], data[11], data[16], data[17], data[18]);
    }

    timer_schedule(guard_next_cb, SW2_CMD_GUARD_MS);   // §8.6: ~10ms guard
}

static void handle_input_notify(uint16_t value_handle, const uint8_t *data, uint16_t len) {
    static bool len_warned;
    if (len != SW2_GATT_REPORT_LEN) {
        if (!len_warned) {   // report rate is ~133 Hz — log the condition once
            len_warned = true;
            LOG("[sw2] report len %u (expected 63) — MTU too small?", len);
        }
        if (len < SW2_GATT_REPORT_LEN) return;
    } else {
        len_warned = false;
    }
    sw2_state_t s;
    if (value_handle == SW2_HANDLE_NATIVE_INPUT_VALUE) {
        native_seen = true;
        if (st == SW2_ST_STREAMING && stream_native) {
            sw2_decode_pro_native(data, &s);
            store_report(&s);
        } else if (st == SW2_ST_INIT && init_acts[act_idx].kind == ACT_WAIT_NATIVE) {
            LOG("[sw2] first native report seen (+10ms sync)");
            timer_schedule(guard_next_cb, 10);   // §8.1 step 10
        }
    } else if (value_handle == SW2_HANDLE_COMMON_INPUT_VALUE) {
        if (st == SW2_ST_SWITCHING) {
            common_seen = true;
            LOG("[sw2] common report flowing — streaming (common)");
            stream_native = false;
            st = SW2_ST_STREAMING;
        }
        if (st == SW2_ST_STREAMING && !stream_native) {
            sw2_decode_common(data, &s);
            store_report(&s);
        }
    }
}

// ------------------------------------------------------- packet handler
static btstack_packet_callback_registration_t hci_cb_reg;

// ------------------------------------------------------- MTU / ATT setup
// Setup sequence, shaped by hardware observations (logs #1-#3):
//   * the Pro never answers an ATT Exchange MTU request
//   * it never answers plain ATT write requests either
//   * bluez-switch2 evidence (PROTOCOL.md 4.3): primary discovery works
// So: probe with primary-service discovery first (also reveals the real
// handle layout), then write CCCs as WRITE COMMANDs (no ATT response
// expected), and run the init sequence on no-response writes as well —
// command replies arrive as notifications on handle 0x001a anyway.

static void setup_discovery_start(void) {
    services_seen = 0;
    discovery_pending = true;
    LOG("[sw2] discovery probe: primary services");
    gatt_client_discover_primary_services(sw2_packet_handler, conn);
    timer_schedule(setup_watchdog_cb, SW2_SETUP_SILENT_MS);
}

static void init_start_cb(btstack_timer_source_t *t) {
    (void)t;
    if (st != SW2_ST_SETUP) return;
    LOG("[sw2] CCCs written (no-response); starting init sequence");
    act_idx = 0;
    st = SW2_ST_INIT;
    send_current_act();   // step 1; replies advance via run_next_act
}

static void ccc_native_write_cb(btstack_timer_source_t *t) {
    (void)t;
    if (st != SW2_ST_SETUP) return;
    static uint16_t on = 0x0001;
    gatt_client_write_value_of_characteristic_without_response(
        conn, SW2_HANDLE_NATIVE_INPUT_CCC, 2, (uint8_t *)&on);
    timer_schedule(init_start_cb, 40);
}

static void ccc_write_and_init(void) {
    static uint16_t on = 0x0001;
    gatt_client_write_value_of_characteristic_without_response(
        conn, SW2_HANDLE_COMMAND_REPLY_CCC, 2, (uint8_t *)&on);
    // stagger the second write so the first has left the ACL buffer
    timer_schedule(ccc_native_write_cb, 20);
}

static void mtu_watchdog_cb(btstack_timer_source_t *t) {
    (void)t;
    if (st == SW2_ST_SETUP && mtu_pending) {
        mtu_pending = false;
        mtu_skip = true;   // latch: no more MTU exchanges this power cycle
        LOG("[sw2] MTU exchange unanswered — next attempt without it (MTU 23)");
        fatal("MTU exchange unanswered");
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
                    LOG("[sw2] connected handle 0x%04x", conn);
                    // Connection params (7.5ms / 0 / 420ms) are already set for
                    // the outgoing link by gap_set_connection_parameters(). Do
                    // NOT send an L2CAP Connection Parameter Update Request
                    // here: that PDU is Peripheral-only (Core Vol 3/A 4.20).
                    // Sending it as Central is a spec violation — observed on
                    // the Switch 2 Pro as a wedged ATT server or an instant
                    // link drop.
                    //
                    // The Pro also never answers an ATT MTU exchange, which
                    // (with auto negotiation) wedges every queued write for
                    // 30s. Attempt the exchange manually once; if silent,
                    // latch mtu_skip and fall back to plain writes at MTU 23.
                    if (mtu_skip) {
                        setup_discovery_start();
                    } else {
                        mtu_pending = true;
                        LOG("[sw2] requesting MTU exchange");
                        gatt_client_send_mtu_negotiation(sw2_packet_handler, conn);
                        timer_schedule(mtu_watchdog_cb, SW2_MTU_WAIT_MS);
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
                ccc_write_and_init();
                break;
            }
            if (status != ERROR_CODE_SUCCESS) {
                LOG("[sw2] gatt query status=%02x (con 0x%04x)", status, h);
                if (st == SW2_ST_SETUP && mtu_pending) {
                    // explicit rejection of the manual MTU exchange
                    mtu_pending = false;
                    mtu_skip = true;
                    fatal("MTU exchange rejected — continuing without");
                    break;
                }
                if (st == SW2_ST_INIT) { fatal("gatt write failed"); break; }
            }
            if (st == SW2_ST_SWITCHING && !common_seen && !stream_native) {
                // native CCC off completed -> subscribe common
                timer_schedule(sub_common_cb, SW2_NATIVE_TO_COMMON_DELAY_MS);
            }
            break;
        }

        case GATT_EVENT_MTU:
            LOG("[sw2] ATT MTU -> %u", gatt_event_mtu_get_MTU(packet));
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

// ------------------------------------------------------------------ init
void sw2_ble_init(void) {
    critical_section_init(&report_cs);
    sw2_cal_set_defaults(&sw2_cal_l);
    sw2_cal_set_defaults(&sw2_cal_r);

    gap_set_connection_parameters(
        0x0060, 0x0030,
        SW2_CONN_INTERVAL_MIN, SW2_CONN_INTERVAL_MAX,
        SW2_CONN_LATENCY, SW2_CONN_SUPERVISION,
        0x0010, 0x0030);

    gatt_client_mtu_enable_auto_negotiation(0);   // exchange is driven manually above

    hci_cb_reg.callback = &sw2_packet_handler;
    hci_add_event_handler(&hci_cb_reg);

    hci_power_control(HCI_POWER_ON);
    LOG("[sw2] power on requested");
}
