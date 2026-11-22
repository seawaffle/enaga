#include "kernel.h"

/* The tempo clock, transport and SYNC (docs/design.md "E4 plan", step 4). One clock for the whole unit, counted in
 * ticks of ENAGA_CLOCK_PPQN (96) to the quarter note, as Lofi-12 XT and CyDrums count their steps.
 *
 * It advances once per audio block, in the audio interrupt before the app's callback, so the app can ask which ticks
 * fall in the block it is computing and at which frame (clock_ticks). Its source is the CLOCK SRC setting:
 *   INT   the app drives it: tempo, start, stop, continue, locate.
 *   MIDI  MIDI clock at MIDI IN: 4 ticks per F8 (24 to the quarter note), START / STOP / CONTINUE and Song Position
 *         Pointer. Taken from the bytes in UART0's interrupt (clock_midi_byte), in their order, not from the main
 *         loop's parser. The tempo is the mean interval of the last 24 clocks, kept while stopped (many
 *         sequencers send clock all the time), so a START runs at the right tempo from its first tick.
 *   SYNC  pulses at SYNC IN (GP7[8], the edge POL IN picks, GPIO bank 7 interrupt): 48 ticks per pulse (2 to the quarter
 *         note). The first pulse starts the clock; none for over 2.5 intervals (at least 100 ms; 2 s before the
 *         second pulse gives an interval) stops it. The tempo is the mean of the last 2 intervals.
 * Following an outside clock is as the original firmware does it (smpltrek-re HW-41): the clock runs at the derived
 * tempo but never passes the tick of the next clock or pulse, waiting for it; when an early one finds it more than
 * one clock behind, it catches up at once, every tick still given out.
 *
 * Out, whatever the source (so the unit can also bridge SYNC to MIDI): with CLOCK OUT on, MIDI clock every 4 ticks,
 * START, STOP, and Song Position Pointer then CONTINUE (as Lofi-12 XT); with SYNC OUT on, a 15 ms pulse on GP7[9]
 * every 48 ticks, no closer than 60 ms (the original firmware's pulse, HW-27). Both are sent from the audio
 * interrupt, so they are exact on average and late by less than one block (1.33 ms).
 *
 * Polarity, as the original firmware's POLARITY IN and OUT (smpltrek-re HW-27; the labels from its settings row):
 * POL IN RISE (0, the default) arms the *falling* edge of GP7[8] and FALL the rising one, so the board presumably
 * inverts SYNC IN between the jack and the pin; Enaga keeps the stock meaning of each label rather than guess. POL
 * OUT RISE (0) idles GP7[9] low and pulses it high, FALL idles high and pulses low. Neither is checked on the unit. */

#define FRAMES_PER_SECOND 48000.0f
#define CYCLES_PER_FRAME (TICKS_PER_MS / 48u)           /* 7,750 */
#define TICKS_PER_CLOCK 4u                               /* 96 / 24 */
#define TICKS_PER_PULSE 48u                              /* 96 / 2 */
#define MIDI_MEAN 24u
#define PULSE_MEAN 2u
#define PULSE_MS 15u
#define PULSE_GAP_MS 60u
#define TEMPO_MIN 20.0f
#define TEMPO_MAX 300.0f
#define BLOCK_TICKS 32u

#define GPIO_PAIR3   (GPIO + 0x10 + 3 * 0x28)           /* banks 6 and 7 */
#define GPIO_BINTEN  (GPIO + 0x08)
#define SYNC_IN_BIT  (1u << (16 + 8))                   /* GP7[8] */

int clock_source_setting = ENAGA_CLOCK_INTERNAL;
int clock_out_setting, sync_out_setting = 1;
int sync_in_polarity_setting, sync_out_polarity_setting;   /* POL IN / POL OUT: 0 RISE, 1 FALL */

static volatile u8 running, waiting;                    /* waiting: started by MIDI, no clock yet */
static volatile u32 whole;                              /* position: whole + frac ticks */
static volatile float frac;
static volatile u32 next_tick;                          /* the first tick not yet given out */
static volatile u32 cap;                                /* outside clock: the position may not pass this */
static volatile float tempo = 120.0f;                   /* BPM */
static volatile float rate;                             /* ticks per frame while following an outside clock */

static enaga_clock_tick_t block_ticks[BLOCK_TICKS];
static volatile u32 block_count;

/* Outside clock measurements, written in the UART0 and GPIO interrupts, read in the audio interrupt. */
static u32 midi_intervals[MIDI_MEAN], midi_interval_count, midi_last_cycle;
static volatile u32 midi_clocks;                         /* F8s since START / CONTINUE */
static u8 spp_need, spp_low;
static volatile u32 spp_ticks;
static u32 pulse_intervals[PULSE_MEAN], pulse_interval_count;
static volatile u32 pulse_edges, pulse_last_cycle;
static volatile u32 pulse_seen;                          /* pulses taken by the audio interrupt */

