#ifndef SW2_PROTOCOL_H
#define SW2_PROTOCOL_H
//
// Switch 2 controller wire protocol constants.
//
// Source of truth: refs/linux-switch2/PROTOCOL.md (novakpetya/linux-switch2),
// cross-checked against Valve's drivers/hid/hid-nintendo.c (refs/hid-nintendo-valve.c).
// All BLE GATT values carry the report WITHOUT the HID report-ID byte.
//
#include <stdint.h>

// ---------------------------------------------------------------- identities
#define SW2_USB_VID                 0x057e
#define SW2_PID_JOYCON2_RIGHT       0x2066
#define SW2_PID_JOYCON2_LEFT        0x2067
#define SW2_PID_PRO_CONTROLLER      0x2069
#define SW2_PID_GAMECUBE            0x2073

// BLE advertisement manufacturer data (company id 0x0553, little-endian 53 05):
//   01 00 03 7e 05 <pid LE16>
#define SW2_ADV_COMPANY_ID          0x0553
static const uint8_t SW2_ADV_MAGIC[5] = {0x01, 0x00, 0x03, 0x7e, 0x05};

// ------------------------------------------------------------ connection
// PROTOCOL.md §3: interval 6 (=7.5 ms) fixed, latency 0, supervision 42 (420 ms).
// Wrong parameters degrade motion delivery severely.
#define SW2_CONN_INTERVAL_MIN       6
#define SW2_CONN_INTERVAL_MAX       6
#define SW2_CONN_LATENCY            0
#define SW2_CONN_SUPERVISION        42

// ---------------------------------------------------------------- GATT db
// Nintendo controller service (primary): ab7de9be-89fe-49ad-828f-118f09df7fd0
// Attribute handles are fixed on current firmware (bluez-switch2 seeds them).
#define SW2_HANDLE_BOOTSTRAP_SERVICE        0x0001

#define SW2_HANDLE_COMMON_INPUT_VALUE       0x000a  // UUID ...7fd2, report 0x05
#define SW2_HANDLE_COMMON_INPUT_CCC         0x000b
#define SW2_HANDLE_NATIVE_INPUT_VALUE       0x000e  // Pro UUID 7492866c-ec3e-4619-8258-32755ffcc0f8, report 0x09
#define SW2_HANDLE_NATIVE_INPUT_CCC         0x000f
#define SW2_HANDLE_VIBRATION_VALUE          0x0012  // WRITE WITHOUT RESPONSE
#define SW2_HANDLE_COMMAND_VALUE            0x0014  // UUID 649d4ac9-8eb7-4e6c-af44-1ea54fe5f005
#define SW2_HANDLE_COMMAND_REPLY_VALUE      0x001a  // UUID c765a961-d9d8-4d36-a20a-5315b111836a
#define SW2_HANDLE_COMMAND_REPLY_CCC        0x001b

static const uint8_t SW2_UUID_SERVICE[16] = {          // little-endian, as gatt_client wants it
    0xd0, 0x7f, 0xdf, 0x09, 0x8f, 0x11, 0x8f, 0x82,
    0xad, 0x49, 0xfe, 0x89, 0xbe, 0xe9, 0x7d, 0xab};

// ---------------------------------------------------------------- reports
#define SW2_REPORT_ID_JOYCON_L      0x07
#define SW2_REPORT_ID_JOYCON_R      0x08
#define SW2_REPORT_ID_PRO_NATIVE    0x09
#define SW2_REPORT_ID_COMMON        0x05
#define SW2_GATT_REPORT_LEN         63      // all native/common GATT values are 63 bytes

// common report 0x05, GATT offsets (PROTOCOL.md §29)
#define SW2_COMMON_COUNTER          0x00    // u32
#define SW2_COMMON_BUTTONS          0x04    // 4 bytes
#define SW2_COMMON_STICK_L          0x0a    // 3 bytes, packed 12-bit X/Y
#define SW2_COMMON_STICK_R          0x0d
#define SW2_COMMON_BATT_VOLTAGE     0x1f    // u16 mV
#define SW2_COMMON_BATT_CHARGING    0x21
#define SW2_COMMON_MOTION           0x2a    // 18 bytes
#define SW2_COMMON_ACCEL            0x30    // 3 x s16
#define SW2_COMMON_GYRO             0x36    // 3 x s16

