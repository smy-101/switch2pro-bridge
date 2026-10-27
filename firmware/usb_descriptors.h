#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H
#include <stdint.h>

// USB device identity: DualShock 4 (CUH-ZCT2U), accepted by Steam / DS4Windows.
#define USB_VID_SONY_DS4   0x054c
#define USB_PID_DS4_V2     0x09cc

void usb_descriptors_init(void);   // nothing dynamic yet; reserved

#endif
