#include "kernel.h"
#include "tusb.h"
#include "usb_c6748.h"

/* The debug port: a second USB MIDI cable ("Enaga Debug") on the MIDI endpoints, for getting Enaga's log, its
 * state and the screen out of a unit without photographing the STATUS page. tools/usb_debug.py is the computer's
 * end. Cable 0 stays the app's MIDI port, untouched: the debug port sends only when cable 0 has nothing waiting,
 * and only after the computer has asked for something, so a computer that never opens the port never gets its
 * traffic. It never fills more than DEBUG_SHARE bytes of TinyUSB's MIDI IN buffer, so MIDI waits behind at
 * most that much debug data (two full-speed packets) and the two packets the FIFO holds: about 0.3 ms.
 *
 * Every message is SysEx with the non-commercial ID: F0 7D 45 <command> <data> F7 (45 is 'E'). Data bytes are
 * 7-bit: text as it is (log lines are ASCII; anything above 7F goes as '?'), numbers as groups of 7 bits, lowest
 * first (a 32-bit word in 5 bytes).
 *
 *   From the computer                         The unit's answer
 *   00 HELLO                                  00 "ENAGA <version> <protocol version>"
 *   01 LOG ON                                 01 <sequence: 3 bytes> <text>, for each line kept (LOG_LINES) and
 *                                                then each new one as it is logged; 02 <count: 3 bytes> when
 *                                                lines were overwritten before they could be sent
 *   02 LOG OFF                                -
 *   03 STATUS                                 03 <text>, a "NAME VALUE" line per item, then 03 alone (the end)
 *   04 SCREEN                                 04 <row> <128 pixels, 0-15>, for rows 0-127, of the frame shown last
 *   05 PEEK <address: 5 bytes> <words: 1>     05 <address: 5 bytes> <word: 5 bytes> ..., 1-32 words read from
 *                                                RAM (DDR, shared RAM, L2) or the USB0 wrapper's and SYSCFG0's
 *                                                registers; 7F <command> "<reason>" if not allowed
 *   06 KEY <key> <1 down, 0 up>               06, once a frame has been drawn after it (see below)
 *   07 TURN <encoder 0-4> <detents: 2>        07, likewise; detents are 14-bit two's complement
 *   08 PAD <pad 0-14> <velocity>              08, likewise; velocity 1-127 presses, 0 releases
 *   09 PRESSURE <pad> <0-127>                 09, likewise; the pad must be held from here
 *   0A RELEASE                                0A, likewise: every key and pad held from here let go
 *   anything else                             7F <command> "UNKNOWN"
 *
 * Remote input (06-0A) goes through main.c's remote_*: keys and pads as if pressed on the unit, keys 0-47 as
 * enaga_key and 48 for POWER, which is refused ("NOT ALLOWED") unless F1 is down, as it would switch the unit off.
 * The answer waits until the input has been delivered and a frame drawn after it, so a SCREEN asked for next
 * shows its effect. Remote input is let go when the computer releases it, stops reading (below), or USB goes,
 * when anything on the unit is pressed or turned, and after REMOTE_HOLD_MS without any request: a program killed
 * while holding a key never says so, and Windows goes on reading the port (unit session, 2026-11-25), so a
 * script holding longer sends something meanwhile (usb_debug.py's Client.wait sends HELLO each second).
 *
 * One request is answered at a time: a new one waits for the message being sent to finish, then replaces the
 * rest of the answer under way (LOG ON and OFF act at once). If nothing can be sent for DEBUG_STALL_MS (the computer stopped reading), the log stops and the answer
 * is dropped (the message under way still ends whole), so a forgotten port does not hold up the app's MIDI for
 * long. */

#define DEBUG_PROTOCOL  2
#define DEBUG_CABLE     1
#define DEBUG_STALL_MS  2000u
#define REMOTE_HOLD_MS  5000u                  /* remote input let go after this long without a request */
#define DEBUG_SHARE     128u                   /* bytes of TinyUSB's MIDI IN buffer the debug port may fill */
#define REQUEST_MAX     16
#define MESSAGE_MAX     (8 + 128 + 2)          /* the longest: a screen row */
#define PEEK_MAX        32u

enum { HELLO, LOG_ON, LOG_OFF, STATUS, SCREEN, PEEK, KEY, TURN, PAD, PRESSURE, RELEASE, LOST = 2, ERROR = 0x7F };

u32 usb_debug_requests, usb_debug_messages;    /* counted for tests and STATUS */

static u8 request[REQUEST_MAX];                /* the SysEx being received, from its command byte */
static u32 request_bytes;
static int receiving;
static u8 pending[REQUEST_MAX];                /* a request received, waiting for the current message to finish */
static u32 pending_bytes;
static int has_pending;

