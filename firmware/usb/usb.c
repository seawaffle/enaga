#include "kernel.h"
#include "tusb.h"
#include "usb_c6748.h"
#include "usb_audio.h"

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
 * Mass storage (E5 step 3, "USB DRIVE" in the menu): a second function on the same device, bulk-only transport with
 * SCSI commands (TinyUSB's MSC class), one removable drive that is the whole SD card. It shows no medium until the
 * menu shares the card (usb_drive_share), so a computer sees an empty card reader, as with any reader with no card
 * in it; then it reports a medium change (UNIT ATTENTION), and the computer mounts the card. START STOP UNIT with
 * START clear (the computer's eject) is noted for the menu, which then takes the card back. The SD card is never
 * used by FatFs and the computer at once: the menu stops the app and unmounts it first (menu.c). Reads and writes run
 * in usb_poll, in the main loop, straight on the SD driver.
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
    REG8(ENAGA_USB0 + 0x401) = 0x00;           /* POWER: HSENAB off. It is on after reset, and the original firmware
                                                * writes 0x40 here; left on, the core chirps and a computer runs it at
                                                * high speed, where the full-speed descriptors mean other things
                                                * (bInterval 1 = 125 us, bulk at 64 bytes): unit session 6 */
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

/* ---- mass storage ---- */

#define DRIVE_SECTORS_MAX (CFG_TUD_MSC_EP_BUFSIZE / 512u)
static volatile int drive_shared, drive_attention, drive_ejected;
u32 usb_drive_read_sectors, usb_drive_written_sectors, usb_drive_errors;

void usb_drive_share(int on)
{
    drive_shared = on;
    drive_attention = on;                      /* the computer is told the medium changed */
    drive_ejected = 0;
}

int usb_drive_ejected(void) { return drive_ejected; }

static int drive_ready(void) { return drive_shared && !drive_ejected && sd_present() && sd_sectors; }

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;
    if (!drive_ready()) { tud_msc_set_sense(0, SCSI_SENSE_NOT_READY, 0x3A, 0x00); return false; }   /* no medium */
    if (drive_attention) {
        drive_attention = 0;
        tud_msc_set_sense(0, SCSI_SENSE_UNIT_ATTENTION, 0x28, 0x00);   /* not ready to ready: medium may have changed */
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    *block_count = drive_ready() ? sd_sectors : 0;
    *block_size = 512;
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    const char *vendor = "Enaga", *product = "SD card", *rev = ENAGA_VERSION;   /* space-padded fields */
    u32 i, end;
    (void)lun;
    for (i = 0, end = 0; i < 8; i++) { if (!vendor[i]) end = 1; vendor_id[i] = end ? ' ' : (u8)vendor[i]; }
    for (i = 0, end = 0; i < 16; i++) { if (!product[i]) end = 1; product_id[i] = end ? ' ' : (u8)product[i]; }
    for (i = 0, end = 0; i < 4; i++) { if (!rev[i] || (i == 3 && rev[i] == '.')) end = 1; product_rev[i] = end ? ' ' : (u8)rev[i]; }
}

bool tud_msc_is_writable_cb(uint8_t lun) { (void)lun; return true; }

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun; (void)power_condition; (void)load_eject;
    if (!start && drive_shared) drive_ejected = 1;   /* eject, or stop: either way the computer is done */
    return true;
}

static int drive_range(uint32_t lba, uint32_t offset, uint32_t bytes)
{
    return drive_ready() && offset == 0 && bytes % 512u == 0 && bytes / 512u <= DRIVE_SECTORS_MAX
           && lba < sd_sectors && bytes / 512u <= sd_sectors - lba;
}

/* A read or write the drive could not do, logged (the first four): "DRV R -3 C18 0008" is the SD driver's code, its
 * command and MMCST0; "DRV W RANGE" a request outside the card or misaligned. */
static void drive_failed(char what, int sd_code)
{
    static const char hex[] = "0123456789ABCDEF";
    char line[24] = "DRV ? ";
    int n = 6, i;
    usb_drive_errors++;
    if (usb_drive_errors > 4) return;
    line[4] = what;
    if (!sd_code) {
        for (i = 0; "RANGE"[i]; i++) line[n++] = "RANGE"[i];
    } else {
        u32 v = (u32)(sd_code < 0 ? -sd_code : sd_code), c = sd_io_command;
        if (sd_code < 0) line[n++] = '-';
        line[n++] = (char)('0' + v % 10u);
        line[n++] = ' '; line[n++] = 'C';
        if (c >= 10u) line[n++] = (char)('0' + c / 10u % 10u);
        line[n++] = (char)('0' + c % 10u);
        line[n++] = ' ';
        for (i = 12; i >= 0; i -= 4) line[n++] = hex[sd_io_status >> i & 15];
    }
    line[n] = 0;
    log_line(line);
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    int r;
    (void)lun;
    if (!drive_range(lba, offset, bufsize) || ((u32)buffer & 3)) { drive_failed('R', 0); return TUD_MSC_RET_ERROR; }
    if ((r = sd_read(buffer, lba, bufsize / 512u)) != 0) {
        drive_failed('R', r);
        return TUD_MSC_RET_ERROR;
    }
    usb_drive_read_sectors += bufsize / 512u;
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    int r;
    (void)lun;
    if (!drive_range(lba, offset, bufsize) || ((u32)buffer & 3)) { drive_failed('W', 0); return TUD_MSC_RET_ERROR; }
    if ((r = sd_write(buffer, lba, bufsize / 512u)) != 0) {
        drive_failed('W', r);
        return TUD_MSC_RET_ERROR;
    }
    usb_drive_written_sectors += bufsize / 512u;
    return (int32_t)bufsize;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)lun; (void)buffer; (void)bufsize;
    if (scsi_cmd[0] == 0x35) return 0;         /* SYNCHRONIZE CACHE (10): writes go straight to the card */
    tud_msc_set_sense(0, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);   /* invalid command operation code */
    return -1;
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

/* One AudioControl interface for the audio function: its header lists MIDI streaming and the two audio streaming
 * interfaces (TinyUSB's MIDI driver takes the AudioControl interface with the MIDI streaming one right after it;
 * usb_audio.c takes 2 and 3). Then mass storage. */
enum { ITF_AUDIO_CONTROL, ITF_MIDI_STREAMING, ITF_AUDIO_OUT, ITF_AUDIO_IN, ITF_DRIVE, ITF_COUNT };
#define EP_MIDI_OUT  0x01
#define EP_MIDI_IN   0x81
#define EP_DRIVE_OUT 0x02
#define EP_DRIVE_IN  0x82
#define AC_LEN       (11 + 12 + 9 + 12 + 9)  /* header, then each direction's input and output terminal */
#define MS_LEN       (TUD_MIDI_DESC_LEN - TUD_MIDI_DESC_HEAD_LEN + 9 + 7)
#define AS_LEN       (9 + 9 + 7 + 11 + 9 + 7)
#define CONFIG_LENGTH (TUD_CONFIG_DESC_LEN + 9 + AC_LEN + MS_LEN + 2 * AS_LEN + TUD_MSC_DESC_LEN)

#define TERMINAL_IN(_id, _type)  12, TUSB_DESC_CS_INTERFACE, 0x02, _id, U16_TO_U8S_LE(_type), 0, 2, U16_TO_U8S_LE(0x0003), 0, 0
#define TERMINAL_OUT(_id, _type, _source)  9, TUSB_DESC_CS_INTERFACE, 0x03, _id, U16_TO_U8S_LE(_type), 0, _source, 0
/* A streaming interface: alternate 0 with no endpoint, alternate 1 with PCM, 2 channels of 16 bits at 48 kHz, one
 * isochronous endpoint of 196 bytes (49 frames) every frame. */
#define AUDIO_STREAMING(_itf, _terminal, _ep, _sync) \
    9, TUSB_DESC_INTERFACE, _itf, 0, 0, TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_STREAMING, 0, 0, \
    9, TUSB_DESC_INTERFACE, _itf, 1, 1, TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_STREAMING, 0, 0, \
    7, TUSB_DESC_CS_INTERFACE, 0x01, _terminal, 1, U16_TO_U8S_LE(0x0001), \
    11, TUSB_DESC_CS_INTERFACE, 0x02, 1, 2, 2, 16, 1, 0x80, 0xBB, 0x00, \
    9, TUSB_DESC_ENDPOINT, _ep, (uint8_t)(TUSB_XFER_ISOCHRONOUS | (_sync)), U16_TO_U8S_LE(196), 1, 0, 0, \
    7, TUSB_DESC_CS_ENDPOINT, 0x01, 0, 0, U16_TO_U8S_LE(0)

static const uint8_t configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_LENGTH, TUSB_DESC_CONFIG_ATT_SELF_POWERED, 100),
    9, TUSB_DESC_INTERFACE, ITF_AUDIO_CONTROL, 0, 0, TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_CONTROL, 0, 0,
    11, TUSB_DESC_CS_INTERFACE, 0x01, U16_TO_U8S_LE(0x0100), U16_TO_U8S_LE(AC_LEN), 3, ITF_MIDI_STREAMING,
    ITF_AUDIO_OUT, ITF_AUDIO_IN,
    TERMINAL_IN(1, 0x0101),                    /* USB streaming, from the computer */
    TERMINAL_OUT(2, 0x0603, 1),                /* -> line connector: the app's input */
    TERMINAL_IN(3, 0x0603),                    /* line connector: the app's output */
    TERMINAL_OUT(4, 0x0101, 3),                /* -> USB streaming, to the computer */
    9, TUSB_DESC_INTERFACE, ITF_MIDI_STREAMING, 0, 2, TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_MIDI_STREAMING, 0, 0,
    7, TUSB_DESC_CS_INTERFACE, MIDI_CS_INTERFACE_HEADER, U16_TO_U8S_LE(0x0100),
    U16_TO_U8S_LE(7 + TUD_MIDI_DESC_JACK_LEN + 2 * TUD_MIDI_DESC_EP_LEN(1)),
    TUD_MIDI_DESC_JACK_DESC(1, 0),
    TUD_MIDI_DESC_EP(EP_MIDI_OUT, 64, 1), TUD_MIDI_JACKID_IN_EMB(1),
    TUD_MIDI_DESC_EP(EP_MIDI_IN, 64, 1), TUD_MIDI_JACKID_OUT_EMB(1),
    AUDIO_STREAMING(ITF_AUDIO_OUT, 1, USB_AUDIO_EP_OUT, TUSB_ISO_EP_ATT_ADAPTIVE),
    AUDIO_STREAMING(ITF_AUDIO_IN, 4, USB_AUDIO_EP_IN, TUSB_ISO_EP_ATT_ASYNCHRONOUS),
    TUD_MSC_DESCRIPTOR(ITF_DRIVE, 0, EP_DRIVE_OUT, EP_DRIVE_IN, 64),
};
_Static_assert(sizeof configuration_descriptor == CONFIG_LENGTH, "the configuration's length");

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
