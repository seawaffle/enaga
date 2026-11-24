#include "kernel.h"
#include "tusb.h"
#include "device/usbd_pvt.h"
#include "usb_c6748.h"
#include "usb_audio.h"

#include "portable/mentor/musb/musb_type.h"

/* USB audio (docs/design.md "E5 plan", step 4): USB Audio Class 1.0, as the original firmware (smpltrek-re HW-18,
 * HW-40): 48 kHz, 16-bit, stereo each way, isochronous, 1 ms frames. The computer's audio arrives as an input source
 * (ENAGA_INPUT_USB: audio.c gives it to the app's callback in place of the jacks), and what the app's callback
 * puts out goes to the computer as well as to the jacks, before VOLUME, with no app code.
 *
 *   - Descriptors (usb.c): one AudioControl interface (0) whose header lists MIDI streaming (1), audio out (2) and
 *     audio in (3). Out: USB streaming terminal 1 -> line out terminal 2, endpoint 0x03, adaptive. In: line
 *     terminal 3 -> USB streaming terminal 4, endpoint 0x83, asynchronous. Both 196 bytes (49 frames at most).
 *     No feature unit, so computers use their own volume.
 *   - This file is a TinyUSB class driver for interfaces 2 and 3: it claims them, allocates their FIFOs
 *     (double-buffered) and answers SET_INTERFACE. Alternate setting 1 starts a stream, 0 stops it. Computers also
 *     set and read each endpoint's sampling frequency as a stream starts (UAC1 5.2.3.2.3.1), although the
 *     descriptor offers no controls: SET_CUR is accepted (another rate than 48,000 is logged, as below) and the
 *     GETs answer 48,000. Any other request
 *     for these interfaces or endpoints is refused (a stall) and logged ("USB REQ 22 01 0200 0003": request type,
 *     request, value, index), so a computer's surprise shows on the status page.
 *   - Packets do not go through TinyUSB's transfers, which finish in the main loop (about 850 passes a second, less
 *     while an app draws). They are moved in the USB interrupt on every SOF (1 ms; the SOF interrupt is on while
 *     a stream runs): the OUT packet that arrived in the last frame is read from the FIFO, and the IN packet for
 *     the next frame is loaded (POWER.ISOUPDATE holds it until the SOF). The endpoints' own interrupts stay off,
 *     so TinyUSB never sees them. The core's endpoint registers are reached through INDEX, which TinyUSB also
 *     uses, so it is saved and restored.
 *   - Out (computer -> unit): a ring of frames, filled here and read by the audio interrupt (usb_audio_read). The
 *     computer's clock and the codec's crystal differ by some ppm, so the read is resampled by a ratio that keeps
 *     the ring near USB_OUT_TARGET frames (linear interpolation; the ratio follows the smoothed fill). A ring that
 *     runs dry gives silence and fills to the target again before playing.
 *   - In (unit -> computer): a ring filled by the audio interrupt (usb_audio_write) and emptied here, 48 frames a
 *     packet, 47 or 49 when the ring drifts from USB_IN_TARGET, which is what an asynchronous endpoint does: the
 *     computer follows the sizes. Until the ring has filled, packets carry silence.
 *   - Both rings' levels are taken at a steady phase (frames_since), so the 1 ms packets against the 1.33 ms
 *     blocks do not look like clock drift. */

#define USB_OUT_RING    1024u                  /* frames, a power of two */
#define USB_OUT_TARGET  144u                   /* 3 ms */
#define USB_IN_RING     1024u
#define USB_IN_TARGET   128u                   /* two audio blocks */
#define USB_IN_SLACK    12u
#define CYCLES_PER_FRAME (TICKS_PER_MS / 48u)
#define FRAME_BYTES     4u                     /* 16-bit left, 16-bit right */
#define PACKET_MAX      196u

enum { ITF_OUT = 2, ITF_IN = 3 };

static u32 out_ring[USB_OUT_RING], in_ring[USB_IN_RING];   /* frames: left in the low half, right in the high */
static volatile u32 out_head, out_tail, in_head, in_tail;
static volatile u8 out_alt, in_alt, out_primed, in_primed;
static float out_pos, out_fill, out_ratio = 1.0f;          /* audio interrupt only */
static volatile u32 out_arrived, in_written;               /* TSC at the last OUT packet, the last block written */
static float in_level;                                     /* USB interrupt only: the IN ring's level, averaged */

