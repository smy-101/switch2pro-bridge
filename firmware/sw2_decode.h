#ifndef SW2_DECODE_H
#define SW2_DECODE_H
#include <stdint.h>
#include <stdbool.h>
#include "sw2_protocol.h"

typedef struct __packed {
    uint16_t neutral_x, neutral_y;
    uint16_t range_pos_x, range_pos_y;   // neutral .. max
    uint16_t range_neg_x, range_neg_y;   // min .. neutral
} sw2_stick_cal_t;

typedef struct {
    // unified button mask (SW2_U* bits)
    uint32_t buttons;
    // normalized raw sticks, 12-bit each
    uint16_t raw_lx, raw_ly, raw_rx, raw_ry;
    // calibrated 0..255 (x: left->right, y: up->down)
    uint8_t lx, ly, rx, ry;
    // battery
    uint16_t batt_mv;
    uint8_t  charging;
    // motion (raw common-report values, see PROTOCOL.md §36/37)
    int16_t accel[3], gyro[3];
    uint32_t counter;
    bool motion_valid;
} sw2_state_t;

// Parse a 63-byte common report 0x05 GATT value into state (applies calibration).
void sw2_decode_common(const uint8_t *gatt63, sw2_state_t *out);

// Parse a 63-byte native Pro 0x09 GATT value into unified state.
void sw2_decode_pro_native(const uint8_t *gatt63, sw2_state_t *out);

// Calibration storage; call before decoding reports.
extern sw2_stick_cal_t sw2_cal_l, sw2_cal_r;
void sw2_cal_set_defaults(sw2_stick_cal_t *cal);
// Decode a 9-byte factory calibration blob (PROTOCOL.md §11).
bool sw2_cal_decode_factory(const uint8_t *b9, sw2_stick_cal_t *cal);
// Decode an 11-byte user calibration blob; false if magic absent.
bool sw2_cal_decode_user(const uint8_t *b11, sw2_stick_cal_t *cal);
// Map a raw 12-bit axis value through calibration to 0..255.
uint8_t sw2_cal_map(uint16_t raw, uint16_t neutral, uint16_t range_pos, uint16_t range_neg);

#endif