static u8 message[MESSAGE_MAX];                /* the message being sent */
static u32 message_bytes, message_sent;

static int log_on;
static u32 log_next;                           /* the next log line to send: an index into log_count's count */
static int answering = -1;                     /* the request being answered, or -1 */
static u32 answer_step;                        /* how far: the status line or screen row */
static u8 screen[CANVAS_W * CANVAS_H];         /* the frame being sent, copied when asked for */
static u32 peek_address, peek_words;
static u32 progress_ms;                        /* when something was last sent */
static u32 input_pass;                         /* kernel_passes when the remote input being answered was given */
static u32 request_ms;                         /* when the last request came */

/* ---- Building messages ---- */

static void begin(int command)
{
    message[0] = 0xF0; message[1] = 0x7D; message[2] = 0x45; message[3] = (u8)command;
    message_bytes = 4;
    message_sent = 0;
}

static void add(u8 byte) { if (message_bytes < MESSAGE_MAX - 1) message[message_bytes++] = (u8)(byte & 0x7F); }

static void add_number(u32 value, int bytes) { while (bytes--) { add((u8)(value & 0x7F)); value >>= 7; } }

static void add_text(const char *s) { while (*s) { add((u8)(*s & 0x80 ? '?' : *s)); s++; } }

static void add_dec(u32 value)
{
    char digits[11];
    int n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value);
    while (n) add((u8)digits[--n]);
}

static void add_signed(int value)
{
    if (value < 0) { add('-'); add_dec((u32)-value); } else add_dec((u32)value);
}

static void add_hex(u32 value)
{
    int shift;
    for (shift = 28; shift >= 0; shift -= 4) add((u8)"0123456789ABCDEF"[(value >> shift) & 15]);
}

static void finish(void) { message[message_bytes++] = 0xF7; usb_debug_messages++; }

static void error(int command, const char *reason)
{
    begin(ERROR);
    add((u8)command);
    add_text(reason);
    finish();
}

/* ---- Sending: the message as USB MIDI event packets on the debug cable, as many as TinyUSB takes ---- */

static int send_some(void)                     /* 1 when the whole message has gone */
{
    while (message_sent < message_bytes) {
        u32 left = message_bytes - message_sent, n = left > 3 ? 3 : left;
        u8 packet[4] = {0, 0, 0, 0};
        u32 i;
        int ends = message[message_sent + n - 1] == 0xF7;
        /* Leave the rest of the buffer to MIDI: what cable 0 sends waits behind at most DEBUG_SHARE bytes of ours
         * (two full-speed packets), even during a screen. */
        if (tud_midi_n_write_available(0) < CFG_TUD_MIDI_TX_BUFSIZE - DEBUG_SHARE + 4) return 0;   /* only the last byte is F7: data bytes are 7-bit */
        /* CIN 4: SysEx starts or continues (3 bytes); 5, 6, 7: it ends with 1, 2 or 3 bytes. */
        packet[0] = (u8)(DEBUG_CABLE << 4 | (ends ? 4 + n : 4));
        for (i = 0; i < n; i++) packet[1 + i] = message[message_sent + i];
        if (!tud_midi_packet_write(packet)) return 0;
        message_sent += n;
        progress_ms = timer_ms();
    }
    return 1;
}

/* ---- The answers ---- */

