#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------
// Common Configuration
//--------------------------------------------------------------------

// RP2350 device is selected by the pico-sdk tinyusb build (PICO_RP2350).
// Do not define CFG_TUSB_MCU here.

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_PICO
#endif

#ifndef CFG_TUSB_RHPORT0_MODE
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#endif

#define CFG_TUD_ENABLED 1

//--------------------------------------------------------------------
// Device Configuration
//--------------------------------------------------------------------

#define CFG_TUD_ENDPOINT0_SIZE 64

//------------- CLASS -------------//
// USB serial log. It is a bring-up aid, not part of the controller persona:
// the reference dongle ships without it. Set this to 0 for a clean
// single-interface HID device (the config descriptor and log sink follow this
// one switch, and so does TinyUSB itself, so they can never disagree).
#ifndef SW2_CDC_LOG
#define SW2_CDC_LOG 1
#endif

#if SW2_CDC_LOG
#define CFG_TUD_CDC 1
#else
#define CFG_TUD_CDC 0
#endif
#define CFG_TUD_HID 1
#define CFG_TUD_MSC 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0

// CDC buffer sizes (log transport)
#define CFG_TUD_CDC_RX_BUFSIZE 64
#define CFG_TUD_CDC_TX_BUFSIZE 1024
#define CFG_TUD_CDC_EP_BUFSIZE 64

// HID
#define CFG_TUD_HID_EP_BUFSIZE 64

#ifdef __cplusplus
}
#endif

#endif
