#ifndef DS4_H
#define DS4_H
#include <stdint.h>
#include <stdbool.h>
#include "sw2_decode.h"

#define DS4_INPUT_REPORT_ID    0x01
#define DS4_INPUT_REPORT_TOTAL 64   // includes report id
#define DS4_INPUT_REPORT_BODY  63
#define DS4_OUTPUT_REPORT_ID   0x05

// DS4 button constants (layout as used by DS4Windows; matches DS5_Bridge).
#define DS4_BTN_SQUARE   0x10
#define DS4_BTN_CROSS    0x20
#define DS4_BTN_CIRCLE   0x40
#define DS4_BTN_TRIANGLE 0x80
#define DS4_BTN_L1 0x01
#define DS4_BTN_R1 0x02
#define DS4_BTN_L2 0x04
#define DS4_BTN_R2 0x08
#define DS4_BTN_SHARE  0x10
#define DS4_BTN_OPTIONS 0x20
#define DS4_BTN_L3 0x40
#define DS4_BTN_R3 0x80
#define DS4_BTN_PS 0x01
#define DS4_BTN_TPAD 0x02

// Encode a Switch 2 controller state into a 63-byte DS4 report body
// (caller sends with report id 0x01 prepended by TinyUSB).
// Nintendo face layout is translated to Xbox/DS4 layout:
//   SW2 A(0x08 bit) -> cross, B -> circle, X -> triangle, Y -> square.
void ds4_encode(const sw2_state_t *st, uint8_t body[DS4_INPUT_REPORT_BODY]);

#endif
