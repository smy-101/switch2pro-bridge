#ifndef CONFIG_H
#define CONFIG_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Bridge configuration exposed over the HID vendor feature reports
// (see cmd.h) and persisted in the last flash sector.
//
// Wire layout contract (mirrors web/index.html):
//   [0]      version        (CONFIG_VERSION)
//   [1]      persona        0 = switch-pro, 1 = ds4
//   [2..59]  reserved       future knobs (rumble gain, gyro, LED, ...)
//
// Rules for evolution: never reorder or resize existing fields; new fields
// claim reserved bytes from the front and CONFIG_VERSION increments; older
// firmware loads newer bodies by copying only the bytes it knows.
#define CONFIG_VERSION 1

// NOTE: pico-sdk's newlib maps __packed to nothing and sw2_decode.h already
// claims the `struct __packed` tag, so use the explicit GCC attribute here.
typedef struct __attribute__((packed)) {
    uint8_t version;
    uint8_t persona;         // usb_persona_t value
    uint8_t reserved[58];
} config_body_t;             // 60 bytes (report payload is funcid + 63)

// Load from flash (CRC/version checked) or fall back to defaults.
// Call once at boot, before tusb_init(), so descriptors enumerate with
// the persisted persona.
void config_init(void);

// Live RAM copy (authoritative between set/save cycles).
const config_body_t *config_get(void);

// Replace the RAM copy from a wire body. Invalid values are clamped,
// unknown-version bodies are accepted field-by-field (front-prefix).
// Returns true when the persona value changed (caller should re-enumerate).
bool config_set(const uint8_t *body, uint16_t len);

// Persist the RAM copy to flash. Safe while USB/BTstack are running
// (flash_safe_execute). Returns false on flash failure or bad context.
bool config_save(void);

// Reset RAM copy to compiled-in defaults (does NOT touch flash).
void config_reset_defaults(void);

#endif
