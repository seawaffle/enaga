/* TinyUSB configuration for Enaga (firmware/third_party/tinyusb): a USB MIDI and mass storage device on the C6748's USB0, full speed,
 * no RTOS. The C6748 is not one of TinyUSB's MCU options: the MUSB driver is selected here and its port is
 * musb_enaga.h. */
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#define CFG_TUSB_MCU               OPT_MCU_NONE
#define CFG_TUSB_OS                OPT_OS_NONE
#define CFG_TUSB_DEBUG             0
#define CFG_TUSB_RHPORT0_MODE      (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_ENABLED            1
#define CFG_TUD_MAX_SPEED          OPT_MODE_FULL_SPEED

#define TUP_USBIP_MUSB             1
#define TUP_USBIP_MUSB_ENAGA       1
#define TUP_DCD_ENDPOINT_MAX       5             /* EP0 and EP1-4 (SPRUH79, USB0) */

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN         __attribute__((aligned(4)))

#define CFG_TUD_ENDPOINT0_SIZE     64

#define CFG_TUD_CDC                0
#define CFG_TUD_MSC                1
#define CFG_TUD_MSC_EP_BUFSIZE     8192          /* 16 sectors per SD command */
#define CFG_TUD_HID                0
#define CFG_TUD_VENDOR             0
#define CFG_TUD_AUDIO              0
#define CFG_TUD_MIDI               1
#define CFG_TUD_MIDI_RX_BUFSIZE    512
#define CFG_TUD_MIDI_TX_BUFSIZE    512

#endif