/* Ring levels at a steady phase. Packets come every 1 ms (48 frames) and blocks every 1.33 ms (64 frames), so a ring's
 * raw count saw-tooths by up to a block depending on which came last. Counting the frames the other side has made or
 * used since its last packet or block, at 48 a millisecond, takes the saw-tooth out. */
static u32 frames_since(u32 tsc)
{
    u32 n = (tsc_read() - tsc) / CYCLES_PER_FRAME;
    return n > 64u ? 64u : n;
}

u32 usb_audio_out_packets, usb_audio_out_overruns, usb_audio_out_underruns;
u32 usb_audio_in_packets, usb_audio_in_late, usb_audio_in_underruns;

/* How the IN endpoint is fed, chosen on the STATUS page (LEFT / RIGHT) and taken when a stream starts. On the unit,
 * at full speed, Windows saw babble on most IN packets (unit session 6, round 3) with the samples themselves right;
 * the emulator does not show it, so these are the candidates to try there:
 *   0  double-buffered, both halves kept loaded, POWER.ISOUPDATE (as first written)
 *   1  double-buffered, one packet a frame, ISOUPDATE
 *   2  double-buffered, one packet a frame, no ISOUPDATE
 *   3  single-buffered, one packet a frame, no ISOUPDATE */
int usb_audio_in_mode;
static u8 in_mode;                                         /* the running stream's */
static u32 in_late_at, in_underruns_at;                    /* the counters when it started */
float usb_audio_out_ratio;                    /* the resampling ratio in use, for the status page and tests */

int usb_audio_out_streaming(void) { return out_alt; }
int usb_audio_in_streaming(void) { return in_alt; }

/* ---- the audio interrupt's side ---- */

static float sample(u32 frame, int right)
{
    return (float)(int16_t)(right ? frame >> 16 : frame) * (1.0f / 32768.0f);
}

void usb_audio_read(float *in, u32 frames)    /* audio interrupt: the computer's audio, or silence */
{
    u32 i, avail = out_head - out_tail;
    float level = (float)avail - (float)frames_since(out_arrived);   /* what is left now of the last packet's frames */
    if (!out_alt || (!out_primed && avail < USB_OUT_TARGET)) {
        for (i = 0; i < 2 * frames; i++) in[i] = 0.0f;
        return;
    }
    if (!out_primed) { out_primed = 1; out_pos = 0.0f; out_fill = level; out_ratio = 1.0f; }
    out_fill += (level - out_fill) * (1.0f / 32.0f);       /* smoothed over about 32 blocks (43 ms) */
    out_ratio = 1.0f + (out_fill - (float)USB_OUT_TARGET) * 2e-5f;
    if (out_ratio > 1.002f) out_ratio = 1.002f;            /* 2000 ppm either way */
    if (out_ratio < 0.998f) out_ratio = 0.998f;
    usb_audio_out_ratio = out_ratio;
    if ((float)avail < out_pos + out_ratio * (float)frames + 2.0f) {   /* dry: silence, then fill again */
        out_primed = 0;
        usb_audio_out_underruns++;
        for (i = 0; i < 2 * frames; i++) in[i] = 0.0f;
        return;
    }
    for (i = 0; i < frames; i++) {
        u32 k = (u32)out_pos;
        float t = out_pos - (float)k;
        u32 a = out_ring[(out_tail + k) % USB_OUT_RING], b = out_ring[(out_tail + k + 1) % USB_OUT_RING];
        in[2 * i] = sample(a, 0) + (sample(b, 0) - sample(a, 0)) * t;
        in[2 * i + 1] = sample(a, 1) + (sample(b, 1) - sample(a, 1)) * t;
        out_pos += out_ratio;
    }
    {
        u32 used = (u32)out_pos;
        out_tail += used;
        out_pos -= (float)used;
    }
}

static u32 to16(float v)
{
    float s = v * 32768.0f;
    int x = s >= 32767.0f ? 32767 : s <= -32768.0f ? -32768 : (int)s;
    return (u32)x & 0xFFFFu;
}

void usb_audio_write(const float *out, u32 frames)   /* audio interrupt: the app's output, to the computer */
{
    u32 i;
    if (!in_alt) return;
    if (in_head - in_tail + frames > USB_IN_RING) { usb_audio_in_underruns++; return; }   /* not being taken */
    for (i = 0; i < frames; i++)
        in_ring[(in_head + i) % USB_IN_RING] = to16(out[2 * i]) | to16(out[2 * i + 1]) << 16;
    in_head += frames;
    in_written = tsc_read();
}

