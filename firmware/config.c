//
// config.c — persistent bridge configuration.
//
// Storage: one 4K sector at the top of flash (PICO_FLASH_SIZE_BYTES -
// FLASH_SECTOR_SIZE), holding a magic + version + CRC-protected
// config_body_t. Reflashing the .uf2 wipes it along with everything
// else, which is fine — defaults are always sane.
//
// Concurrency: the RAM copy is only mutated from the main loop (USB
// callbacks and BOOTSEL handler both run there via tud_task), so no
// locking is needed. flash_save goes through flash_safe_execute, which
// stalls XIP + interrupts briefly; TinyUSB tolerates the pause (same
// approach as DS5Dongle's config_save).
//
#include "config.h"
#include "usb_descriptors.h"
#include "log.h"

#include <string.h>
#include "hardware/flash.h"
#include "pico/flash.h"

#define CONFIG_FLASH_SECTOR_OFFS (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

#define CONFIG_MAGIC 0x53573243u  // "SW2C"

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t body_len;
    config_body_t body;
    uint16_t crc;  // CRC-16/CCITT-FALSE over body
} flash_record_t;

static config_body_t live;

// ---------------- CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) ----------------
static uint16_t crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

// ---------------- flash plumbing ----------------
// Page-sized staging buffer: flash_range_program needs a whole 256-byte
// page and the record itself is only ~70 bytes. Static (not stack) so the
// pointer stays valid inside the flash_safe_execute callback.
static uint8_t save_page[FLASH_PAGE_SIZE];

static void config_save_cb(void *param) {
    (void)param;
    flash_range_erase(CONFIG_FLASH_SECTOR_OFFS, FLASH_SECTOR_SIZE);
    flash_range_program(CONFIG_FLASH_SECTOR_OFFS, save_page, FLASH_PAGE_SIZE);
}

// ---------------- defaults / load ----------------
void config_reset_defaults(void) {
    memset(&live, 0, sizeof live);
    live.version = CONFIG_VERSION;
    live.persona = USB_PERSONA_PRO;  // README: switch-pro is the default host persona
}

void config_init(void) {
    config_reset_defaults();

    const flash_record_t *rec =
        (const flash_record_t *)(XIP_BASE + CONFIG_FLASH_SECTOR_OFFS);
    if (rec->magic != CONFIG_MAGIC) {
        LOG("[cfg] flash blank/invalid magic -> defaults");
        return;
    }
    if (rec->body_len != sizeof(config_body_t) ||
        rec->crc != crc16((const uint8_t *)&rec->body, rec->body_len)) {
        LOG("[cfg] flash CRC/len mismatch (v%u len=%u) -> defaults",
            rec->version, rec->body_len);
        return;
    }
    // Front-prefix copy: an older firmware meeting a newer body still
    // takes only the fields it knows.
    uint16_t take = rec->body_len < sizeof live ? rec->body_len : (uint16_t)sizeof live;
    memcpy(&live, &rec->body, take);
    if (live.persona > USB_PERSONA_DS4) live.persona = USB_PERSONA_PRO;
    LOG("[cfg] loaded from flash: persona=%s", live.persona ? "ds4" : "switch-pro");
}

const config_body_t *config_get(void) { return &live; }

bool config_set(const uint8_t *body, uint16_t len) {
    if (!body || len < 2) return false;
    uint16_t take = len < sizeof live ? len : (uint16_t)sizeof live;
    const bool persona_before = live.persona;

    // Field-wise accept: copy the prefix, re-stamp our version.
    memcpy(&live, body, take);
    live.version = CONFIG_VERSION;
    if (live.persona > USB_PERSONA_DS4) live.persona = USB_PERSONA_PRO;

    LOG("[cfg] set: persona=%s (len=%u)", live.persona ? "ds4" : "switch-pro", len);
    return live.persona != persona_before;
}

bool config_save(void) {
    flash_record_t rec;
    memset(&rec, 0x00, sizeof rec);
    rec.magic = CONFIG_MAGIC;
    rec.version = CONFIG_VERSION;
    rec.body_len = sizeof live;
    rec.body = live;
    rec.crc = crc16((const uint8_t *)&rec.body, sizeof rec.body);

    memset(save_page, 0xFF, sizeof save_page);  // erased-flash fill pattern
    memcpy(save_page, &rec, sizeof rec);

    const int rc = flash_safe_execute(config_save_cb, NULL, 1000);
    if (rc != PICO_OK) {
        LOG("[cfg] save failed: flash_safe_execute rc=%d", rc);
        return false;
    }
    LOG("[cfg] saved to flash @0x%x", CONFIG_FLASH_SECTOR_OFFS);
    return true;
}
