#include "sw2_decode.h"
#include <string.h>

sw2_stick_cal_t sw2_cal_l, sw2_cal_r;

void sw2_cal_set_defaults(sw2_stick_cal_t *cal) {
    cal->neutral_x = cal->neutral_y = 2048;
    cal->range_pos_x = cal->range_pos_y = 2048;
    cal->range_neg_x = cal->range_neg_y = 2048;
}

static uint16_t unx(const uint8_t *b) {  // 12-bit packed pair, PROTOCOL.md §52
    uint16_t x = (uint16_t)(b[0] | ((b[1] & 0x0f) << 8));
    uint16_t y = (uint16_t)((b[1] >> 4) | (b[2] << 4));
    // pack both into one return via caller; helper below instead
    (void)y; (void)x;
    return 0;
}

static void stick_unpack(const uint8_t *b, uint16_t *x, uint16_t *y) {
    *x = (uint16_t)(b[0] | ((b[1] & 0x0f) << 8));
    *y = (uint16_t)((b[1] >> 4) | (b[2] << 4));
}

bool sw2_cal_decode_factory(const uint8_t *b, sw2_stick_cal_t *cal) {
    // all-FF means absent
    bool all_ff = true;
    for (int i = 0; i < 9; i++) if (b[i] != 0xff) { all_ff = false; break; }
    if (all_ff) return false;
    uint16_t nx, ny, px, py, mx_, my_;
    stick_unpack(&b[0], &nx, &ny);
    stick_unpack(&b[3], &px, &py);
    stick_unpack(&b[6], &mx_, &my_);
    cal->neutral_x = nx; cal->neutral_y = ny;
    cal->range_pos_x = (uint16_t)(px > nx ? px - nx : 1);
    cal->range_pos_y = (uint16_t)(py > ny ? py - ny : 1);
    cal->range_neg_x = (uint16_t)(nx > mx_ ? nx - mx_ : 1);
    cal->range_neg_y = (uint16_t)(ny > my_ ? ny - my_ : 1);
    return true;
}

bool sw2_cal_decode_user(const uint8_t *b, sw2_stick_cal_t *cal) {
    if ((uint16_t)(b[0] | (b[1] << 8)) != SW2_USER_CAL_MAGIC) return false;
    return sw2_cal_decode_factory(&b[2], cal);
}