static volatile u32 sync_out_since;                     /* TSC at the last pulse's start */
static volatile u8 sync_out_high;

/* Transport events for the app, delivered by clock_poll in the main loop. */
#define EVENTS 16u
static enaga_event_t events[EVENTS];
static volatile u32 events_head, events_tail;

static void event(u32 what, u32 position)
{
    if (events_head - events_tail >= EVENTS) return;
    events[events_head % EVENTS].type = ENAGA_EVENT_TRANSPORT;
    events[events_head % EVENTS].id = (u16)what;
    events[events_head % EVENTS].value = (int)position;
    events_head++;
}

static void midi_out(const u8 *bytes, u32 n)      /* to the MIDI OUT routing (midi.c) */
{
    if (clock_out_setting) midi_out_bytes(0, bytes, n);
}

static void send_position(u32 ticks)                    /* SPP: sixteenths, 24 ticks each */
{
    u32 sixteenths = ticks / 24u;
    u8 m[3] = { 0xF2, (u8)(sixteenths & 0x7F), (u8)((sixteenths >> 7) & 0x7F) };
    midi_out(m, 3);
}

static void sync_pin(int pulse)                         /* 1: the pulse's level, 0: idle (POL OUT) */
{
    if (pulse != sync_out_polarity_setting) gpio_set(7, 9); else gpio_clr(7, 9);
    sync_out_high = (u8)pulse;
}

void clock_sync_polarity_changed(void)                  /* the settings: re-arm SYNC IN, re-drive SYNC OUT */
{
    u32 gie = irq_global_disable();
    if (sync_in_polarity_setting) {                     /* FALL: the pin's rising edge */
        REG32(GPIO_PAIR3 + 0x20) = SYNC_IN_BIT;         /* CLR_FAL_TRIG */
        REG32(GPIO_PAIR3 + 0x14) = SYNC_IN_BIT;         /* SET_RIS_TRIG */
    } else {                                            /* RISE: the pin's falling edge */
        REG32(GPIO_PAIR3 + 0x18) = SYNC_IN_BIT;         /* CLR_RIS_TRIG */
        REG32(GPIO_PAIR3 + 0x1C) = SYNC_IN_BIT;         /* SET_FAL_TRIG */
    }
    REG32(GPIO_PAIR3 + 0x24) = SYNC_IN_BIT;             /* no stale edge from the change */
    sync_pin(sync_out_high);
    if (gie) irq_global_enable();
}

void clock_init(void)
{
    pinmux_set(17, 0, 8);                               /* GP7[9] SYNC OUT */
    pinmux_set(17, 1, 8);                               /* GP7[8] SYNC IN */
    gpio_dir_output(7, 9);
    sync_pin(0);
    REG32(GPIO_PAIR3) |= SYNC_IN_BIT;                   /* input */
    clock_sync_polarity_changed();                      /* the edge; nothing pending */
    REG32(GPIO_BINTEN) |= 1u << 7;
}

static void stop_now(void)
{
    u8 fc = 0xFC;
    if (!running && !waiting) return;
    running = 0; waiting = 0;
    midi_out(&fc, 1);
    event(ENAGA_TRANSPORT_STOP, whole);
}

/* ---- outside clocks, in their interrupts ---- */

void clock_gpio_isr(void)                               /* GPIO bank 7: a SYNC IN edge */
{
    u32 now = tsc_read();
    REG32(GPIO_PAIR3 + 0x24) = SYNC_IN_BIT;
    if (clock_source_setting != ENAGA_CLOCK_SYNC) return;
    if (pulse_edges) {
        pulse_intervals[pulse_interval_count++ % PULSE_MEAN] = now - pulse_last_cycle;
    }
    pulse_last_cycle = now;
    pulse_edges++;
}

