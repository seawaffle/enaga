#include "kernel.h"

/* MIDI (docs/design.md "E4 plan", step 3). Bytes from the DIN input (uart0.c) are parsed in the main loop into
 * whole messages for the app: ENAGA_EVENT_MIDI for everything but SysEx, the app's midi_sysex callback for SysEx in
 * chunks. The parser follows the MIDI 1.0 specification:
 *   - running status: data bytes after a channel message reuse its status; system common and SysEx clear it;
 *   - real-time bytes (F8-FF) are messages of their own anywhere, even inside another message or a SysEx;
 *   - a SysEx ends at F7, or at any other non-real-time status byte (then CUT, and that byte starts a message);
 *   - a note on with velocity 0 is delivered as a note off with velocity 0;
 *   - undefined status bytes (F4, F5, F9, FD) are dropped, and data bytes with no status before them;
 *   - active sensing (FE) is not delivered: once seen, a gap of over 300 ms gives the app All Notes Off (CC 123)
 *     on every channel, as the specification asks of a receiver, and sensing waits for the next FE.
 * MIDI THRU (a setting) copies each complete DIN message, except SysEx, to the DIN output, queued like the app's
 * own messages so that the two never interleave mid-message.
 *
 * Ports: DIN (uart0.c) and USB (usb.c, step 5), each with its own parser. The MIDI IN setting says which reach the
 * app (and whose MIDI clock the clock follows); MIDI OUT says where messages sent to "the user's routing" (ports 0)
 * go, as the original firmware's IN FROM / OUT TO. */
#define SENSE_TIMEOUT_MS 300u

typedef struct {
    u8 status, need, count, data[2];
    u8 in_sysex, sysex_started;
    u32 sysex_bytes;
    u8 sysex[ENAGA_SYSEX_CHUNK];
    u8 sensing;
    u32 sensed_ms;
} midi_port_t;

static midi_port_t din, usb;
int midi_channel_setting;                     /* MIDI CH, 0-15 */
int midi_in_setting = ENAGA_MIDI_DIN | ENAGA_MIDI_USB;    /* MIDI IN: ports whose messages reach the app */
int midi_out_setting = ENAGA_MIDI_DIN | ENAGA_MIDI_USB;   /* MIDI OUT: where ports 0 sends */
int midi_thru;                                /* MIDI THRU on */
u32 midi_messages_in, midi_messages_out, midi_sysex_in, midi_dropped_out;

static void deliver(int port, u8 status, u8 a, u8 b, u32 ms)
{
    enaga_event_t e;
    e.type = ENAGA_EVENT_MIDI;
    e.id = (u16)port;
    e.value = (int)(status | (u32)a << 8 | (u32)b << 16);
    e.ms = ms;
    if (!(midi_in_setting & port)) return;
    midi_messages_in++;
    apps_event(&e);
}

static void thru(int port, const u8 *m, u32 bytes)
{
    if (!midi_thru || port != ENAGA_MIDI_DIN) return;
    if (bytes == 1 && m[0] >= 0xF8) { if (uart0_send_realtime(m[0]) < 0) midi_dropped_out++; }
    else if (uart0_send(m, bytes) < 0) midi_dropped_out++;
}

static void message(midi_port_t *p, int port, u32 ms)
{
    u8 m[3];
    u32 n = 1u + p->count;
    m[0] = p->status; m[1] = p->data[0]; m[2] = p->data[1];
    thru(port, m, n);
    if ((m[0] & 0xF0) == 0x90 && m[2] == 0) m[0] = (u8)(0x80 | (m[0] & 0x0F));
    deliver(port, m[0], n > 1 ? m[1] : 0, n > 2 ? m[2] : 0, ms);
}

static void sysex_flush(midi_port_t *p, int port, u32 flags)
{
    if (!p->sysex_started) flags |= ENAGA_SYSEX_START;
    p->sysex_started = 1;
    if (midi_in_setting & port) apps_sysex(port, p->sysex, p->sysex_bytes, flags);
    p->sysex_bytes = 0;
}

static void sysex_byte(midi_port_t *p, int port, u8 b)
{
    p->sysex[p->sysex_bytes++] = b;
    if (p->sysex_bytes == ENAGA_SYSEX_CHUNK) sysex_flush(p, port, 0);
}

static unsigned data_bytes(u8 status)
{
    if (status < 0xF0) return (status & 0xE0) == 0xC0 ? 1u : 2u;   /* Cn, Dn: one */
    if (status == 0xF2) return 2u;
    if (status == 0xF1 || status == 0xF3) return 1u;
    return 0u;                                /* F6 */
}