/* The STATUS answer's lines, one per step; 0 past the last. */
static int status_line(u32 step)
{
    begin(STATUS);
    switch (step) {
    case 0: add_text("VERSION " ENAGA_VERSION); break;
    case 1: add_text("UPTIME_MS "); add_dec(timer_ms()); break;
    case 2: add_text("APP "); add_text(app_name[0] ? app_name : "-"); break;
    case 3: add_text("APP_STATUS "); add_text(app_status); break;
    case 4: add_text("CRASH "); add_text(crash_reason[0] ? crash_reason : "-"); break;
    case 5: add_text("CRASH_PC "); add_hex(crash_pc); add_text(" EFR "); add_hex(crash_efr); add_text(" IERR ");
            add_hex(crash_ierr); break;
    case 6: add_text("AUDIO "); add_signed(audio_status); add_text(" BLOCKS "); add_dec(audio_blocks);
            add_text(" OVERRUNS "); add_dec(audio_overruns); break;
    case 7: add_text("AUDIO_LOAD_PEAK "); add_dec(audio_cycles_peak / (AUDIO_BUDGET_CYCLES / 1000u)); break;
    case 8: add_text("USB_AUDIO_OUT "); add_dec(usb_audio_out_packets); add_text(" OVERRUNS ");
            add_dec(usb_audio_out_overruns); add_text(" UNDERRUNS "); add_dec(usb_audio_out_underruns); break;
    case 9: add_text("USB_AUDIO_IN "); add_dec(usb_audio_in_packets); add_text(" LATE "); add_dec(usb_audio_in_late);
            add_text(" UNDERRUNS "); add_dec(usb_audio_in_underruns); break;
    case 10: add_text("USB_DRIVE READ "); add_dec(usb_drive_read_sectors); add_text(" WRITTEN ");
             add_dec(usb_drive_written_sectors); add_text(" ERRORS "); add_dec(usb_drive_errors); break;
    case 11: add_text("USB_MIDI_LOST "); add_dec(usb_tx_lost); break;
    /* Where USB DRIVE's time went since the page opened (usb.c): per direction the callbacks, the ms in the SD
     * driver, and the ms between callbacks; then the drive's packets and the ms the host took to take or send
     * them (musb_enaga.h). */
    case 16: add_text("USB_DRIVE_TIME R "); add_dec(usb_drive_calls[0]); add_text(" SD_MS ");
             add_dec(usb_drive_sd_us[0] / 1000u); add_text(" GAP_MS "); add_dec(usb_drive_gap_us[0] / 1000u);
             add_text(" W "); add_dec(usb_drive_calls[1]); add_text(" SD_MS "); add_dec(usb_drive_sd_us[1] / 1000u);
             add_text(" GAP_MS "); add_dec(usb_drive_gap_us[1] / 1000u); break;
    case 17: add_text("USB_DRIVE_PACKETS IN "); add_dec(enaga_musb_in_packets[2]); add_text(" WAIT_MS ");
             add_dec(enaga_musb_in_wait_us[2] / 1000u); add_text(" OUT "); add_dec(enaga_musb_out_packets[2]);
             add_text(" GAP_MS "); add_dec(enaga_musb_out_gap_us[2] / 1000u); break;
    case 12: add_text("MIDI IN "); add_dec(midi_messages_in); add_text(" OUT "); add_dec(midi_messages_out);
             add_text(" DROPPED "); add_dec(midi_dropped_out); break;
    case 13: add_text("SD READ "); add_dec(sd_reads); add_text(" WRITTEN "); add_dec(sd_writes); add_text(" ERROR ");
             add_signed(sd_io_error); add_text(" COMMAND "); add_dec(sd_io_command); add_text(" STATUS ");
             add_hex(sd_io_status); break;
    case 14: add_text("BATTERY "); add_signed(battery_level()); break;
    case 15: add_text("LOG_COUNT "); add_dec(log_count); break;
    case 18: break;                            /* the end: the command alone */
    default: return 0;
    }
    finish();
    return 1;
}

static int screen_row(u32 row)
{
    u32 x;
    if (row >= CANVAS_H) return 0;
    begin(SCREEN);
    add((u8)row);
    for (x = 0; x < CANVAS_W; x++) add(screen[row * CANVAS_W + x]);
    finish();
    return 1;
}

static int peek_allowed(u32 address, u32 words)
{
    u32 end = address + 4u * words;
    if (address & 3 || !words || words > PEEK_MAX || end < address) return 0;
    return (address >= 0xC0000000u && end <= 0xC8000000u)      /* DDR */
        || (address >= 0x80000000u && end <= 0x80020000u)      /* shared RAM */
        || (address >= 0x11800000u && end <= 0x11840000u)      /* L2 */
        || (address >= 0x01E00000u && end <= 0x01E00400u)      /* the USB0 wrapper (not the core: reads clear) */
        || (address >= 0x01C14000u && end <= 0x01C14200u);     /* SYSCFG0 */
}

static void peek_message(void)
{
    u32 i;
    begin(PEEK);
    add_number(peek_address, 5);
    for (i = 0; i < peek_words; i++) add_number(REG32(peek_address + 4u * i), 5);
    finish();
}

/* The next message of the answer under way into `message`; 0 when it is done, -1 while it waits for a frame. */
static int next_answer(void)
{
    switch (answering) {
    case KEY: case TURN: case PAD: case PRESSURE: case RELEASE:
        if (answer_step) return 0;
        /* Keys reach the app at the next pass's poll_inputs, pads at once: a frame drawn in a later pass than the
         * request's comes after either. */
        if ((int)(kernel_drawn - input_pass) <= 0) return -1;
        answer_step = 1;
        begin(answering);
        finish();
        return 1;
    case STATUS: return status_line(answer_step++);
    case SCREEN: return screen_row(answer_step++);
    case PEEK: if (answer_step++) return 0; peek_message(); return 1;
    case HELLO:
        if (answer_step++) return 0;
        begin(HELLO);
        add_text("ENAGA " ENAGA_VERSION " ");
        add_dec(DEBUG_PROTOCOL);
        finish();
        return 1;
    default: return 0;
    }
}

