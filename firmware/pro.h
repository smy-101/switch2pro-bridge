#ifndef PRO_H
#define PRO_H
#include <stdint.h>
#include "sw2_decode.h"

// Switch Pro (v1) wired USB persona.
//
// Input report 0x30 (48 bytes total; 47-byte body, TinyUSB prepends the ID):
//   [0] timer (u8++)
//   [1] battery/conn: high nibble connection info (0xE = USB), low = battery 0..8
//   [2] buttons1: Y X B A SR_R SL_R R ZR        (== Switch 2 unified U1 byte)
//   [3] buttons2: Minus Plus RS LS Home Capture - -  (== U2, C dropped)
//   [4] buttons3: Down Up Right Left SR_L SL_L L ZL (== U3 byte)
//   [5..10] sticks, packed 12-bit X/Y, raw passthrough
//   [11..46] IMU 3x(accel+gyro) s16LE — zero until motion support lands
//
// Sources: dekuNukem/Nintendo_Switch_Reverse_Engineering bluetooth_hid_notes.md,
// churunfa/SwitchProControllerEsp32S3 USB emulation notes.
#define PRO_INPUT_REPORT_ID    0x30
#define PRO_INPUT_REPORT_TOTAL 48
#define PRO_INPUT_REPORT_BODY  47
// USB wired connection info nibble
#define PRO_CONN_USB 0x0E

void pro_encode(const sw2_state_t *st, uint8_t body[PRO_INPUT_REPORT_BODY]);

#endif
