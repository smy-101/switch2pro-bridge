#include "ds4.h"
#include <string.h>

static uint8_t ds4_counter;

static void put_u16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void put_i16(uint8_t *b, int16_t v)  { put_u16(b, (uint16_t)v); }

static uint8_t hat_from_state(const sw2_state_t *st) {
    bool up    = st->buttons & SW2_U3_UP;
    bool down  = st->buttons & SW2_U3_DOWN;
    bool left  = st->buttons & SW2_U3_LEFT;
    bool right = st->buttons & SW2_U3_RIGHT;
    if (up && right)       return 1;
    if (right && down)     return 3;
    if (down && left)      return 5;
    if (left && up)        return 7;
    if (up)    return 0;
    if (right) return 2;
    if (down)  return 4;
    if (left)  return 6;
    return 8;
}

void ds4_encode(const sw2_state_t *st, uint8_t body[DS4_INPUT_REPORT_BODY]) {
    memset(body, 0, DS4_INPUT_REPORT_BODY);

    // [0..3] LX LY RX RY (0..255, y down-positive — same orientation as Switch raw)
    body[0] = st->lx;
    body[1] = st->ly;
    body[2] = st->rx;
    body[3] = st->ry;

    // [4] dpad hat (low nibble) | face buttons (high nibble)
    uint8_t face = 0;
    if (st->buttons & SW2_U1_Y) face |= DS4_BTN_SQUARE;    // Nintendo Y (west)  -> square (west)
    if (st->buttons & SW2_U1_A) face |= DS4_BTN_CROSS;     // Nintendo A (south) -> cross  (south)
    if (st->buttons & SW2_U1_B) face |= DS4_BTN_CIRCLE;    // Nintendo B (east)  -> circle (east)
    if (st->buttons & SW2_U1_X) face |= DS4_BTN_TRIANGLE;  // Nintendo X (north) -> triangle(north)
    body[4] = (uint8_t)(hat_from_state(st) | face);

    // [5] L1 R1 L2 R2 share options L3 R3
    uint8_t btn = 0;
    if (st->buttons & SW2_U3_L)     btn |= DS4_BTN_L1;
    if (st->buttons & SW2_U1_R)     btn |= DS4_BTN_R1;
    if (st->buttons & SW2_U3_ZL)    btn |= DS4_BTN_L2;   // digital triggers
    if (st->buttons & SW2_U1_ZR)    btn |= DS4_BTN_R2;
    if (st->buttons & SW2_U2_MINUS) btn |= DS4_BTN_SHARE;
    if (st->buttons & SW2_U2_PLUS)  btn |= DS4_BTN_OPTIONS;
    if (st->buttons & SW2_U2_LS)    btn |= DS4_BTN_L3;
    if (st->buttons & SW2_U2_RS)    btn |= DS4_BTN_R3;
    body[5] = btn;

    // [6] counter<<2 | PS | touchpad-click
    uint8_t ps = (uint8_t)((ds4_counter++ & 0x3f) << 2);
    if (st->buttons & SW2_U2_HOME)    ps |= DS4_BTN_PS;
    if (st->buttons & SW2_U2_CAPTURE) ps |= DS4_BTN_TPAD;  // Capture -> touchpad click
    body[6] = ps;

    // [7..8] analog triggers (digital on Switch 2 Pro)
    body[7] = (st->buttons & SW2_U3_ZL) ? 255 : 0;
    body[8] = (st->buttons & SW2_U1_ZR) ? 255 : 0;

    // [9..10] timestamp counter
    put_u16(&body[9], ds4_counter * 1024u);

    // [11] battery status byte ("USB, charged" constant, as DS5_Bridge)
    body[11] = 0x09;

    // [12..17] gyro, [18..23] accel — v2 will pass Switch 2 motion through;
    // for now idle gyro and a resting accel (~1g on Z) so consumers stay sane.
    if (st->motion_valid) {
        // scales differ: SW2 common 40 rad/s full-scale vs DS4's ~2 rad/s unit.
        // Keep raw passthrough for v1 diagnostics; real calibration lands with motion support.
        put_i16(&body[12], st->gyro[0]);
        put_i16(&body[14], st->gyro[1]);
        put_i16(&body[16], st->gyro[2]);
        put_i16(&body[18], st->accel[0]);
        put_i16(&body[20], st->accel[1]);
        put_i16(&body[22], st->accel[2]);
    } else {
        put_i16(&body[22], -5023);   // ~1g on DS4 Z axis
    }

    // [29] battery level ("fully charged, USB")
    body[29] = 0x1a;
    // [32] status/plug info, [33] timestamp lsb
    body[32] = 0x01;
    body[33] = (uint8_t)(ds4_counter & 0xff);
    // [34..41] touchpad points stay "not touched"
}