/* ---- the USB interrupt's side, every SOF ---- */

static musb_regs_t *const musb = (musb_regs_t *)(ENAGA_USB0 + 0x400);

static void service_out(void)
{
    musb_ep_csr_t *csr = &musb->indexed_csr;
    musb->index = USB_AUDIO_EP_OUT & 0x0F;
    if (csr->rx_csrl & MUSB_RXCSRL1_OVER) { csr->rx_csrl &= (u8)~MUSB_RXCSRL1_OVER; usb_audio_out_overruns++; }
    while (csr->rx_csrl & MUSB_RXCSRL1_RXRDY) {
        u32 bytes = csr->rx_count, n = bytes / FRAME_BYTES, i;
        volatile uint32_t *fifo = &musb->fifo[USB_AUDIO_EP_OUT & 0x0F];
        if (out_head - out_tail + n > USB_OUT_RING) { usb_audio_out_overruns++; n = 0; }
        for (i = 0; i < bytes / FRAME_BYTES; i++) {
            u32 frame = *fifo;
            if (i < n) out_ring[(out_head + i) % USB_OUT_RING] = frame;
        }
        for (i = 0; i < bytes % FRAME_BYTES; i++) (void)*(volatile u8 *)fifo;
        out_head += n;
        out_arrived = tsc_read();
        csr->rx_csrl = 0;                      /* the slot is free */
        usb_audio_out_packets++;
    }
}

static void load_in(volatile uint32_t *fifo, musb_ep_csr_t *csr)   /* one packet into a free FIFO half */
{
    u32 n = 48, i, avail = in_head - in_tail, level = avail + frames_since(in_written);
    if (!in_primed && level >= USB_IN_TARGET) {      /* start at the target: the oldest frames beyond it go */
        in_primed = 1;
        in_tail += level - USB_IN_TARGET < avail ? level - USB_IN_TARGET : 0;
        avail = in_head - in_tail;
        level = USB_IN_TARGET;
        in_level = (float)USB_IN_TARGET;
    }
    if (in_primed) {
        /* Averaged over about 32 packets: a SOF served late (behind the app's callback) makes the ring look a packet
         * fuller for a moment, which is not drift. */
        in_level += ((float)level - in_level) * (1.0f / 32.0f);
        if (in_level > (float)(USB_IN_TARGET + USB_IN_SLACK)) n = 49;   /* the codec runs fast against the computer */
        else if (in_level < (float)(USB_IN_TARGET - USB_IN_SLACK)) n = 47;
        if (avail < n) { in_primed = 0; usb_audio_in_underruns++; }
    }
    for (i = 0; i < n; i++) {
        if (in_primed) { *fifo = in_ring[in_tail % USB_IN_RING]; in_tail++; }
        else *fifo = 0;
    }
    csr->tx_csrl = MUSB_TXCSRL1_TXRDY;
    usb_audio_in_packets++;
}

/* In mode 0 both halves of the double-buffered FIFO are kept loaded, so a SOF served late (the USB interrupt waits
 * while the app's audio callback runs, up to a block) still finds a packet ready: 1 ms more latency for no gaps.
 * The other modes load one packet a frame (usb_audio_in_mode). */
static void service_in(void)
{
    musb_ep_csr_t *csr = &musb->indexed_csr;
    volatile uint32_t *fifo = &musb->fifo[USB_AUDIO_EP_IN & 0x0F];
    int k;
    u8 csrl;
    musb->index = USB_AUDIO_EP_IN & 0x0F;
    csrl = csr->tx_csrl;
    if (csrl & MUSB_TXCSRL1_UNDRN) {         /* cleared without writing TXPKTRDY back, which would commit a packet */
        csr->tx_csrl = (u8)(csrl & ~(MUSB_TXCSRL1_UNDRN | MUSB_TXCSRL1_TXRDY));
        usb_audio_in_late++;
    }
    for (k = 0; k < (in_mode ? 1 : 2) && !(csr->tx_csrl & MUSB_TXCSRL1_TXRDY); k++) load_in(fifo, csr);
}

static void uac_sof(uint8_t rhport, uint32_t frame)
{
    u8 index = musb->index;
    (void)rhport; (void)frame;
    if (out_alt) service_out();
    if (in_alt) service_in();
    musb->index = index;
}

