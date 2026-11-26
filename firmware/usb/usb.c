#include "kernel.h"
#include "tusb.h"
#include "usb_c6748.h"
#include "usb_audio.h"

/* USB MIDI on the C6748's USB0 (docs/design.md "E4 plan", step 5), with TinyUSB (firmware/third_party/tinyusb; port
 * in musb_enaga.h). The unit is a class-compliant USB MIDI device with two cables on a full-speed bulk endpoint each
 * way: cable 0 is MIDI (the app's, and Enaga's clock), cable 1 the debug port ("Enaga Debug", usb_debug.c). Built
 * so mass storage and USB audio can join it as a composite device (E5).
 *
 *   - usb_init: TinyUSB's device stack, which starts the PHY and connects; CPU interrupt 10 <- event 19 (USB0_INT).
 *   - usb_isr: TinyUSB's interrupt handler, then the wrapper's end of interrupt.
 *   - usb_poll (main loop): TinyUSB's task; received MIDI bytes go to the parser as port ENAGA_MIDI_USB (midi.c),
 *     the debug cable's to usb_debug.c; queued MIDI out goes to TinyUSB, then the debug port's, when MIDI has
 *     nothing waiting. The out queue (usb_midi_send) takes whole messages from the main loop, the
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

#define EP_MIDI_OUT  0x01                      /* the endpoints (descriptors below) */
#define EP_MIDI_IN   0x81
#define EP_DRIVE_OUT 0x02
#define EP_DRIVE_IN  0x82

/* ---- mass storage ---- */

#define DRIVE_SECTORS_MAX (CFG_TUD_MSC_EP_BUFSIZE / 512u)
static volatile int drive_shared, drive_attention, drive_ejected;
u32 usb_drive_read_sectors, usb_drive_written_sectors, usb_drive_errors;
/* Where a copy's time goes, from the page's opening (STATUS USB_DRIVE_TIME, USB_DRIVE_PACKETS): reads and writes
 * (the callbacks, each up to DRIVE_SECTORS_MAX sectors), the time in the SD driver, and the time from one callback's
 * end to the next one's start (the USB data, the status and next command, and the computer's pauses) when under
 * DRIVE_GAP_MS. Microseconds, summed; [0] reads, [1] writes. */
#define DRIVE_GAP_MS 100u
u32 usb_drive_calls[2], usb_drive_sd_us[2], usb_drive_gap_us[2];
static u32 drive_call_end;

static void drive_call(int write, u32 start)   /* a callback's end: the gap before it */
{
    if (drive_call_end && start - drive_call_end < DRIVE_GAP_MS * TICKS_PER_MS)
        usb_drive_gap_us[write] += (start - drive_call_end) / 372u;
    usb_drive_calls[write]++;
    drive_call_end = tsc_read() | 1;
}

/* The SD card beside the USB transfer (2026-11-26): the card's time was in series with the packets, a quarter of a
 * write on a good port. A command's chunks (TinyUSB's 8 KiB buffer) now overlap the card through a second buffer,
 * worked in usb_poll after TinyUSB's task while the USB interrupt moves the next chunk's packets:
 *   - writing, every chunk but the command's last is copied aside and acknowledged, so TinyUSB takes the next one
 *     while the card writes this one. The last is written before the callback returns, so the command's status
 *     (and a SYNCHRONIZE CACHE or STOP after it) comes only once every sector is on the card. A chunk written
 *     behind that fails fails the command at its next chunk (there is always one).
 *   - reading, the command's next chunk is read ahead while this one goes out; a read ahead that failed, or does
 *     not match, is read again in the callback, which reports any error.
 * Read-ahead data is dropped by any write and when the card is shared or taken back, so it is always the card's.
 * usb_drive_ahead counts the chunks served from a read ahead [0] and written behind [1]. */
enum { SIDE_EMPTY, SIDE_WRITE, SIDE_READ_WANTED, SIDE_READ };
static u32 drive_side[CFG_TUD_MSC_EP_BUFSIZE / 4u];
static int side_state, side_failed;
static u32 side_lba, side_count, side_failed_next;   /* after a failure: the lba its command goes on at */
u32 usb_drive_ahead[2];

