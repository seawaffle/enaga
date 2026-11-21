/* The C6748's USB0 wrapper registers and CFGCHIP2 (SPRUH79; musb_enaga.h describes their use). */
#ifndef USB_C6748_H
#define USB_C6748_H

#include <stdint.h>

#define ENAGA_USB0          0x01E00000u
#define ENAGA_USB0_CTRLR    (ENAGA_USB0 + 0x04)
#define ENAGA_USB0_INTCLRR  (ENAGA_USB0 + 0x28)
#define ENAGA_USB0_INTMSKSETR (ENAGA_USB0 + 0x30)
#define ENAGA_USB0_INTMSKCLRR (ENAGA_USB0 + 0x34)
#define ENAGA_USB0_INTMSKR  (ENAGA_USB0 + 0x2C)
#define ENAGA_USB0_INTMASKEDR (ENAGA_USB0 + 0x38)
#define ENAGA_USB0_EOIR     (ENAGA_USB0 + 0x3C)
#define ENAGA_USB0_SOURCES  0x01FF1E1Fu      /* TX 0-4, RX 1-4, USB 0-8: the original firmware's mask */
#define ENAGA_CFGCHIP2      0x01C14184u
#define ENAGA_REG(a)        (*(volatile uint32_t *)(a))

#endif
