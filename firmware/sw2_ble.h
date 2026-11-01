#ifndef SW2_BLE_H
#define SW2_BLE_H
#include <stdint.h>
#include <stdbool.h>
#include "sw2_decode.h"

void sw2_ble_init(void);                 // call after cyw43_arch_init
bool sw2_ble_take_state(sw2_state_t *out); // consume latest decoded report (true = new)
bool sw2_ble_is_streaming(void);
const char *sw2_ble_state_name(void);
uint16_t sw2_ble_peer_pid(void);
bool sw2_ble_use_native_reports(void);

// Host -> controller haptics. `usb_rumble8` is the 8-byte classic HD-rumble
// block SDL puts at bytes [2..9] of its USB output reports 0x10 / 0x01.
void sw2_ble_set_rumble(const uint8_t *usb_rumble8);
// Mirror the host's player-light pattern onto the controller's LEDs.
void sw2_ble_set_player_led(uint8_t mask);
// Sink for deferred BLE work (haptics arming, LED writes). Main loop only:
// BTstack run-loop state must never be touched from the USB callbacks.
void sw2_ble_poll(void);

// Vendor status channel (cmd.h report 0xF9).
uint8_t sw2_ble_state_code(void);               // 0 off .. 5 streaming
bool sw2_ble_battery(uint16_t *mv, uint8_t *charging);  // false = no report yet

#endif