static void parse(midi_port_t *p, int port, u8 b, u32 ms)
{
    if (b >= 0xF8) {                          /* real-time: never disturbs the message around it */
        if (b == 0xFE) { p->sensing = 1; p->sensed_ms = ms; return; }
        if (b == 0xF9 || b == 0xFD) return;
        thru(port, &b, 1);
        deliver(port, b, 0, 0, ms);
        return;
    }
    if (b & 0x80) {
        if (p->in_sysex) {                    /* any status ends a SysEx; only F7 ends it properly */
            if (b == 0xF7) sysex_byte(p, port, b);
            midi_sysex_in++;
            sysex_flush(p, port, ENAGA_SYSEX_END | (b == 0xF7 ? 0u : ENAGA_SYSEX_CUT));
            p->in_sysex = 0;
            if (b == 0xF7) return;
        }
        p->status = 0;
        p->count = 0;
        if (b == 0xF0) {
            p->in_sysex = 1; p->sysex_started = 0; p->sysex_bytes = 0;
            sysex_byte(p, port, b);
            return;
        }
        if (b == 0xF4 || b == 0xF5 || b == 0xF7) return;   /* undefined, or a stray end of SysEx */
        p->status = b;
        p->need = (u8)data_bytes(b);
        if (p->need == 0) { message(p, port, ms); p->status = 0; }   /* F6 */
        return;
    }
    if (p->in_sysex) { sysex_byte(p, port, b); return; }
    if (!p->status) return;                   /* data with no status: dropped */
    p->data[p->count++] = b;
    if (p->count < p->need) return;
    message(p, port, ms);
    p->count = 0;
    if (p->status >= 0xF0) p->status = 0;     /* system common: no running status */
}

int midi_poll(u32 ms)                         /* main loop: returns the number of bytes taken */
{
    u8 b;
    int n = 0, ch;
    while (uart0_read(&b)) { parse(&din, ENAGA_MIDI_DIN, b, ms); n++; }
    if (din.sensing && ms - din.sensed_ms > SENSE_TIMEOUT_MS) {
        din.sensing = 0;
        for (ch = 0; ch < 16; ch++) deliver(ENAGA_MIDI_DIN, (u8)(0xB0 | ch), 123, 0, ms);
    }
    if (usb.sensing && ms - usb.sensed_ms > SENSE_TIMEOUT_MS) {
        usb.sensing = 0;
        for (ch = 0; ch < 16; ch++) deliver(ENAGA_MIDI_USB, (u8)(0xB0 | ch), 123, 0, ms);
    }
    return n;
}

void midi_usb_byte(u8 b, u32 ms)              /* usb.c, main loop: one received USB MIDI byte */
{
    if (midi_in_setting & ENAGA_MIDI_USB) {   /* MIDI clock from USB, timed here (a main-loop pass late) */
        u32 gie = irq_global_disable();
        clock_midi_byte(b);
        if (gie) irq_global_enable();
    }
    parse(&usb, ENAGA_MIDI_USB, b, ms);
}

/* Bytes out to `ports` (0 = the MIDI OUT setting): all or nothing across the ports asked for, so a full queue holds
 * the message back for every port and the sender can wait for room. USB counts only while the host is taking its
 * bytes: one that has stopped (usb_midi_stalled) would otherwise hold up DIN for good, so it gets what fits and
 * drops the rest (usb_tx_lost). A single real-time byte goes ahead of the DIN queue. Main or interrupt context. */
int midi_out_bytes(u32 ports, const u8 *data, u32 bytes)
{
    int realtime = bytes == 1 && data[0] >= 0xF8;
    if (ports == 0) ports = (u32)midi_out_setting;
    if ((ports & ENAGA_MIDI_DIN) && !realtime && uart0_tx_free() < bytes) return -1;
    if ((ports & ENAGA_MIDI_USB) && usb_configured() && !usb_midi_stalled() && usb_midi_tx_free() < bytes) return -1;
    if (ports & ENAGA_MIDI_DIN) {
        if (realtime ? uart0_send_realtime(data[0]) < 0 : uart0_send(data, bytes) < 0) return -1;
    }
    if (ports & ENAGA_MIDI_USB) usb_midi_send(data, bytes);
    return 0;
}

/* An app's midi_send: whole messages only, all or nothing. ports 0 = the user's routing (MIDI OUT). */
int midi_send(u32 ports, const u8 *data, u32 bytes)
{
    if (!bytes || !data || !(data[0] & 0x80)) return -1;
    if (midi_out_bytes(ports, data, bytes) < 0) return -1;
    midi_messages_out++;
    return 0;
}
