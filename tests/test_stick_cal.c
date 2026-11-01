/* Host regression test for pro_stick_cal_blob(): checks the packing order against
 * SDL LoadStickCalibration() decoder. Run: tests/run.sh */
#include <stdio.h>
#include <string.h>
#include "sw2_decode.h"
#include "pro.h"

/* sw2_cal_l / sw2_cal_r live in sw2_decode.c, which is linked in. */

/* SDL_hidapi_switch.c decoding of one 3-byte packed pair. */
static void unpack(const uint8_t *p, unsigned *x, unsigned *y) {
    *x = (unsigned)(((p[1] << 8) & 0xF00) | p[0]);
    *y = (unsigned)((p[2] << 4) | (p[1] >> 4));
}

static int check(const char *what, const uint8_t *p, unsigned ex, unsigned ey) {
    unsigned x, y;
    unpack(p, &x, &y);
    int ok = (x == ex && y == ey);
    printf("%-14s %4u %4u  (expected %4u %4u)  %s\n",
           what, x, y, ex, ey, ok ? "ok" : "MISMATCH");
    return ok;
}

int main(void) {
    uint8_t blob[18];

    sw2_cal_l.neutral_x = 2048;   sw2_cal_l.neutral_y = 2010;
    sw2_cal_l.range_pos_x = 1300; sw2_cal_l.range_pos_y = 1250;
    sw2_cal_l.range_neg_x = 1400; sw2_cal_l.range_neg_y = 1350;
    sw2_cal_r.neutral_x = 1980;   sw2_cal_r.neutral_y = 2050;
    sw2_cal_r.range_pos_x = 1200; sw2_cal_r.range_pos_y = 1280;
    sw2_cal_r.range_neg_x = 1350; sw2_cal_r.range_neg_y = 1310;

    pro_stick_cal_blob(blob);

    int ok = 1;
    ok &= check("left max",     &blob[0],  1300, 1250);
    ok &= check("left center",  &blob[3],  2048, 2010);
    ok &= check("left min",     &blob[6],  1400, 1350);
    ok &= check("right center", &blob[9],  1980, 2050);
    ok &= check("right min",    &blob[12], 1350, 1310);
    ok &= check("right max",    &blob[15], 1200, 1280);

    /* A default-calibration controller must still report a sane center. */
    sw2_cal_set_defaults(&sw2_cal_l);
    sw2_cal_set_defaults(&sw2_cal_r);
    pro_stick_cal_blob(blob);
    ok &= check("default center", &blob[3], 2048, 2048);
    ok &= check("default max",    &blob[0], 2048, 2048);

    printf("%s\n", ok ? "ALL OK" : "FAILED");
    return ok ? 0 : 1;
}
