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

#endif
