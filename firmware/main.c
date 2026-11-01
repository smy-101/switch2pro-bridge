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
#include "config.h"
#include "cmd.h"
#include "sw2_ble.h"
#include "sw2_protocol.h"
#include "ds4.h"
#include "pro.h"
#include "usb_descriptors.h"

static uint8_t pro_body[PRO_INPUT_REPORT_BODY];
static uint8_t ds4_body[DS4_INPUT_REPORT_BODY];
static uint32_t last_report_ms;
static uint32_t last_keepalive_ms;
static uint32_t usb_tx_count, usb_fail_count, usb_diag_ms;

static void hid_send(uint8_t report_id, const uint8_t *body, uint16_t len) {
    if (!tud_hid_n_ready(0)) {
        usb_fail_count++;
        return;
    }
    if (tud_hid_n_report(0, report_id, body, len)) {
        if (usb_tx_count == 0)
            LOG("[usb] first HID report id=%02x body=%02x %02x %02x %02x %02x %02x %02x %02x",
                report_id, body[0], body[1], body[2], body[3],
                body[4], body[5], body[6], body[7]);
        usb_tx_count++;
    } else {
        usb_fail_count++;
    }
}

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
        // Keep flash in sync so the choice survives replug/reboot —
        // same storage the web config page writes (config.h).
        config_body_t b = *config_get();
        b.persona = (uint8_t)usb_persona_get();
        (void)config_set((const uint8_t *)&b, sizeof b);
        (void)config_save();
        LOG("[usb] persona -> %s (saved, re-enumerating)", usb_persona_name());
        usb_reenumerate();
    } else if (!down) {
        was_down = false;
    }
}

// ------------------------------------------------------ Switch Pro USB handshake
// Steam (and the real Pro firmware) performs a USB handshake before the
// controller is considered usable:
//   host -> 0x80/0x01 IDENTIFY        device -> 0x81/0x01 type + MAC
//   host -> 0x80/0x02 HANDSHAKE       device -> 0x81/0x02
//   host -> 0x80/0x03 BAUD_RATE       device -> 0x81/0x03
//   host -> 0x80/0x04 DISABLE_TIMEOUT device -> 0x30/0x04 (stream may start)
// Without these replies Steam retries forever and never lists the controller.
static uint8_t pro_hs_report_id;
static uint8_t pro_hs_body[PRO_INPUT_REPORT_BODY];
static volatile bool pro_hs_pending;

// Switch 1 Pro subcommand replies (input report 0x21). SDL/Steam reads
// device info (0x02), SPI calibration (0x10), input mode (0x03), lights,
// IMU and vibration enable before it will open the pad.
static const uint8_t pro_dev_info[12] = {
    0x03, 0x49,             // firmware 3.49
    0x03,                   // Pro Controller
    0x02,                   // unknown
    0x01, 0x00, 0x00, 0x00, 0x00, 0x02,  // MAC 02:00:00:00:00:01 reversed
    0x01,                   // unknown
    0x02,                   // stored colors
};

static void pro_switch_subcommand(uint8_t sub, const uint8_t *params,
                                  uint16_t plen) {
    memset(pro_hs_body, 0, sizeof(pro_hs_body));
    memcpy(pro_hs_body, pro_body, 11);   // counter/battery/buttons/sticks
    pro_hs_body[11] = 0;                 // vibration code
    uint8_t *ack  = &pro_hs_body[12];
    uint8_t *cmd  = &pro_hs_body[13];
    uint8_t *data = &pro_hs_body[14];
    *cmd = sub;

    switch (sub) {
    case 0x02:                           // REQUEST_DEVICE_INFO
        *ack = 0x82;
        memcpy(data, pro_dev_info, sizeof(pro_dev_info));
        break;
    case 0x10: {                         // SPI_FLASH_READ
        *ack = 0x90;
        if (plen >= 5) {
            const uint32_t addr = (uint32_t)params[0] |
                                  ((uint32_t)params[1] << 8) |
                                  ((uint32_t)params[2] << 16) |
                                  ((uint32_t)params[3] << 24);
            memcpy(data, params, 5);     // echo address + length
            memset(data + 5, 0xff, 44);  // default: uncalibrated
            if (addr == PRO_SPI_FACTORY_STICK_CAL)
                pro_stick_cal_blob(&data[5]);   // real sticks, real range
        } else {
            *ack = 0x80;
        }
        break;
    }
    case 0x03:                           // SET_INPUT_REPORT_MODE
        *ack = 0x80;
        data[0] = plen ? params[0] : 0x30;
        break;
    case 0x30:                           // SET_PLAYER_LIGHTS
        *ack = 0x80;
        data[0] = 0x03;
        // Steam assigns a player slot with this subcommand; mirror the same
        // 4-LED bitmask onto the controller so its own LEDs follow.
        if (plen >= 1) sw2_ble_set_player_led(params[0]);
        break;
    case 0x50:                           // GET_VOLTAGE
        *ack = 0xd0;
        data[0] = 0x83;
        data[1] = 0x06;
        break;
    default:                             // generic ack
        *ack = 0x80;
        if (plen) data[0] = params[0];
        break;
    }
    pro_hs_report_id = 0x21;
    pro_hs_pending = true;
}

