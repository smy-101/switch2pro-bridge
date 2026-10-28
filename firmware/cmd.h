#ifndef CMD_H
#define CMD_H
#include <stdint.h>
#include <stdbool.h>

// Vendor config channel over HID feature reports — same IDs as DS5Dongle,
// so tooling habits transfer. Declared for BOTH personas in
// usb_descriptors.c; the web page (web/index.html) is the primary client.
//
//   GET 0xF7  config_body_t (60 bytes, layout in config.h)
//   GET 0xF8  firmware version string (NUL-terminated ASCII)
//   GET 0xF9  status, 16 bytes:
//               [0]    persona        0 = switch-pro, 1 = ds4
//               [1]    ble_state      0 off / 1 scanning / 2 connecting /
//                                     3 setup / 4 init / 5 switching /
//                                     6 streaming
//               [2]    flags          bit0 streaming, bit1 charging
//               [3]    reserved (0)
//               [4..5] battery mV     LE16 (0 when no report seen yet)
//               [6..7] peer PID       LE16 (0x2009 Pro etc., 0 = none)
//               [8..15] reserved (0)
//
//   SET 0xF6  payload[0] = funcid:
//               0x01 + config_body  update RAM config (clamped)
//               0x02                persist RAM config to flash
//               0x03                re-enumerate USB (apply persona change)
//
// Persona changes need a re-enumeration (VID/PID/descriptors all change);
// the client does set -> save -> reconnect(0x03) and reopens the device,
// which comes back under the other VID/PID identity.

#define CMD_REPORT_SET     0xF6
#define CMD_REPORT_CONFIG  0xF7
#define CMD_REPORT_VERSION 0xF8
#define CMD_REPORT_STATUS  0xF9

// Firmware version string returned on GET 0xF8; also used by the boot log.
// Keep in sync with README.md 版本号 and release tags.
#define SW2BRIDGE_VERSION "sw2bridge v0.3"

enum {
    CMD_FUNC_SET_CONFIG = 0x01,
    CMD_FUNC_SAVE_FLASH = 0x02,
    CMD_FUNC_RECONNECT  = 0x03,
};

bool bridge_cmd_is_mine(uint8_t report_id);

// tud_hid_get_report_cb hook. Returns the number of bytes written
// (0 -> TinyUSB STALLs the request).
uint16_t bridge_cmd_get(uint8_t report_id, uint8_t *buffer, uint16_t reqlen);

// tud_hid_set_report_cb hook for feature reports (report_id 0xF6).
void bridge_cmd_set(uint8_t report_id, const uint8_t *buffer, uint16_t bufsize);

// Main loop: perform ops deferred out of control-transfer context
// (USB re-enumeration). Returns true if a re-enumeration happened.
bool bridge_cmd_poll(void);

#endif
