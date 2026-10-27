#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H
#include <stdint.h>

// Host persona: what the PC thinks the bridge is.
typedef enum {
    USB_PERSONA_PRO = 0,   // Nintendo 057e:2009 Switch Pro (default; Steam
                           // shows native ABXY glyphs matching the controller)
    USB_PERSONA_DS4 = 1,   // Sony 054c:09cc DualShock 4 (DS4Windows setups)
} usb_persona_t;

usb_persona_t usb_persona_get(void);
const char *usb_persona_name(void);
void usb_persona_toggle(void);   // then call usb_reenumerate()

// Sony DS4 identity constants (kept for reference in logs/tests)
#define USB_VID_SONY_DS4   0x054c
#define USB_PID_DS4_V2     0x09cc

void usb_descriptors_init(void);
void usb_reenumerate(void);       // disconnect/reconnect with current persona

#endif
