#include "pro.h"
#include <string.h>

static uint8_t pro_timer;

static uint8_t battery_level_from_mv(uint16_t mv) {
    if (mv == 0) return 4;                       // unknown -> mid
    if (mv >= 4100) return 8;
    if (mv >= 3950) return 7;
    if (mv >= 3800) return 6;
    if (mv >= 3700) return 5;
    if (mv >= 3600) return 4;
    if (mv >= 3500) return 3;
    if (mv >= 3400) return 2;
    return 1;
}

static void pack_stick(uint8_t *b, uint16_t x, uint16_t y) {
    b[0] = (uint8_t)(x & 0xff);
    b[1] = (uint8_t)(((x >> 8) & 0x0f) | ((y & 0x0f) << 4));
    b[2] = (uint8_t)((y >> 4) & 0xff);
}

void pro_encode(const sw2_state_t *st, uint8_t body[PRO_INPUT_REPORT_BODY]) {
    memset(body, 0, PRO_INPUT_REPORT_BODY);

    body[0] = pro_timer++;

    // connection info high nibble | battery low nibble
    body[1] = (uint8_t)((PRO_CONN_USB << 4) | (battery_level_from_mv(st->batt_mv) & 0x0f));

    // unified button bytes map 1:1 (same bit order as Switch 1 standard report)
    body[2] = (uint8_t)(st->buttons & 0xff);          // U1: Y X B A SR_R SL_R R ZR
    // U2: Minus Plus RS LS Home Capture C. SDL only reads bit 6 (C) when the
    // device identifies as a Switch 2 controller, so keeping it is harmless for
    // the 0x2009 persona and lets a 0x2069 persona reuse this encoder.
    body[3] = (uint8_t)((st->buttons >> 8) & 0x7f);
    body[4] = (uint8_t)((st->buttons >> 16) & 0xff);  // U3: Down Up Right Left SR_L SL_L L ZL

    // raw 12-bit sticks passthrough (host-side calibration handles the rest)
    pack_stick(&body[5], st->raw_lx, st->raw_ly);
    pack_stick(&body[8], st->raw_rx, st->raw_ry);

    // [11..62] IMU / trailing pad: zero until motion support (v2)
}

void pro_stick_cal_blob(uint8_t out[18]) {
    // SDL decodes left as {max, center, min} and right as {center, min, max},
    // where max/min are distances measured from the center.
    pack_stick(&out[0],  sw2_cal_l.range_pos_x, sw2_cal_l.range_pos_y);   // left max
    pack_stick(&out[3],  sw2_cal_l.neutral_x,   sw2_cal_l.neutral_y);     // left center
    pack_stick(&out[6],  sw2_cal_l.range_neg_x, sw2_cal_l.range_neg_y);   // left min
    pack_stick(&out[9],  sw2_cal_r.neutral_x,   sw2_cal_r.neutral_y);     // right center
    pack_stick(&out[12], sw2_cal_r.range_neg_x, sw2_cal_r.range_neg_y);   // right min
    pack_stick(&out[15], sw2_cal_r.range_pos_x, sw2_cal_r.range_pos_y);   // right max
}