uint8_t sw2_cal_map(uint16_t raw, uint16_t neutral, uint16_t range_pos, uint16_t range_neg) {
    int32_t v;
    if (raw >= neutral) {
        int32_t d = (int32_t)raw - (int32_t)neutral;
        if (d > (int32_t)range_pos) d = range_pos;
        v = 128 + (d * 127) / (int32_t)(range_pos ? range_pos : 1);
    } else {
        int32_t d = (int32_t)neutral - (int32_t)raw;
        if (d > (int32_t)range_neg) d = range_neg;
        v = 128 - (d * 127) / (int32_t)(range_neg ? range_neg : 1);
    }
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

static uint32_t rd32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint16_t rd16(const uint8_t *b) {
    return (uint16_t)(b[0] | (b[1] << 8));
}

static int16_t rds16(const uint8_t *b) {
    return (int16_t)rd16(b);
}

void sw2_decode_common(const uint8_t *g, sw2_state_t *out) {
    memset(out, 0, sizeof(*out));
    out->counter = rd32(&g[SW2_COMMON_COUNTER]);
    out->buttons = rd32(&g[SW2_COMMON_BUTTONS]);

    stick_unpack(&g[SW2_COMMON_STICK_L], &out->raw_lx, &out->raw_ly);
    stick_unpack(&g[SW2_COMMON_STICK_R], &out->raw_rx, &out->raw_ry);
    out->lx = sw2_cal_map(out->raw_lx, sw2_cal_l.neutral_x, sw2_cal_l.range_pos_x, sw2_cal_l.range_neg_x);
    out->ly = sw2_cal_map(out->raw_ly, sw2_cal_l.neutral_y, sw2_cal_l.range_pos_y, sw2_cal_l.range_neg_y);
    out->rx = sw2_cal_map(out->raw_rx, sw2_cal_r.neutral_x, sw2_cal_r.range_pos_x, sw2_cal_r.range_neg_x);
    out->ry = sw2_cal_map(out->raw_ry, sw2_cal_r.neutral_y, sw2_cal_r.range_pos_y, sw2_cal_r.range_neg_y);

    out->batt_mv = rd16(&g[SW2_COMMON_BATT_VOLTAGE]);
    out->charging = g[SW2_COMMON_BATT_CHARGING];

    // motion block (PROTOCOL.md §36): accel then gyro, s16 LE
    out->accel[0] = rds16(&g[SW2_COMMON_ACCEL + 0]);
    out->accel[1] = rds16(&g[SW2_COMMON_ACCEL + 2]);
    out->accel[2] = rds16(&g[SW2_COMMON_ACCEL + 4]);
    out->gyro[0] = rds16(&g[SW2_COMMON_GYRO + 0]);
    out->gyro[1] = rds16(&g[SW2_COMMON_GYRO + 2]);
    out->gyro[2] = rds16(&g[SW2_COMMON_GYRO + 4]);
    out->motion_valid = true;
}

void sw2_decode_pro_native(const uint8_t *g, sw2_state_t *out) {
    memset(out, 0, sizeof(*out));
    out->counter = g[SW2_PRO_COUNTER];

    const uint8_t b0 = g[SW2_PRO_BTN0];
    const uint8_t b1 = g[SW2_PRO_BTN1];
    const uint8_t b2 = g[SW2_PRO_BTN2];
    uint32_t u = 0;
    if (b0 & SW2_PRO_BTN0_Y)     u |= SW2_U1_Y;
    if (b0 & SW2_PRO_BTN0_X)     u |= SW2_U1_X;
    if (b0 & SW2_PRO_BTN0_B)     u |= SW2_U1_B;
    if (b0 & SW2_PRO_BTN0_A)     u |= SW2_U1_A;
    if (b0 & SW2_PRO_BTN0_R)     u |= SW2_U1_R;
    if (b0 & SW2_PRO_BTN0_ZR)    u |= SW2_U1_ZR;
    if (b0 & SW2_PRO_BTN0_PLUS)  u |= SW2_U2_PLUS;
    if (b0 & SW2_PRO_BTN0_RS)    u |= SW2_U2_RS;
    if (b1 & SW2_PRO_BTN1_DOWN)  u |= SW2_U3_DOWN;
    if (b1 & SW2_PRO_BTN1_UP)    u |= SW2_U3_UP;
    if (b1 & SW2_PRO_BTN1_RIGHT) u |= SW2_U3_RIGHT;
    if (b1 & SW2_PRO_BTN1_LEFT)  u |= SW2_U3_LEFT;
    if (b1 & SW2_PRO_BTN1_L)     u |= SW2_U3_L;
    if (b1 & SW2_PRO_BTN1_ZL)    u |= SW2_U3_ZL;
    if (b1 & SW2_PRO_BTN1_MINUS) u |= SW2_U2_MINUS;
    if (b1 & SW2_PRO_BTN1_LS)    u |= SW2_U2_LS;
    if (b2 & SW2_PRO_BTN2_HOME)    u |= SW2_U2_HOME;
    if (b2 & SW2_PRO_BTN2_CAPTURE) u |= SW2_U2_CAPTURE;
    if (b2 & SW2_PRO_BTN2_GL)      u |= SW2_U4_GL;
    if (b2 & SW2_PRO_BTN2_GR)      u |= SW2_U4_GR;
    if (b2 & SW2_PRO_BTN2_C)       u |= SW2_U2_C;
    out->buttons = u;

    stick_unpack(&g[SW2_PRO_STICK_L], &out->raw_lx, &out->raw_ly);
    stick_unpack(&g[SW2_PRO_STICK_R], &out->raw_rx, &out->raw_ry);
    out->lx = sw2_cal_map(out->raw_lx, sw2_cal_l.neutral_x, sw2_cal_l.range_pos_x, sw2_cal_l.range_neg_x);
    out->ly = sw2_cal_map(out->raw_ly, sw2_cal_l.neutral_y, sw2_cal_l.range_pos_y, sw2_cal_l.range_neg_y);
    out->rx = sw2_cal_map(out->raw_rx, sw2_cal_r.neutral_x, sw2_cal_r.range_pos_x, sw2_cal_r.range_neg_x);
    out->ry = sw2_cal_map(out->raw_ry, sw2_cal_r.neutral_y, sw2_cal_r.range_pos_y, sw2_cal_r.range_neg_y);

    // native power byte (PROTOCOL.md §14)
    out->batt_mv = (uint16_t)(((g[SW2_PRO_POWER] >> 2) & 0x0f) * 55 + 3300); // rough, display only
    out->charging = (g[SW2_PRO_POWER] >> 1) & 1;
    out->motion_valid = false;  // native motion format deferred to v2
}