static void pro_switch_handshake(uint8_t sub) {
    memset(pro_hs_body, 0, sizeof(pro_hs_body));
    pro_hs_report_id = 0x81;
    switch (sub) {
    case 0x01: {                       // IDENTIFY
        static const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
        pro_hs_body[0] = 0x01;
        pro_hs_body[1] = 0x00;
        pro_hs_body[2] = 0x03;         // Pro Controller
        for (int i = 0; i < 6; i++) pro_hs_body[3 + i] = mac[5 - i];
        break;
    }
    case 0x02:                         // HANDSHAKE
    case 0x03:                         // BAUD_RATE
        pro_hs_body[0] = sub;
        break;
    case 0x04:                         // DISABLE_USB_TIMEOUT
    case 0x05:                         // ENABLE_USB_TIMEOUT
        pro_hs_report_id = 0x30;
        pro_hs_body[0] = sub;
        break;
    default:
        return;
    }
    pro_hs_pending = true;
}

// ------------------------------------------------------------------ HID I/O
// Host -> bridge output reports. Persona-specific formats:
//   DS4: report 0x05 (rumble/LED)          — logged, translation in v2
//   Pro: reports 0x01/0x10/0x80 (commands) — logged, translation in v2
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    (void)itf;
    if ((report_type == HID_REPORT_TYPE_FEATURE ||
         report_type == HID_REPORT_TYPE_INVALID) &&
        bridge_cmd_is_mine(report_id)) {
        bridge_cmd_set(report_id, buffer, bufsize);
        return;
    }
    if (report_type == HID_REPORT_TYPE_OUTPUT || report_type == HID_REPORT_TYPE_INVALID) {
        if (usb_persona_get() == USB_PERSONA_PRO && bufsize >= 1) {
            // TinyUSB may pass the report ID either in report_id or as
            // buffer[0]; the working ESP32 emulator relies on buffer[0].
            uint8_t cfg_sub = 0;
            if (buffer[0] == 0x80 && bufsize >= 2)
                cfg_sub = buffer[1];
            else if (report_id == 0x80)
                cfg_sub = buffer[0];
            if (cfg_sub) {
                LOG("[pro] cfg sub=%02x len=%u", cfg_sub, bufsize);
                pro_switch_handshake(cfg_sub);
                return;
            }

            // Haptics: SDL puts the classic 8-byte HD-rumble block at [2..9]
            // of output report 0x10 (rumble only) and 0x01 (rumble+subcommand).
            if (bufsize >= 10 && (buffer[0] == 0x10 || buffer[0] == 0x01))
                sw2_ble_set_rumble(&buffer[2]);

            // Output report 0x01: rumble + subcommand at byte 10.
            uint8_t sub = 0;
            const uint8_t *params = NULL;
            uint16_t plen = 0;
            bool have_sub = false;
            if (report_id == 0x01 && bufsize >= 10) {
                sub = buffer[9];
                params = &buffer[10];
                plen = (uint16_t)(bufsize - 10);
                have_sub = true;
            } else if (buffer[0] == 0x01 && bufsize >= 11) {
                sub = buffer[10];
                params = &buffer[11];
                plen = (uint16_t)(bufsize - 11);
                have_sub = true;
            }
            if (have_sub) {
                LOG("[pro] sub=%02x len=%u", sub, bufsize);
                pro_switch_subcommand(sub, params, plen);
                return;
            }
        }
        if (usb_persona_get() == USB_PERSONA_PRO && bufsize >= 2) {
            // 0x01 = command+rumble, 0x10 = rumble only, 0x80 = USB bridge cmds
            LOG("[pro] out report id=%02x sub=%02x len=%u",
                report_id, buffer[1], bufsize);
        } else if ((report_id == DS4_OUTPUT_REPORT_ID ||
                    (bufsize >= 2 && buffer[0] == DS4_OUTPUT_REPORT_ID)) &&
                   bufsize >= 2) {
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
    (void)itf;
    if (report_type == HID_REPORT_TYPE_FEATURE && bridge_cmd_is_mine(report_id)) {
        return bridge_cmd_get(report_id, buffer, reqlen);
    }
    if (report_type == HID_REPORT_TYPE_INVALID && bridge_cmd_is_mine(report_id)) {
        // Some hosts (and the WebHID shim on some builds) probe with an
        // unspecified report type; answer feature-style regardless.
        return bridge_cmd_get(report_id, buffer, reqlen);
    }
    LOG("[hid] get_report id=%02x type=%d", report_id, report_type);
    return 0;  // other feature reports unimplemented; hosts fall back
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
    config_init();                       // persona from flash before USB enum
    usb_persona_set(config_get()->persona);
    tusb_init();
    flash_safe_execute_core_init();

    LOG("%s (Switch 2 Pro -> %s persona, Pico 2 W)",
        SW2BRIDGE_VERSION, usb_persona_name());

    if (cyw43_arch_init()) {
        LOG("FATAL: cyw43 init failed");
        while (true) { watchdog_update(); sleep_ms(1000); }
    }
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, false);

    btstack_cyw43_init(cyw43_arch_async_context());
    sw2_ble_init();

    // Watchdog arms AFTER the slow cyw43/btstack bring-up (WL firmware load
    // can stretch a fixed 4s budget on cold boot); the main loop feeds it.
    // A hang *inside* init is no longer watchdog-caught — accepted tradeoff.
    watchdog_enable(4000, true);

    uint32_t bootsel_last_poll = 0;
    sw2_state_t s;

    // Neutral 0x30 report used before the BLE link delivers real state.
    sw2_state_t neutral = {0};
    neutral.raw_lx = neutral.raw_ly = neutral.raw_rx = neutral.raw_ry = 2048;
    pro_encode(&neutral, pro_body);

    while (true) {
        tud_task();
        if (pro_hs_pending && tud_hid_n_ready(0)) {
            pro_hs_pending = false;
            if (tud_hid_n_report(0, pro_hs_report_id, pro_hs_body,
                                 PRO_INPUT_REPORT_BODY)) {
                uint8_t what = (pro_hs_report_id == 0x21)
                                   ? pro_hs_body[13] : pro_hs_body[0];
                LOG("[usb] pro reply %02x/%02x", pro_hs_report_id, what);
            } else {
                pro_hs_pending = true;
            }
        }
        watchdog_update();
        log_drain();
        (void)bridge_cmd_poll();   // deferred re-enumeration from web config
        uint32_t now = to_ms_since_boot(get_absolute_time());
        led_update(now);
        if (now - bootsel_last_poll >= 10) {
            bootsel_last_poll = now;
            bootsel_task(now);
        }

        sw2_ble_poll();
        if (sw2_ble_take_state(&s)) {
            last_report_ms = now;
            if (usb_persona_get() == USB_PERSONA_PRO) {
                pro_encode(&s, pro_body);
                hid_send(PRO_INPUT_REPORT_ID, pro_body, PRO_INPUT_REPORT_BODY);
            } else {
                ds4_encode(&s, ds4_body);
                hid_send(DS4_INPUT_REPORT_ID, ds4_body, DS4_INPUT_REPORT_BODY);
            }
        } else if (usb_persona_get() == USB_PERSONA_PRO &&
                   now - last_keepalive_ms >= 16) {
            // The real Pro streams 0x30 from power-on. Steam expects this
            // after the USB handshake, even before the BLE link is up.
            hid_send(PRO_INPUT_REPORT_ID, pro_body, PRO_INPUT_REPORT_BODY);
            last_keepalive_ms = now;
        } else if (sw2_ble_is_streaming() && now - last_report_ms >= 8 &&
                   now - last_keepalive_ms >= 8) {
            // DS4 keepalive
            hid_send(DS4_INPUT_REPORT_ID, ds4_body, DS4_INPUT_REPORT_BODY);
            last_keepalive_ms = now;
        }

        if (now - usb_diag_ms >= 1000) {
            usb_diag_ms = now;
            LOG("[usb] tx=%lu fail=%lu mounted=%d ready=%d",
                (unsigned long)usb_tx_count, (unsigned long)usb_fail_count,
                tud_mounted() ? 1 : 0, tud_hid_n_ready(0) ? 1 : 0);
        }

        cyw43_arch_poll();
        sleep_us(200);
    }
}