void clock_midi_byte(u8 b)                              /* UART0's interrupt, every received byte in order */
{
    u32 now;
    if (clock_source_setting != ENAGA_CLOCK_MIDI) return;
    if (b < 0x80) {
        if (spp_need == 2) { spp_low = b; spp_need = 1; }
        else if (spp_need == 1) {
            spp_need = 0;
            if (!running && !waiting) {
                spp_ticks = ((u32)spp_low | (u32)b << 7) * 24u;
                whole = spp_ticks; frac = 0.0f; next_tick = whole;
                event(ENAGA_TRANSPORT_LOCATE, whole);
            }
        }
        return;
    }
    if (b < 0xF8) { spp_need = b == 0xF2 ? 2 : 0; return; }
    switch (b) {
    case 0xF8:                                          /* intervals count while stopped too, if steady */
        now = tsc_read();
        if (midi_last_cycle && now - midi_last_cycle < 1000u * TICKS_PER_MS)
            midi_intervals[midi_interval_count++ % MIDI_MEAN] = now - midi_last_cycle;
        midi_last_cycle = now ? now : 1;
        if (waiting) { waiting = 0; running = 1; cap = whole; next_tick = whole; }   /* this clock is that tick */
        if (running) { midi_clocks++; cap += TICKS_PER_CLOCK; }
        break;
    case 0xFA:
        whole = 0; frac = 0.0f; next_tick = 0; running = 0; waiting = 1; midi_clocks = 0;
        event(ENAGA_TRANSPORT_START, 0);
        break;
    case 0xFB:
        if (running || waiting) break;
        if (next_tick > whole) { whole = next_tick; frac = 0.0f; }   /* resume on the next whole tick */
        waiting = 1; midi_clocks = 0;
        event(ENAGA_TRANSPORT_CONTINUE, whole);
        break;
    case 0xFC:
        if (!running && !waiting) break;
        running = 0; waiting = 0;
        event(ENAGA_TRANSPORT_STOP, whole);
        break;
    default:
        break;
    }
}

static float mean(const u32 *intervals, u32 count, u32 size)
{
    u32 n = count < size ? count : size, i;
    float sum = 0.0f;
    for (i = 0; i < n; i++) sum += (float)intervals[i];
    return n ? sum / (float)n : 0.0f;
}

/* ---- the audio interrupt: advance one block ---- */

static void tick_out(u32 t)
{
    if (t % TICKS_PER_CLOCK == 0) { u8 f8 = 0xF8; midi_out(&f8, 1); }
    if (t % TICKS_PER_PULSE == 0 && sync_out_setting) {
        u32 now = tsc_read();
        if (!sync_out_since || now - sync_out_since >= PULSE_GAP_MS * TICKS_PER_MS) {
            sync_out_since = now ? now : 1;
            sync_pin(1);
        }
    }
}

static void give(u32 t, float frame)
{
    if (block_count < BLOCK_TICKS) {
        block_ticks[block_count].tick = t;
        block_ticks[block_count].frame = frame < 0.0f ? 0u : frame >= (float)AUDIO_FRAMES ? AUDIO_FRAMES - 1u : (u32)frame;
    }
    block_count++;
    tick_out(t);
}

void clock_block(void)
{
    float per_frame, avail, d;
    u32 external = clock_source_setting != ENAGA_CLOCK_INTERNAL;
    block_count = 0;
    if (sync_out_high && tsc_read() - sync_out_since >= PULSE_MS * TICKS_PER_MS) sync_pin(0);

    if (clock_source_setting == ENAGA_CLOCK_SYNC) {
        u32 edges = pulse_edges, now = tsc_read();
        float interval = mean(pulse_intervals, pulse_interval_count, PULSE_MEAN);
        while (pulse_seen < edges) {
            if (!running) {
                running = 1; whole = 0; frac = 0.0f; cap = 0; next_tick = 0;
                event(ENAGA_TRANSPORT_START, 0);
            }
            cap += TICKS_PER_PULSE;
            pulse_seen++;
        }
        if (interval > 0.0f) {
            tempo = 60.0f * (float)TICKS_PER_MS * 1000.0f / (interval * 2.0f);
            rate = (float)TICKS_PER_PULSE * (float)CYCLES_PER_FRAME / interval;
        }
        if (running) {
            float limit = interval > 0.0f ? interval * 2.5f : 2000.0f * TICKS_PER_MS;   /* 2 s before a tempo */
            if (limit < 100.0f * TICKS_PER_MS) limit = 100.0f * TICKS_PER_MS;
            if ((float)(now - pulse_last_cycle) > limit) {
                stop_now();
                pulse_interval_count = 0;
                pulse_edges = pulse_seen = 0;
                rate = 0.0f;
            }
        }
    } else if (clock_source_setting == ENAGA_CLOCK_MIDI) {
        float interval = mean(midi_intervals, midi_interval_count, MIDI_MEAN);
        if (interval > 0.0f) {
            tempo = 60.0f * (float)TICKS_PER_MS * 1000.0f / (interval * 24.0f);
            rate = (float)TICKS_PER_CLOCK * (float)CYCLES_PER_FRAME / interval;
        }
    }
    if (!running) return;

    /* An outside clock with no interval yet (its first clock or pulse) runs at the last tempo known. */
    per_frame = external && rate > 0.0f ? rate : tempo * (float)ENAGA_CLOCK_PPQN / (60.0f * FRAMES_PER_SECOND);
    {
        u32 unit = clock_source_setting == ENAGA_CLOCK_SYNC ? TICKS_PER_PULSE : TICKS_PER_CLOCK;
        if (external && cap > next_tick && cap - next_tick > unit) {   /* over one clock or pulse behind: catch up */
            while (cap - next_tick > unit) give(next_tick++, 0.0f);
            if ((float)whole + frac < (float)next_tick) { whole = next_tick; frac = 0.0f; }
        }
    }
    avail = per_frame * (float)AUDIO_FRAMES;
    if (external) {                                     /* never past the next outside clock's tick */
        float room = cap > whole ? (float)(cap - whole) - frac : 0.0f;
        if (avail >= room) {
            avail = room > 0.0f ? room : 0.0f;
            while (next_tick < cap) {                   /* the ticks up to it, then sit exactly on it */
                d = (float)next_tick - ((float)whole + frac);
                give(next_tick++, d > 0.0f ? d / per_frame : 0.0f);
            }
            if (cap > whole || (cap == whole && frac > 0.0f)) { whole = cap; frac = 0.0f; }
            return;
        }
    }
    for (;;) {
        d = (float)next_tick - ((float)whole + frac);
        if (d >= avail) break;
        give(next_tick++, d > 0.0f ? d / per_frame : 0.0f);
    }
    frac += avail;
    while (frac >= 1.0f) { frac -= 1.0f; whole++; }
}