/* ---- the class driver ---- */

static const tusb_desc_endpoint_t *out_ep, *in_ep;

static void stop(int itf)
{
    if (itf == ITF_OUT) { out_alt = 0; out_primed = 0; out_head = out_tail = 0; }
    else { in_alt = 0; in_primed = 0; in_head = in_tail = 0; }
    if (!out_alt && !in_alt) usbd_sof_enable(0, SOF_CONSUMER_AUDIO, false);
}

static void uac_init(void) {}

static void uac_reset(uint8_t rhport)
{
    (void)rhport;
    out_alt = in_alt = out_primed = in_primed = 0;
    out_head = out_tail = in_head = in_tail = 0;
    out_ep = in_ep = 0;
}

static uint16_t uac_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len)
{
    const uint8_t *p = (const uint8_t *)itf, *end = p + max_len;
    if (itf->bInterfaceClass != TUSB_CLASS_AUDIO || itf->bInterfaceSubClass != AUDIO_SUBCLASS_STREAMING ||
        (itf->bInterfaceNumber != ITF_OUT && itf->bInterfaceNumber != ITF_IN) || itf->bAlternateSetting != 0)
        return 0;
    p = tu_desc_next(p);                       /* past alternate setting 0, which has no endpoints */
    while (p < end && !(tu_desc_type(p) == TUSB_DESC_INTERFACE &&
                        ((const tusb_desc_interface_t *)p)->bInterfaceNumber != itf->bInterfaceNumber)) {
        if (tu_desc_type(p) == TUSB_DESC_ENDPOINT) {
            const tusb_desc_endpoint_t *ep = (const tusb_desc_endpoint_t *)p;
            TU_ASSERT(usbd_edpt_iso_alloc(rhport, ep->bEndpointAddress, PACKET_MAX), 0);
            if (itf->bInterfaceNumber == ITF_OUT) out_ep = ep; else in_ep = ep;
        }
        p = tu_desc_next(p);
    }
    return (uint16_t)(p - (const uint8_t *)itf);
}

static int put_dec(char *line, int n, u32 v)    /* v in decimal at line[n]; returns the end */
{
    char digits[10];
    int k = 0;
    do { digits[k++] = (char)('0' + v % 10u); v /= 10u; } while (v && k < 5);
    while (k) line[n++] = digits[--k];
    return n;
}

/* A request refused: logged (the first few), and a stall. */
static bool refuse(tusb_control_request_t const *request)
{
    static const char hex[] = "0123456789ABCDEF";
    static u32 logged;
    char line[] = "USB REQ tt rr vvvv iiii";
    u32 fields[4] = {request->bmRequestType, request->bRequest, request->wValue, request->wIndex};
    int at[4] = {8, 11, 14, 19}, digits[4] = {2, 2, 4, 4}, f, d;
    if (logged >= 8) return false;
    logged++;
    for (f = 0; f < 4; f++)
        for (d = 0; d < digits[f]; d++) line[at[f] + d] = hex[(fields[f] >> (4 * (digits[f] - 1 - d))) & 15];
    log_line(line);
    return false;
}

/* The sampling frequency control of an endpoint (UAC1 5.2.3.2.3.1): 3 bytes, little-endian Hz. One rate. */
static bool sampling_frequency(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    static uint8_t rate[3];
    if (request->wLength != 3) return refuse(request);
    if (request->bRequest == AUDIO10_CS_REQ_SET_CUR) {
        if (stage == CONTROL_STAGE_SETUP) return tud_control_xfer(rhport, request, rate, 3);
        if (stage == CONTROL_STAGE_DATA && (rate[0] | rate[1] << 8 | rate[2] << 16) != 48000u)
            refuse(request);                  /* logged; accepted all the same, as refusing mid-transfer would hang */
        return true;
    }
    if (request->bRequest == AUDIO10_CS_REQ_GET_CUR || request->bRequest == AUDIO10_CS_REQ_GET_MIN ||
        request->bRequest == AUDIO10_CS_REQ_GET_MAX || request->bRequest == AUDIO10_CS_REQ_GET_RES) {
        if (stage != CONTROL_STAGE_SETUP) return true;
        rate[0] = 48000u & 0xFF; rate[1] = (48000u >> 8) & 0xFF; rate[2] = 0;
        if (request->bRequest == AUDIO10_CS_REQ_GET_RES) rate[0] = rate[1] = 0;   /* no steps: one rate */
        return tud_control_xfer(rhport, request, rate, 3);
    }
    return refuse(request);
}

