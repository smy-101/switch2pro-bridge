//
// sw2bridge — Nintendo Switch 2 Pro Controller → DualShock 4 USB bridge
// for Raspberry Pi Pico 2 W. See ../README.md and ../docs/BRINGUP.md.
//
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "pico/cyw43_arch.h"
#include "pico/btstack_cyw43.h"
#include "hardware/watchdog.h"
#include "tusb.h"

#include "log.h"
#include "sw2_ble.h"
#include "sw2_protocol.h"
#include "ds4.h"

static uint8_t ds4_body[DS4_INPUT_REPORT_BODY];
static uint32_t last_report_ms;
static uint32_t last_keepalive_ms;

// ------------------------------------------------------------------ HID I/O
// DS4 output report 0x05 (rumble/LED from DS4Windows/Steam): logged, v2 will
// translate to Switch 2 Pro rumble frames (PROTOCOL.md §44).
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    (void)itf;
    if (report_type == HID_REPORT_TYPE_OUTPUT && report_id == DS4_OUTPUT_REPORT_ID) {
        LOG("[ds4] output report 0x05: flags=%02x rumble R=%u L=%u led=%02x%02x%02x",
            buffer[1], bufsize > 4 ? buffer[4] : 0, bufsize > 5 ? buffer[5] : 0,
            bufsize > 6 ? buffer[6] : 0, bufsize > 7 ? buffer[7] : 0,
            bufsize > 8 ? buffer[8] : 0);
    } else {
        LOG("[ds4] set_report id=%02x type=%d len=%u", report_id, report_type, bufsize);
    }
}

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)itf; (void)buffer; (void)reqlen;
    LOG("[ds4] get_report id=%02x type=%d", report_id, report_type);
    return 0;  // v1: no feature reports; DS4Windows falls back to defaults
}

// ------------------------------------------------------------------ LED
static void led_update(uint32_t now) {
    static uint32_t last_toggle;
    static bool on;
    uint32_t period;
    if (sw2_ble_is_streaming()) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
        return;
    }
    const char *s = sw2_ble_state_name();
    if (strcmp(s, "off") == 0)              period = 2000;   // retry wait: slow
    else if (strcmp(s, "init") == 0 || strcmp(s, "switching") == 0 ||
             strcmp(s, "setup") == 0)       period = 100;    // busy: fast
    else                                    period = 500;    // scanning/connecting
    if (now - last_toggle >= period) {
        last_toggle = now;
        on = !on;
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
    }
}

// ------------------------------------------------------------------ main
int main(void) {
    set_sys_clock_khz(150000, true);

    log_init();
    tusb_init();

    LOG("sw2bridge v0.1 (Switch 2 Pro -> DS4, Pico 2 W)");

    watchdog_enable(4000, true);

    if (cyw43_arch_init()) {
        LOG("FATAL: cyw43 init failed");
        while (true) { watchdog_update(); sleep_ms(1000); }
    }
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);

    btstack_cyw43_init(cyw43_arch_async_context());
    sw2_ble_init();

    sw2_state_t s;
    while (true) {
        tud_task();
        watchdog_update();
        log_drain();
        uint32_t now = to_ms_since_boot(get_absolute_time());
        led_update(now);

        if (sw2_ble_take_state(&s)) {
            ds4_encode(&s, ds4_body);
            last_report_ms = now;
            if (tud_hid_n_ready(0)) {
                tud_hid_n_report(0, DS4_INPUT_REPORT_ID, ds4_body, DS4_INPUT_REPORT_BODY);
            }
        } else if (sw2_ble_is_streaming() && now - last_report_ms >= 8 &&
                   now - last_keepalive_ms >= 8 && tud_hid_n_ready(0)) {
            // periodic keepalive so hosts see a live controller
            tud_hid_n_report(0, DS4_INPUT_REPORT_ID, ds4_body, DS4_INPUT_REPORT_BODY);
            last_keepalive_ms = now;
        }

        cyw43_arch_poll();
        sleep_us(200);
    }
}