static int drive_sd(int write, void *buffer, u32 lba, u32 count)    /* the SD driver, timed */
{
    u32 start = tsc_read();
    int r = write ? sd_write(buffer, lba, count) : sd_read(buffer, lba, count);
    usb_drive_sd_us[write] += (tsc_read() - start) / 372u;
    return r;
}

static void drive_copy(u32 *to, const u32 *from, u32 bytes)
{
    u32 i;
    for (i = 0; i < bytes / 4u; i++) to[i] = from[i];
}

static void drive_failed(char what, int sd_code);

static void side_write(void)                   /* the chunk waiting aside, to the card */
{
    int r = drive_sd(1, drive_side, side_lba, side_count);
    side_state = SIDE_EMPTY;
    if (r != 0) { drive_failed('W', r); side_failed = 1; side_failed_next = side_lba + side_count; return; }
    usb_drive_written_sectors += side_count;
    usb_drive_ahead[1]++;
}

void usb_drive_share(int on)
{
    if (side_state == SIDE_WRITE && drive_shared) side_write();    /* usb_poll leaves none; but never lose one */
    side_state = SIDE_EMPTY;
    side_failed = 0;
    if (on) {                                  /* the counters start with the page */
        u32 i;
        for (i = 0; i < 2; i++) usb_drive_calls[i] = usb_drive_sd_us[i] = usb_drive_gap_us[i] = usb_drive_ahead[i] = 0;
        drive_call_end = 0;
        i = tu_edpt_number(EP_DRIVE_IN);
        enaga_musb_in_packets[i] = enaga_musb_in_wait_us[i] = 0;
        enaga_musb_out_packets[i] = enaga_musb_out_gap_us[i] = 0;
        enaga_musb_in_empty[i] = enaga_musb_in_queued[i] = enaga_musb_out_backlog[i] = enaga_musb_out_alone[i] = 0;
        usb_isr_count = usb_isr_us = usb_isr_max_us = 0;
    }
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

/* A read or write while the drive is not ready (not shared, or after the computer's STOP: Windows sometimes reads
 * once more after ejecting) fails as NOT READY, MEDIUM NOT PRESENT, as TEST UNIT READY says; not a card error. */
static int drive_not_ready(void)
{
    if (drive_ready()) return 0;
    tud_msc_set_sense(0, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
    return 1;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    int r = 0;
    u32 start = tsc_read(), count = bufsize / 512u, left;
    if (drive_not_ready()) return TUD_MSC_RET_ERROR;
    if (!drive_range(lba, offset, bufsize) || ((u32)buffer & 3)) { drive_failed('R', 0); return TUD_MSC_RET_ERROR; }
    if (side_state == SIDE_WRITE) side_write();
    if (side_state == SIDE_READ && side_lba == lba && side_count == count) {
        drive_copy(buffer, drive_side, bufsize);
        usb_drive_ahead[0]++;
    } else {
        r = drive_sd(0, buffer, lba, count);
    }
    side_state = SIDE_EMPTY;
    left = tud_msc_data_left(lun) - bufsize;
    if (r == 0 && left >= 512u) {              /* the command goes on: its next chunk, in usb_poll */
        side_lba = lba + count;
        side_count = (left < CFG_TUD_MSC_EP_BUFSIZE ? left : CFG_TUD_MSC_EP_BUFSIZE) / 512u;
        side_state = SIDE_READ_WANTED;
    }
    drive_call(0, start);
    if (r != 0) {
        drive_failed('R', r);
        return TUD_MSC_RET_ERROR;
    }
    usb_drive_read_sectors += count;
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    int r;
    u32 start = tsc_read(), count = bufsize / 512u;
    if (drive_not_ready()) return TUD_MSC_RET_ERROR;
    if (!drive_range(lba, offset, bufsize) || ((u32)buffer & 3)) { drive_failed('W', 0); return TUD_MSC_RET_ERROR; }
    if (side_state == SIDE_WRITE) side_write();
    side_state = SIDE_EMPTY;                   /* a read ahead is stale now */
    if (side_failed) {                         /* a chunk written behind failed (logged then): if this is the */
        side_failed = 0;                       /* same command's next chunk, the command fails */
        if (lba == side_failed_next) { drive_call(1, start); return TUD_MSC_RET_ERROR; }
    }
    if (tud_msc_data_left(lun) > bufsize) {    /* not the last: aside, written in usb_poll */
        drive_copy(drive_side, (const u32 *)buffer, bufsize);
        side_lba = lba;
        side_count = count;
        side_state = SIDE_WRITE;
        drive_call(1, start);
        return (int32_t)bufsize;
    }
    r = drive_sd(1, buffer, lba, count);
    drive_call(1, start);
    if (r != 0) {
        drive_failed('W', r);
        return TUD_MSC_RET_ERROR;
    }
    usb_drive_written_sectors += count;
    return (int32_t)bufsize;
}

static void drive_work(void)                   /* usb_poll, after TinyUSB's task: the card beside the packets */
{
    if (side_state == SIDE_WRITE) {
        side_write();
    } else if (side_state == SIDE_READ_WANTED) {
        side_state = drive_range(side_lba, 0, side_count * 512u)
                     && drive_sd(0, drive_side, side_lba, side_count) == 0 ? SIDE_READ : SIDE_EMPTY;
    }
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
    enaga_usb_phy_start();                     /* first: TinyUSB's dcd_init touches the core before its PHY hook,
                                                * and the core must be powered on (PSC1) before any access */
    tusb_init(0, &init);
}

u32 usb_isr_count, usb_isr_us, usb_isr_max_us;

void usb_isr(void)
{
    u32 start = tsc_read(), us;
    tud_int_handler(0);
    REG32(ENAGA_USB0_EOIR) = 0;
    us = (tsc_read() - start) / 372u;
    usb_isr_count++;
    usb_isr_us += us;
    if (us > usb_isr_max_us) usb_isr_max_us = us;
}

int usb_poll(u32 ms)                           /* main loop: returns the MIDI bytes received */
{
    u8 buffer[64], cable;
    u32 n, i, got = 0;
    tud_task_ext(0, false);
    drive_work();
    while ((n = tud_midi_n_demux_stream_read(0, &cable, buffer, sizeof buffer)) > 0) {
        if (cable == 1) {
            for (i = 0; i < n; i++) usb_debug_byte(buffer[i]);
            continue;
        }
        if (cable != 0) continue;
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
    if (tx_tail == tx_head) usb_debug_poll();  /* MIDI first */
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
    .bcdDevice = 0x0030,                       /* 0.3.0 */
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
#define AC_LEN       (11 + 12 + 9 + 12 + 9)  /* header, then each direction's input and output terminal */
#define MS_LEN       (9 + 7 + 2 * TUD_MIDI_DESC_JACK_LEN + 2 * TUD_MIDI_DESC_EP_LEN(2))
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
    U16_TO_U8S_LE(7 + 2 * TUD_MIDI_DESC_JACK_LEN + 2 * TUD_MIDI_DESC_EP_LEN(2)),
    TUD_MIDI_DESC_JACK_DESC(1, 0),             /* cable 0: MIDI */
    TUD_MIDI_DESC_JACK_DESC(2, 3),             /* cable 1: the debug port, string 3 */
    TUD_MIDI_DESC_EP(EP_MIDI_OUT, 64, 2), TUD_MIDI_JACKID_IN_EMB(1), TUD_MIDI_JACKID_IN_EMB(2),
    TUD_MIDI_DESC_EP(EP_MIDI_IN, 64, 2), TUD_MIDI_JACKID_OUT_EMB(1), TUD_MIDI_JACKID_OUT_EMB(2),
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

/* Strings: 0 the language (English, US), 1 the maker, 2 the product with the device's name ("Enaga SmplTrek"),
 * 3 the debug port's jacks. */
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
        else if (index == 3) s = "Enaga Debug";
        else return 0;
        for (n = 0; s[n] && n < 31; n++) text[1 + n] = (u8)s[n];
    }
    text[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return text;
}