/* The next log message into `message`, or 0 if none is waiting. */
static int next_log(void)
{
    u32 oldest = log_count > LOG_LINES ? log_count - LOG_LINES : 0;
    if (!log_on || log_next == log_count) return 0;
    if (log_next < oldest) {                   /* overwritten before they went */
        begin(LOST);
        add_number(oldest - log_next, 3);
        finish();
        log_next = oldest;
        return 1;
    }
    begin(LOG_ON);
    add_number(log_next, 3);
    add_text(log_lines[log_next % LOG_LINES]);
    finish();
    log_next++;
    return 1;
}

/* ---- Requests ---- */

static void start(const u8 *request, u32 request_bytes)
{
    int command = request_bytes ? request[0] : -1;
    switch (command) {
    case LOG_ON:
        log_on = 1;
        log_next = log_count > LOG_LINES ? log_count - LOG_LINES : 0;
        break;
    case LOG_OFF:
        log_on = 0;
        break;
    case HELLO:
    case STATUS:
        answering = command;
        answer_step = 0;
        break;
    case SCREEN:
        memcpy(screen, canvas, sizeof screen);
        answering = command;
        answer_step = 0;
        break;
    case KEY: case TURN: case PAD: case PRESSURE: case RELEASE: {
        int refused = 0;
        if (command == RELEASE) remote_release();
        else if (request_bytes != (command == TURN ? 4u : 3u)) refused = 1;
        else if (command == KEY) refused = remote_key(request[1], request[2]) < 0;
        else if (command == TURN) {
            int detents = request[2] | request[3] << 7;
            if (request[1] >= 5) refused = 1;
            else remote_turn(request[1], detents & 0x2000 ? detents - 0x4000 : detents);
        } else if (command == PAD) refused = remote_pad(request[1], request[2]) < 0;
        else refused = remote_pressure(request[1], request[2]) < 0;
        if (refused) {
            error(command, "NOT ALLOWED");
            answering = -1;
            return;
        }
        input_pass = kernel_passes;
        answering = command;
        answer_step = 0;
        break;
    }
    case PEEK: {
        u32 address = 0, i;
        for (i = 0; i < 5 && 1 + i < request_bytes; i++) address |= (u32)request[1 + i] << (7 * i);
        if (request_bytes != 7 || !peek_allowed(address, request[6])) {
            error(PEEK, "NOT ALLOWED");
            answering = -1;
            return;
        }
        peek_address = address;
        peek_words = request[6];
        answering = PEEK;
        answer_step = 0;
        break;
    }
    default:
        error(command < 0 ? 0 : command, "UNKNOWN");
        answering = -1;
        return;
    }
    message_bytes = message_sent = 0;          /* a new request replaces what was being sent */
}

void usb_debug_byte(u8 byte)                   /* a byte the computer sent on the debug cable */
{
    if (byte == 0xF0) { receiving = 1; request_bytes = 0; return; }
    if (!receiving) return;
    if (byte == 0xF7) {
        receiving = 0;
        /* 7D 45 <command> ...: anything else on this cable is not for us */
        if (request_bytes >= 3 && request[0] == 0x7D && request[1] == 0x45) {
            u32 i;
            for (i = 2; i < request_bytes; i++) pending[i - 2] = request[i];
            pending_bytes = request_bytes - 2;
            has_pending = 1;
            request_ms = timer_ms();
            usb_debug_requests++;
        }
        return;
    }
    if (byte & 0x80) { receiving = 0; return; }   /* any other status byte ends a SysEx */
    if (request_bytes < REQUEST_MAX) request[request_bytes++] = byte;
}

/* Main loop, after cable 0's MIDI out has gone: sends what TinyUSB takes. */
void usb_debug_poll(void)
{
    if (!tud_mounted()) {
        log_on = 0; answering = -1; message_bytes = message_sent = 0; receiving = 0; has_pending = 0;
        if (remote_active) remote_release();
        return;
    }
    if (remote_active && !has_pending && timer_ms() - request_ms > REMOTE_HOLD_MS) remote_release();
    if (!log_on && answering < 0 && !has_pending && message_sent == message_bytes) { progress_ms = timer_ms(); return; }
    for (;;) {
        if (message_sent < message_bytes) {
            if (!send_some()) break;
            continue;
        }
        if (has_pending) {                     /* between messages: the request waiting starts */
            has_pending = 0;
            start(pending, pending_bytes);
            continue;
        }
        int next = next_answer();
        if (next > 0) continue;
        if (next == 0) answering = -1;         /* -1: the answer waits for a frame; log lines go meanwhile */
        if (!next_log()) { progress_ms = timer_ms(); return; }
    }
    if (timer_ms() - progress_ms > DEBUG_STALL_MS) {   /* the computer stopped reading: nothing more after the */
        log_on = 0;                                    /* message under way, which ends whole if it ever reads */
        answering = -1;
        if (remote_active) remote_release();           /* and nothing stays held by a computer gone quiet */
    }
}