/* ---- the app's side ---- */

u32 clock_ticks(enaga_clock_tick_t *out, u32 max)       /* audio context: this block's ticks */
{
    u32 n = block_count < BLOCK_TICKS ? block_count : BLOCK_TICKS, i;
    if (n > max) n = max;
    for (i = 0; i < n; i++) out[i] = block_ticks[i];
    return n;
}

void clock_state(enaga_clock_state_t *s)
{
    u32 gie = irq_global_disable();
    s->running = running;
    s->source = (u32)clock_source_setting;
    s->tempo = tempo;
    s->position = whole;
    s->fraction = frac;
    if (gie) irq_global_enable();
}

int clock_control(u32 command, int value)               /* main context */
{
    u32 gie;
    u8 m;
    if (clock_source_setting != ENAGA_CLOCK_INTERNAL) return -1;
    gie = irq_global_disable();
    switch (command) {
    case ENAGA_CLOCK_START:
        whole = 0; frac = 0.0f; next_tick = 0; running = 1;
        m = 0xFA; midi_out(&m, 1);
        event(ENAGA_TRANSPORT_START, 0);
        break;
    case ENAGA_CLOCK_STOP:
        stop_now();
        break;
    case ENAGA_CLOCK_CONTINUE:
        if (running) break;
        whole = next_tick; frac = 0.0f;                 /* resume on the next whole tick */
        { u8 c[4] = { 0xF2, (u8)((whole / 24u) & 0x7F), (u8)((whole / 24u >> 7) & 0x7F), 0xFB };
          midi_out(c, 4); }
        running = 1;
        event(ENAGA_TRANSPORT_CONTINUE, whole);
        break;
    case ENAGA_CLOCK_LOCATE:
        if (value < 0) { if (gie) irq_global_enable(); return -1; }
        whole = (u32)value; frac = 0.0f; next_tick = whole;
        if (!running) send_position(whole);
        event(ENAGA_TRANSPORT_LOCATE, whole);
        break;
    case ENAGA_CLOCK_TEMPO:
        if (value < (int)(TEMPO_MIN * 100.0f) || value > (int)(TEMPO_MAX * 100.0f)) {
            if (gie) irq_global_enable();
            return -1;
        }
        tempo = (float)value * 0.01f;
        break;
    default:
        if (gie) irq_global_enable();
        return -1;
    }
    if (gie) irq_global_enable();
    return 0;
}

void clock_source_changed(void)                          /* the setting moved: stop whatever was running */
{
    u32 gie = irq_global_disable();
    stop_now();
    midi_interval_count = 0; midi_clocks = 0;
    pulse_interval_count = 0; pulse_edges = pulse_seen = 0;
    rate = 0.0f;
    if (clock_source_setting == ENAGA_CLOCK_INTERNAL && (tempo < TEMPO_MIN || tempo > TEMPO_MAX)) tempo = 120.0f;
    if (gie) irq_global_enable();
}

void clock_app_stopped(void)                             /* an internal clock the app ran stops with it */
{
    if (clock_source_setting == ENAGA_CLOCK_INTERNAL) {
        u32 gie = irq_global_disable();
        stop_now();
        if (gie) irq_global_enable();
    }
}

int clock_poll(void)                                     /* main loop: transport events to the app */
{
    int n = 0;
    while (events_tail != events_head) {
        enaga_event_t e = events[events_tail % EVENTS];
        events_tail++;
        e.ms = timer_ms();
        apps_event(&e);
        n++;
    }
    return n;
}
