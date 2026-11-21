#include "kernel.h"
#include "tusb.h"
#include "usb_c6748.h"

/* USB MIDI on the C6748's USB0 (docs/design.md "E4 plan", step 5), with TinyUSB (firmware/third_party/tinyusb; port
 * in musb_enaga.h). The unit is a class-compliant USB MIDI device with one cable, a full-speed bulk endpoint each
 * way. Built so mass storage and USB audio can join it as a composite device (E5).
 *
 *   - usb_init: TinyUSB's device stack, which starts the PHY and connects; CPU interrupt 10 <- event 19 (USB0_INT).
 *   - usb_isr: TinyUSB's interrupt handler, then the wrapper's end of interrupt.
 *   - usb_poll (main loop): TinyUSB's task; received MIDI bytes go to the parser as port ENAGA_MIDI_USB (midi.c);
 *     queued MIDI out goes to TinyUSB. The out queue (usb_midi_send) takes whole messages from the main loop, the
 *     audio callback and the clock in the audio interrupt, with interrupts off while it is written, and is drained
 *     here, so USB MIDI out is late by up to one main-loop pass and one USB frame (1 ms). Nothing is queued while the
 *     unit is not configured by a host.
 *
 * USB IDs: ENAGA_USB_VID / ENAGA_USB_PID. Until the pid.codes product ID is granted these are TinyUSB's test
 * values, not for release (docs/design.md "Open items"). */

#define ENAGA_USB_VID 0xCAFEu                  /* TinyUSB's test vendor ID: replace before a release */
#define ENAGA_USB_PID 0x4011u

#define PSC1_USB0 1u

u32 usb_tx_lost;                               /* MIDI out bytes dropped: the queue was full */
int usb_phy_status = 1;                         /* 0 the PHY started; -1 it did not (PSC or PHY clock); 1 not yet */

int enaga_usb_phy_start(void)                  /* the original firmware's order (musb_enaga.h) */
{
    u32 start;
    usb_phy_status = -1;
    REG32(ENAGA_CFGCHIP2) = 0x0972u;
    if (psc_enable(PSC1, PSC1_USB0) < 0) return -1;
    REG32(ENAGA_USB0_CTRLR) = 1;               /* reset; clears itself */
    start = tsc_read();
    while ((REG32(ENAGA_USB0_CTRLR) & 1) && tsc_read() - start < TICKS_PER_MS) {}
    REG32(ENAGA_CFGCHIP2) = (REG32(ENAGA_CFGCHIP2) & ~((1u << 15) | (1u << 10) | (1u << 9))) | (1u << 6);
    start = tsc_read();
    while (!(REG32(ENAGA_CFGCHIP2) & (1u << 17)))   /* USB0PHYCLKGD: within 10 ms on the unit (HW-59) */
        if (tsc_read() - start > 50u * TICKS_PER_MS) return -1;
    REG32(ENAGA_USB0_INTCLRR) = ENAGA_USB0_SOURCES;
    REG8(ENAGA_USB0 + 0x40B) = 0x27;           /* INTRUSBE: suspend, resume, reset, disconnect (no SOF) */
    REG32(ENAGA_USB0_INTMSKSETR) = ENAGA_USB0_SOURCES;
    usb_phy_status = 0;
    return 0;
}

/* ---- MIDI out queue ---- */

#define TX_RING 2048u
#define USB_STALL_MS 100u
static u8 tx_ring[TX_RING];
static volatile u32 tx_head, tx_tail;

static volatile u32 tx_moved_ms;              /* the queue last drained, or was last empty */

u32 usb_midi_tx_free(void) { return TX_RING - (tx_head - tx_tail); }

/* The host has stopped reading (no program has the port open, say): bytes waiting and none taken for
 * USB_STALL_MS. MIDI out then stops waiting for USB room, so DIN is not held up; USB carries on when it reads. */
int usb_midi_stalled(void) { return tx_head != tx_tail && timer_ms() - tx_moved_ms > USB_STALL_MS; }