// common-report button bits (PROTOCOL.md §30) — "unified" format
#define SW2_U1_Y        (1u << 0)
#define SW2_U1_X        (1u << 1)
#define SW2_U1_B        (1u << 2)
#define SW2_U1_A        (1u << 3)
#define SW2_U1_SR_R     (1u << 4)
#define SW2_U1_SL_R     (1u << 5)
#define SW2_U1_R        (1u << 6)
#define SW2_U1_ZR       (1u << 7)
#define SW2_U2_MINUS    (1u << 8)
#define SW2_U2_PLUS     (1u << 9)
#define SW2_U2_RS       (1u << 10)
#define SW2_U2_LS       (1u << 11)
#define SW2_U2_HOME     (1u << 12)
#define SW2_U2_CAPTURE  (1u << 13)
#define SW2_U2_C        (1u << 14)
#define SW2_U3_DOWN     (1u << 16)
#define SW2_U3_UP       (1u << 17)
#define SW2_U3_RIGHT    (1u << 18)
#define SW2_U3_LEFT     (1u << 19)
#define SW2_U3_SR_L     (1u << 20)
#define SW2_U3_SL_L     (1u << 21)
#define SW2_U3_L        (1u << 22)
#define SW2_U3_ZL       (1u << 23)
#define SW2_U4_GR       (1u << 24)
#define SW2_U4_GL       (1u << 25)

// native Pro report 0x09, GATT offsets = USB HID offsets minus report-id byte
// (USB layout from Valve hid-nintendo.c: buttons [3..5], sticks [6..8]/[9..11])
#define SW2_PRO_COUNTER          0x00
#define SW2_PRO_POWER            0x01
#define SW2_PRO_BTN0             0x02   // B A Y X R ZR PLUS RS  (Valve NS2_BTNR_*)
#define SW2_PRO_BTN1             0x03   // DOWN RIGHT LEFT UP L ZL MINUS LS (NS2_BTNL_*)
#define SW2_PRO_BTN2             0x04   // HOME CAPTURE GL GR C ... (NS2_BTN3_* + PRO bits)
#define SW2_PRO_STICK_L          0x05   // 3 bytes packed 12-bit
#define SW2_PRO_STICK_R          0x08

#define SW2_PRO_BTN0_B     (1u << 0)
#define SW2_PRO_BTN0_A     (1u << 1)
#define SW2_PRO_BTN0_Y     (1u << 2)
#define SW2_PRO_BTN0_X     (1u << 3)
#define SW2_PRO_BTN0_R     (1u << 4)
#define SW2_PRO_BTN0_ZR    (1u << 5)
#define SW2_PRO_BTN0_PLUS  (1u << 6)
#define SW2_PRO_BTN0_RS    (1u << 7)
#define SW2_PRO_BTN1_DOWN   (1u << 0)
#define SW2_PRO_BTN1_RIGHT  (1u << 1)
#define SW2_PRO_BTN1_LEFT   (1u << 2)
#define SW2_PRO_BTN1_UP     (1u << 3)
#define SW2_PRO_BTN1_L      (1u << 4)
#define SW2_PRO_BTN1_ZL     (1u << 5)
#define SW2_PRO_BTN1_MINUS  (1u << 6)
#define SW2_PRO_BTN1_LS     (1u << 7)
#define SW2_PRO_BTN2_HOME    (1u << 0)
#define SW2_PRO_BTN2_CAPTURE (1u << 1)
// linux-switch2 switch2.h: NS2_BTN_PRO_GR = BIT(2), NS2_BTN_PRO_GL = BIT(3).
// (Bit 2 is "the side's rear button" on Joy-Con reports, which is GR on the
// right unit and GL on the left one; on the Pro it is always GR.)
#define SW2_PRO_BTN2_GR      (1u << 2)
#define SW2_PRO_BTN2_GL      (1u << 3)
#define SW2_PRO_BTN2_C       (1u << 4)

