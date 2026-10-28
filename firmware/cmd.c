//
// cmd.c — vendor config channel (WebHID feature-report protocol).
//
// All entry points run from TinyUSB's usbd task == main loop context
// (tud_task), same as the rest of the bridge, so plain statics are fine.
// USB re-enumeration is deferred to the main loop instead of sleeping
// inside the control-transfer callback (avoids stalling the status stage).
//
#include "cmd.h"
#include "config.h"
#include "log.h"
#include "sw2_ble.h"
#include "usb_descriptors.h"

#include <string.h>

// Keep in sync with README.md 版本号 and release tags.
#define SW2BRIDGE_VERSION "sw2bridge v0.3"

typedef enum {
    RECONNECT_NONE = 0,
    RECONNECT_USB,
} pending_op_t;

static pending_op_t pending;

bool bridge_cmd_is_mine(uint8_t report_id) {
    return report_id >= CMD_REPORT_SET && report_id <= CMD_REPORT_STATUS;
}

// ---------------------------------------------------------------- GET
uint16_t bridge_cmd_get(uint8_t report_id, uint8_t *buffer, uint16_t reqlen) {
    switch (report_id) {
    case CMD_REPORT_CONFIG: {
        const uint16_t n = sizeof(config_body_t) < reqlen
                               ? (uint16_t)sizeof(config_body_t) : reqlen;
        memcpy(buffer, config_get(), n);
        return n;
    }
    case CMD_REPORT_VERSION: {
        const uint16_t len = (uint16_t)strlen(SW2BRIDGE_VERSION);
        const uint16_t n = len < reqlen ? len : reqlen;
        memcpy(buffer, SW2BRIDGE_VERSION, n);
        return n;
    }
    case CMD_REPORT_STATUS: {
        const uint16_t n = 16 < reqlen ? 16 : reqlen;
        memset(buffer, 0, n);

        uint16_t mv = 0;
        uint8_t charging = 0;
        (void)sw2_ble_battery(&mv, &charging);

        buffer[0] = (uint8_t)usb_persona_get();
        buffer[1] = sw2_ble_state_code();
        uint8_t flags = 0;
        if (sw2_ble_is_streaming()) flags |= 0x01;
        if (charging) flags |= 0x02;
        buffer[2] = flags;
        buffer[3] = 0;
        buffer[4] = (uint8_t)(mv & 0xFF);
        buffer[5] = (uint8_t)(mv >> 8);
        const uint16_t pid = sw2_ble_peer_pid();
        buffer[6] = (uint8_t)(pid & 0xFF);
        buffer[7] = (uint8_t)(pid >> 8);
        return n;
    }
    default:
        return 0;
    }
}

// ---------------------------------------------------------------- SET
void bridge_cmd_set(uint8_t report_id, const uint8_t *buffer, uint16_t bufsize) {
    if (report_id != CMD_REPORT_SET || bufsize == 0) return;

    switch (buffer[0]) {
    case CMD_FUNC_SET_CONFIG:
        if (bufsize < 2) return;
        // Stage into the RAM config ONLY — the live persona variable must not
        // change before re-enumeration, or the report pump would emit the new
        // persona's report ids under the old persona's descriptors.
        (void)config_set(buffer + 1, bufsize - 1);
        LOG("[cmd] set config (staged persona=%s)",
            config_get()->persona ? "ds4" : "switch-pro");
        break;

    case CMD_FUNC_SAVE_FLASH:
        (void)config_save();
        break;

    case CMD_FUNC_RECONNECT:
        // Persona may have just changed; main loop performs the actual
        // disconnect/connect so we never sleep inside a control transfer.
        pending = RECONNECT_USB;
        LOG("[cmd] reconnect queued");
        break;

    default:
        LOG("[cmd] unknown funcid %02x", buffer[0]);
        break;
    }
}

// Main loop: run deferred ops. Returns true when a USB re-enumeration
// was performed (caller may need to resync report pump state).
bool bridge_cmd_poll(void) {
    if (pending == RECONNECT_USB) {
        pending = RECONNECT_NONE;
        // Persona switches identity exactly at re-enumeration time.
        usb_persona_set((usb_persona_t)config_get()->persona);
        usb_reenumerate();
        return true;
    }
    return false;
}
