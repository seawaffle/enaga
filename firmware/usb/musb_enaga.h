/* TinyUSB MUSB port for the TI C6748's USB0 (included by dcd_musb.c for TUP_USBIP_MUSB_ENAGA).
 *
 * USB0 is a Mentor MUSB core at 0x01E00400 behind TI's wrapper at 0x01E00000, which gathers the core's interrupts:
 * INTMASKEDR (+0x38) holds them, bits 0-4 transmit endpoints 0-4, 8-12 receive endpoints, 16-24 the core's USB
 * interrupts; INTCLRR (+0x28) clears them, INTMSKSETR / INTMSKCLRR (+0x30 / +0x34) mask them, and EOIR (+0x3C) ends
 * the interrupt. That is the mode with CTRLR.UINT = 0 (the reset state), in which the core's own INTRTX / INTRRX /
 * INTRUSB must not be read, since reading clears them (SPRUH79C 33.2.8.13.1, 33.4.9, 33.4.23-33.4.27). The original
 * firmware (StarterWare) and TI's OMAP-L138 driver work this way, and so does this port (musb_dcd_int_status,
 * musb_dcd_int_pending). Enable and disable mask the
 * wrapper's sources: one that arrives meanwhile is held and raises the interrupt when unmasked.
 *
 * PHY and start-up: as the original firmware (smpltrek-re HW-12, docs/04-hardware-access.md "USB device"): CFGCHIP2
 * = 0x0972 (24 MHz reference, PHY clock from AUXCLK, data polarity normal, PHY PLL on, VBUS and session comparators
 * on, no OTG override), PSC1 module 1, CTRLR reset, the PHY's clock-good bit (CFGCHIP2 bit 17), then the masks.
 * Full speed (HSEN is never set). The event is 19 (USB0_INT), on CPU interrupt 10 (usb.c). */
#ifndef MUSB_ENAGA_H
#define MUSB_ENAGA_H

#include <stdint.h>

#include "usb_c6748.h"

#define MUSB_CFG_SHARED_FIFO       0
#define MUSB_CFG_DYNAMIC_FIFO      1
#define MUSB_CFG_DYNAMIC_FIFO_SIZE 4096
#define MUSB_INTR_EP_TX_RX_SWAP    0
/* No RX / TX_DOUBLE_PACKET_DISABLE registers (0x340 / 0x342) on this core: double buffering is the DPB bit of
 * TXFIFOSZ / RXFIFOSZ alone (SPRUH79C 33.4). */
#define MUSB_CFG_NO_DOUBLE_PACKET_DISABLE 1

/* dcd_remote_wakeup's delay loop uses these Cortex-M names: about 1 ms of NOPs here. */
#define SystemCoreClock 372000000u
#define __NOP() __asm__ __volatile__("nop")

static const uintptr_t MUSB_BASES[] = { ENAGA_USB0 + 0x400 };

int enaga_usb_phy_start(void);                /* usb.c: PSC, CFGCHIP2, reset, PHY clock; 0 or -1 */

static inline void musb_dcd_phy_init(uint8_t rhport)
{
    (void)rhport;
    enaga_usb_phy_start();
}

TU_ATTR_ALWAYS_INLINE static inline void musb_dcd_int_enable(uint8_t rhport)
{
    (void)rhport;
    ENAGA_REG(ENAGA_USB0_INTMSKSETR) = ENAGA_USB0_SOURCES;
}

TU_ATTR_ALWAYS_INLINE static inline void musb_dcd_int_disable(uint8_t rhport)
{
    (void)rhport;
    ENAGA_REG(ENAGA_USB0_INTMSKCLRR) = ENAGA_USB0_SOURCES;
}

TU_ATTR_ALWAYS_INLINE static inline unsigned musb_dcd_get_int_enable(uint8_t rhport)
{
    (void)rhport;
    return (ENAGA_REG(ENAGA_USB0_INTMSKR) & ENAGA_USB0_SOURCES) != 0;
}

TU_ATTR_ALWAYS_INLINE static inline void musb_dcd_int_clear(uint8_t rhport)
{
    (void)rhport;
    ENAGA_REG(ENAGA_USB0_INTCLRR) = ENAGA_USB0_SOURCES;
}

static inline void musb_dcd_int_handler_enter(uint8_t rhport)
{
    (void)rhport;
}

/* The status the core's INTRUSB / INTRTX / INTRRX would give, from the wrapper; cleared there as it is read. */
#define musb_dcd_int_status musb_dcd_int_status
static inline void musb_dcd_int_status(uint8_t rhport, uint_fast8_t *usb, uint_fast8_t *tx, uint_fast8_t *rx)
{
    uint32_t status = ENAGA_REG(ENAGA_USB0_INTMASKEDR);
    (void)rhport;
    ENAGA_REG(ENAGA_USB0_INTCLRR) = status;
    *tx = (uint_fast8_t)(status & 0x1Fu);
    *rx = (uint_fast8_t)((status >> 8) & 0x1Eu);
    *usb = (uint_fast8_t)((status >> 16) & 0xFFu);
}

/* The handler's re-check for packets that arrived while it worked (double buffering, a SETUP during bulk): the
 * transmit (is_rx 0) or receive endpoint bits pending in the wrapper, cleared there. In this mode the core's own
 * INTRTX / INTRRX must not be read, since a read clears them (SPRUH79C 33.4.23-33.4.27). */
static inline uint_fast8_t musb_dcd_int_pending(uint8_t rhport, int is_rx)
{
    uint32_t bits = ENAGA_REG(ENAGA_USB0_INTMASKEDR) & (is_rx ? 0x1E00u : 0x1Fu);
    (void)rhport;
    ENAGA_REG(ENAGA_USB0_INTCLRR) = bits;
    return (uint_fast8_t)(is_rx ? bits >> 8 : bits);
}

#endif