// ---------------------------------------------------------------- commands
// 8-byte command header: [cmd, dir, transport, subcmd, 0, payload_len, 0, 0]
// Host requests use dir=0x91, transport: 0x00 USB / 0x01 Bluetooth.
#define SW2_CMD_DIR_HOST     0x91
#define SW2_CMD_TRANSPORT_BT 0x01

#define SW2_CMD_NFC          0x01
#define SW2_CMD_FLASH        0x02
#define SW2_CMD_INIT         0x03
#define SW2_CMD_BLE_SETUP1   0x07
#define SW2_CMD_GRIP         0x08
#define SW2_CMD_LED          0x09
#define SW2_CMD_VIBRATION    0x0a
#define SW2_CMD_BATTERY      0x0b
#define SW2_CMD_FEATSEL      0x0c
#define SW2_CMD_FW_INFO      0x10
#define SW2_CMD_BLE_MOTION   0x11
#define SW2_CMD_BLE_SETUP2   0x16
#define SW2_CMD_BLE_PAIR     0x15

#define SW2_SUBCMD_FLASH_READ          0x04
#define SW2_SUBCMD_INIT_SELECT_REPORT  0x0a
#define SW2_SUBCMD_INIT_ENABLE         0x0d
#define SW2_SUBCMD_FEATSEL_SET         0x02
#define SW2_SUBCMD_FEATSEL_ENABLE      0x04
#define SW2_SUBCMD_LED_SET             0x07
#define SW2_SUBCMD_FW_INFO_GET         0x01

// feature mask bits (PROTOCOL.md §7)
#define SW2_FEATURE_BUTTONS   0x01
#define SW2_FEATURE_ANALOG    0x02
#define SW2_FEATURE_IMU       0x04
#define SW2_FEATURE_OPTICAL   0x10   // Joy-Con only
#define SW2_FEATURE_RUMBLE    0x20
#define SW2_FEATURE_MAGNETO   0x80

// Two different masks, in this order, exactly as linux-switch2 does it
// (switch2-init.c): the measured BLE transport preamble sends 0x94, then the
// ordinary controller feature set 0x27 (the same value a controller enabled
// over USB uses). Do not blend them into 0x2f — 0x08 is not a feature bit.
#define SW2_FEATURE_BLE_PREAMBLE     0x94   // IMU | optical | magneto
#define SW2_FEATURE_PRO_CONTROLLER   0x27   // buttons | analog | IMU | rumble

// ---------------------------------------------------------------- flash
#define SW2_FLASH_SERIAL          0x13002u  // 0x10
#define SW2_FLASH_GYRO_CAL        0x13040u  // 0x10
#define SW2_FLASH_CAL_L_FACTORY   0x130a8u  // 9
#define SW2_FLASH_CAL_R_FACTORY   0x130e8u  // 9
#define SW2_FLASH_ACCEL_CAL       0x1310cu  // 0x0c
#define SW2_FLASH_CAL_L_USER      0x1fc040u // 11 (magic a1b2 + 9)
#define SW2_FLASH_CAL_R_USER      0x1fc080u // 11
#define SW2_USER_CAL_MAGIC        0xb2a1u   // little-endian read of a1 b2

// ---------------------------------------------------------------- timing
#define SW2_CMD_GUARD_MS        10      // pause between acknowledged commands
#define SW2_CMD_TIMEOUT_MS      250
#define SW2_NATIVE_FIRST_WAIT_MS 1000
#define SW2_COMMON_WAIT_MS      600
#define SW2_NATIVE_TO_COMMON_DELAY_MS 250
#define SW2_SETUP_SILENT_MS     2000    // no ATT progress right after connect => rescan
#define SW2_MTU_WAIT_MS         1500    // manual MTU exchange attempt (controller offers 512)
#define SW2_MTU_SETTLE_MS       150     // let the controller's ATT server come up after connect

#endif