int usb_midi_send(const u8 *data, u32 bytes)   /* all or nothing; 0 when not configured (nothing to send to) */
{
    u32 gie, i;
    int result = 0;
    if (!tud_mounted()) return 0;
    gie = irq_global_disable();
    if (tx_head == tx_tail) tx_moved_ms = timer_ms();
    if (TX_RING - (tx_head - tx_tail) >= bytes) {
        for (i = 0; i < bytes; i++) tx_ring[(tx_head + i) % TX_RING] = data[i];
        tx_head += bytes;
    } else {
        usb_tx_lost += bytes;
        result = -1;
    }
    if (gie) irq_global_enable();
    return result;
}

/* ---- the stack ---- */

void usb_init(void)
{
    const tusb_rhport_init_t init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };
    tusb_init(0, &init);
}

void usb_isr(void)
{
    tud_int_handler(0);
    REG32(ENAGA_USB0_EOIR) = 0;
}

int usb_poll(u32 ms)                           /* main loop: returns the MIDI bytes received */
{
    u8 buffer[64];
    u32 n, i, got = 0;
    tud_task_ext(0, false);
    while ((n = tud_midi_stream_read(buffer, sizeof buffer)) > 0) {
        for (i = 0; i < n; i++) midi_usb_byte(buffer[i], ms);
        got += n;
    }
    while (tx_tail != tx_head && tud_mounted()) {   /* whole bytes; TinyUSB packs them into USB MIDI events */
        u32 head = tx_head, count = head - tx_tail, at = tx_tail % TX_RING, sent;
        if (count > TX_RING - at) count = TX_RING - at;
        sent = tud_midi_stream_write(0, &tx_ring[at], count);
        tx_tail += sent;
        if (sent) tx_moved_ms = timer_ms();
        if (sent < count) break;               /* TinyUSB's buffer is full: the rest next pass */
    }
    if (!tud_mounted()) tx_tail = tx_head;     /* the host went: drop what was waiting */
    return (int)got;
}

int usb_configured(void) { return tud_mounted(); }

/* ---- TinyUSB's hooks ---- */

uint32_t tusb_time_millis_api(void) { return timer_ms(); }

static const tusb_desc_device_t device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0,                         /* per interface */
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = ENAGA_USB_VID,
    .idProduct = ENAGA_USB_PID,
    .bcdDevice = 0x0020,                       /* 0.2.0 */
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 0,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)&device_descriptor; }

enum { ITF_MIDI_CONTROL, ITF_MIDI_STREAMING, ITF_COUNT };
#define EP_MIDI_OUT 0x01
#define EP_MIDI_IN  0x81
#define CONFIG_LENGTH (TUD_CONFIG_DESC_LEN + TUD_MIDI_DESC_LEN)

static const uint8_t configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_LENGTH, TUSB_DESC_CONFIG_ATT_SELF_POWERED, 100),
    TUD_MIDI_DESCRIPTOR(ITF_MIDI_CONTROL, 0, EP_MIDI_OUT, EP_MIDI_IN, 64),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return configuration_descriptor;
}

/* Strings: 0 the language (English, US), 1 the maker, 2 the product with the device's name ("Enaga SmplTrek"). */
static const char *product_name(void)
{
    switch (device_id) {
    case ENAGA_DEVICE_SMPLTREK: return "Enaga SmplTrek";
    case ENAGA_DEVICE_LOFI12XT: return "Enaga Lofi-12 XT";
    case ENAGA_DEVICE_CYDRUMS: return "Enaga CyDrums";
    default: return "Enaga";
    }
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    static uint16_t text[32];
    const char *s;
    u32 n = 0;
    (void)langid;
    if (index == 0) { text[1] = 0x0409; n = 1; }
    else {
        if (index == 1) s = "Enaga";
        else if (index == 2) s = product_name();
        else return 0;
        for (n = 0; s[n] && n < 31; n++) text[1 + n] = (u8)s[n];
    }
    text[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return text;
}
