#ifndef PRO_H
#define PRO_H
#include <stdint.h>
#include "sw2_decode.h"

// Switch Pro (v1) wired USB persona.
//
// Input report 0x30 — 64 bytes on the wire (report id + 63-byte body), matching
// both the real controller and the 63-byte body declared in the report
// descriptor (52 trailing const bytes). We zero-fill [11..62]; hosts read the
// raw layout by offset (Steam/SDL/hid-nintendo), padding included:
//   [0] timer (u8++)
//   [1] battery/conn: high nibble connection info (0xE = USB), low = battery 0..8
//   [2] buttons1: Y X B A SR_R SL_R R ZR        (== Switch 2 unified U1 byte)
//   [3] buttons2: Minus Plus RS LS Home Capture - -  (== U2, C dropped)
//   [4] buttons3: Down Up Right Left SR_L SL_L L ZL (== U3 byte)
//   [5..10] sticks, packed 12-bit X/Y, raw passthrough
//   [11..62] IMU 3x(accel+gyro) s16LE — zero until motion support lands
//
// Sources: dekuNukem/Nintendo_Switch_Reverse_Engineering bluetooth_hid_notes.md,
// churunfa/SwitchProControllerEsp32S3 USB emulation notes.
#define PRO_INPUT_REPORT_ID    0x30
#define PRO_INPUT_REPORT_TOTAL 64
#define PRO_INPUT_REPORT_BODY  63
// USB wired connection info nibble
#define PRO_CONN_USB 0x0E

void pro_encode(const sw2_state_t *st, uint8_t body[PRO_INPUT_REPORT_BODY]);

// Factory stick calibration lives here in the Switch 1 SPI map. Responding with
// a synthesized blob is what lets Steam/SDL calibrate the sticks; answering
// 0xFF makes SDL fall back to center 2048 / range 1433, which saturates the
// physical travel at ~85%.
#define PRO_SPI_FACTORY_STICK_CAL 0x603Du

// Left 9 bytes + right 9 bytes, three packed 12-bit (x, y) pairs each.
void pro_stick_cal_blob(uint8_t out[18]);

#endif
