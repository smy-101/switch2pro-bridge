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
    body[3] = (uint8_t)((st->buttons >> 8) & 0x3f);   // U2: Minus Plus RS LS Home Capture
    body[4] = (uint8_t)((st->buttons >> 16) & 0xff);  // U3: Down Up Right Left SR_L SL_L L ZL

    // raw 12-bit sticks passthrough (host-side calibration handles the rest)
    pack_stick(&body[5], st->raw_lx, st->raw_ly);
    pack_stick(&body[8], st->raw_rx, st->raw_ry);

    // [11..62] IMU / trailing pad: zero until motion support (v2)
}
