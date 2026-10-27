//
// sw2bridge — Nintendo Switch 2 Pro Controller wireless bridge
// for Raspberry Pi Pico 2 W. Default host persona: Switch Pro (v1).
// See ../README.md and ../docs/BRINGUP.md.
//
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "pico/cyw43_arch.h"
#include "pico/btstack_cyw43.h"
#include "pico/flash.h"
#include "hardware/gpio.h"
#include "hardware/structs/ioqspi.h"
#include "hardware/watchdog.h"
#include "tusb.h"

#include "log.h"
#include "sw2_ble.h"
#include "sw2_protocol.h"
#include "ds4.h"
#include "pro.h"
#include "usb_descriptors.h"

static uint8_t pro_body[PRO_INPUT_REPORT_BODY];
static uint8_t ds4_body[DS4_INPUT_REPORT_BODY];
static uint32_t last_report_ms;
static uint32_t last_keepalive_ms;

// ------------------------------------------------------------------ BOOTSEL
// Read BOOTSEL via the QSPI CS pad trick (as DS5_Bridge); must not run while
// flash is being written — we only ever read, and we wrap in
// flash_safe_execute with interrupts driven off around the sample.
static void __no_inline_not_in_flash_func(bootsel_sample_cb)(void *param) {
    bool *pressed = (bool *)param;
    const uint CS_PIN_INDEX = 1;
    hw_write_masked(
        &ioqspi_hw->io[CS_PIN_INDEX].ctrl,
        GPIO_OVERRIDE_LOW << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
        IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
    for (int i = 0; i < 1000; ++i) __asm volatile("nop");
#if PICO_RP2350
    *pressed = !(sio_hw->gpio_hi_in & SIO_GPIO_HI_IN_QSPI_CSN_BITS);
#else
    *pressed = !(sio_hw->gpio_hi_in & (1u << CS_PIN_INDEX));
#endif
    hw_write_masked(
        &ioqspi_hw->io[CS_PIN_INDEX].ctrl,
        GPIO_OVERRIDE_NORMAL << IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_LSB,
        IO_QSPI_GPIO_QSPI_SS_CTRL_OEOVER_BITS);
}

static bool bootsel_pressed(void) {
    bool pressed = false;
    flash_safe_execute(bootsel_sample_cb, &pressed, 50);
    return pressed;
}

// Hold BOOTSEL >= 1.5s to toggle host persona (switch-pro <-> ds4).
static void bootsel_task(uint32_t now) {
    static bool was_down, fired;
    static uint32_t down_since;
    const bool down = bootsel_pressed();
    if (down && !was_down) {
        was_down = true;
        fired = false;
        down_since = now;
    } else if (down && was_down && !fired && now - down_since >= 1500) {
        fired = true;
        usb_persona_toggle();
        LOG("[usb] persona -> %s (re-enumerating)", usb_persona_name());
        usb_reenumerate();
    } else if (!down) {
        was_down = false;
    }
}

// ------------------------------------------------------------------ HID I/O
// Host -> bridge output reports. Persona-specific formats:
//   DS4: report 0x05 (rumble/LED)          — logged, translation in v2
//   Pro: reports 0x01/0x10/0x80 (commands) — logged, translation in v2
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    (void)itf;
    if (report_type == HID_REPORT_TYPE_OUTPUT || report_type == HID_REPORT_TYPE_INVALID) {
        if (usb_persona_get() == USB_PERSONA_PRO && bufsize >= 2) {
            // 0x01 = command+rumble, 0x10 = rumble only, 0x80 = USB bridge cmds
            LOG("[pro] out report id=%02x sub=%02x len=%u",
                report_id, buffer[1], bufsize);
        } else if (report_id == DS4_OUTPUT_REPORT_ID) {
            LOG("[ds4] output report 0x05: flags=%02x rumble R=%u L=%u led=%02x%02x%02x",
                buffer[1], bufsize > 4 ? buffer[4] : 0, bufsize > 5 ? buffer[5] : 0,
                bufsize > 6 ? buffer[6] : 0, bufsize > 7 ? buffer[7] : 0,
                bufsize > 8 ? buffer[8] : 0);
        } else {
            LOG("[hid] set_report id=%02x type=%d len=%u", report_id, report_type, bufsize);
        }
    } else {
        LOG("[hid] set_report id=%02x type=%d len=%u", report_id, report_type, bufsize);
    }
}

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)itf; (void)buffer; (void)reqlen;
    LOG("[hid] get_report id=%02x type=%d", report_id, report_type);
    return 0;  // v1: no feature reports; hosts fall back to defaults
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
    if (strcmp(s, "off") == 0)              period = 2000;
    else if (strcmp(s, "init") == 0 || strcmp(s, "switching") == 0 ||
             strcmp(s, "setup") == 0)       period = 100;
    else                                    period = 500;
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
    flash_safe_execute_core_init();

    LOG("sw2bridge v0.2 (Switch 2 Pro -> %s persona, Pico 2 W)", usb_persona_name());

    watchdog_enable(4000, true);

    if (cyw43_arch_init()) {
        LOG("FATAL: cyw43 init failed");
        while (true) { watchdog_update(); sleep_ms(1000); }
    }
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);

    btstack_cyw43_init(cyw43_arch_async_context());
    sw2_ble_init();

    uint32_t bootsel_last_poll = 0;
    sw2_state_t s;
    while (true) {
        tud_task();
        watchdog_update();
        log_drain();
        uint32_t now = to_ms_since_boot(get_absolute_time());
        led_update(now);
        if (now - bootsel_last_poll >= 10) {
            bootsel_last_poll = now;
            bootsel_task(now);
        }

        if (sw2_ble_take_state(&s)) {
            last_report_ms = now;
            if (usb_persona_get() == USB_PERSONA_PRO) {
                pro_encode(&s, pro_body);
                if (tud_hid_n_ready(0))
                    tud_hid_n_report(0, PRO_INPUT_REPORT_ID, pro_body, PRO_INPUT_REPORT_BODY);
            } else {
                ds4_encode(&s, ds4_body);
                if (tud_hid_n_ready(0))
                    tud_hid_n_report(0, DS4_INPUT_REPORT_ID, ds4_body, DS4_INPUT_REPORT_BODY);
            }
        } else if (sw2_ble_is_streaming() && now - last_report_ms >= 8 &&
                   now - last_keepalive_ms >= 8 && tud_hid_n_ready(0)) {
            // periodic keepalive so hosts see a live controller
            if (usb_persona_get() == USB_PERSONA_PRO)
                tud_hid_n_report(0, PRO_INPUT_REPORT_ID, pro_body, PRO_INPUT_REPORT_BODY);
            else
                tud_hid_n_report(0, DS4_INPUT_REPORT_ID, ds4_body, DS4_INPUT_REPORT_BODY);
            last_keepalive_ms = now;
        }

        cyw43_arch_poll();
        sleep_us(200);
    }
}