static bool uac_control(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    u32 itf = tu_u16_low(request->wIndex), alt = request->wValue;
    static uint8_t current;
    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_CLASS &&
        request->bmRequestType_bit.recipient == TUSB_REQ_RCPT_ENDPOINT &&
        (itf == USB_AUDIO_EP_OUT || itf == USB_AUDIO_EP_IN) && tu_u16_high(request->wValue) == 0x01)
        return sampling_frequency(rhport, stage, request);
    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_STANDARD ||
        request->bmRequestType_bit.recipient != TUSB_REQ_RCPT_INTERFACE || (itf != ITF_OUT && itf != ITF_IN))
        return stage == CONTROL_STAGE_SETUP ? refuse(request) : false;
    if (request->bRequest == TUSB_REQ_GET_INTERFACE) {
        if (stage != CONTROL_STAGE_SETUP) return true;
        current = itf == ITF_OUT ? out_alt : in_alt;
        return tud_control_xfer(rhport, request, &current, 1);
    }
    if (request->bRequest != TUSB_REQ_SET_INTERFACE) return false;
    if (stage != CONTROL_STAGE_SETUP) return true;
    if (alt > 1) return false;
    {
        const tusb_desc_endpoint_t *ep = itf == ITF_OUT ? out_ep : in_ep;
        u32 gie = irq_global_disable();
        int was_streaming = itf == ITF_OUT ? out_alt : in_alt;
        stop((int)itf);
        if (alt == 1 && ep) {
            u8 index = musb->index;
            usbd_edpt_iso_activate(rhport, ep);
            musb->index = tu_edpt_number(ep->bEndpointAddress);
            if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN) {
                musb->intr_txen &= (u16)~(1u << tu_edpt_number(ep->bEndpointAddress));   /* served on SOF */
                musb->indexed_csr.tx_csrh |= 0x20;                       /* TXCSRH MODE: transmit */
                in_mode = (u8)(usb_audio_in_mode & 3);
                if (in_mode == 3) musb->fifo_size[0] &= (u8)~MUSB_FIFOSZ_DOUBLE_PACKET;   /* TXFIFOSZ: one packet */
                if (in_mode <= 1) musb->power |= MUSB_POWER_ISOUP;
                else musb->power &= (u8)~MUSB_POWER_ISOUP;
                in_late_at = usb_audio_in_late;
                in_underruns_at = usb_audio_in_underruns;
            } else {
                musb->intr_rxen &= (u16)~(1u << tu_edpt_number(ep->bEndpointAddress));
            }
            musb->index = index;
            if (itf == ITF_OUT) out_alt = 1; else in_alt = 1;
            usbd_sof_enable(rhport, SOF_CONSUMER_AUDIO, true);
        }
        if (gie) irq_global_enable();
        log_line(itf == ITF_OUT ? (alt ? "USB AUDIO OUT ON" : "USB AUDIO OUT OFF")
                                : (alt ? "USB AUDIO IN ON" : "USB AUDIO IN OFF"));
        if (itf == ITF_IN && alt == 0 && was_streaming) {   /* the stream's mode, underruns (UNDERRUN), ring gaps */
            char line[24] = "IN M";
            int n = 4;
            line[n++] = (char)('0' + in_mode);
            line[n++] = ' '; line[n++] = 'L';
            n = put_dec(line, n, usb_audio_in_late - in_late_at);
            line[n++] = ' '; line[n++] = 'U';
            n = put_dec(line, n, usb_audio_in_underruns - in_underruns_at);
            line[n] = 0;
            log_line(line);
        }
    }
    return tud_control_status(rhport, request);
}

static bool uac_xfer(uint8_t rhport, uint8_t ep, xfer_result_t result, uint32_t bytes)
{
    (void)rhport; (void)ep; (void)result; (void)bytes;
    return true;                               /* never: the endpoints are served on SOF */
}

static const usbd_class_driver_t uac_driver = {
    .name = "ENAGA AUDIO",
    .init = uac_init,
    .deinit = 0,
    .reset = uac_reset,
    .open = uac_open,
    .control_xfer_cb = uac_control,
    .xfer_cb = uac_xfer,
    .xfer_isr = 0,
    .sof = uac_sof,
};

usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *count)
{
    *count = 1;
    return &uac_driver;
}
