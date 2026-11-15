#include "kernel.h"

/* Enaga start-up and the main loop.
 *
 * Start-up follows an order proven on the unit: power hold, time-stamp counter, keys, timer, encoders,
 * display, the panel MCU link (90, B1, A0), audio, card. Then interrupts are enabled, the
 * first app found is loaded, and kernel_loop runs: inputs become events for the app, `update` runs every pass,
 * `draw` every 50 ms with the frame sent by EDMA (the loop keeps running while it goes out; a frame still in
 * flight delays the next draw), and the power key held one second switches the unit off. With no app
 * (none found, or one that failed) Enaga shows its own status page. Each pass feeds the watchdog (watchdog.c).
 *
 * Not yet: FUNC at power-on / FUNC + POWER and the Enaga menu, and pad thresholds (raw readings are
 * delivered). Keys are debounced in keys.c. */

extern char __bss_start[], __bss_end[];
static u64 last_keys;
static int encoder_last[5];
static u32 a0_last_ms, a0_sent;
static int power_seen_up, power_was_down;
static u32 power_down_since, last_draw;
static int display_dma;                      /* frames by EDMA once the audio engine has powered it */

static void splash(const char *stage)
{
    static int y = 70;
    gfx_text(10, y, stage, 8);
    y += 10;
    gfx_present();
}

static void poll_for(u32 ms)                  /* a pause in the MCU start messages, reading replies meanwhile */
{
    u32 start = timer_ms();
    while (timer_ms() - start < ms) uart1_poll();
}

static void on_pad(int channel, unsigned value)
{
    enaga_event_t e;
    if (channel >= 15) return;                /* channel 15 is the battery */
    e.type = ENAGA_EVENT_PAD; e.id = (u16)channel; e.value = (int)value; e.ms = timer_ms();
    apps_event(&e);
}

static void status_page(u32 ms)
{
    u32 i, first;
    gfx_clear(0);
    gfx_text(0, 0, "ENAGA " ENAGA_VERSION, 15);
    gfx_dec(96, 0, (int)(ms / 1000u), 8);
    gfx_text(0, 12, app_status, 15);
    if (app_name[0]) gfx_text(0, 21, app_name, 10);
    if (crash_reason[0]) gfx_text(0, 30, crash_reason, 15);
    if (crash_efr) {                          /* the exception that dropped the app: PC, EFR, IERR */
        gfx_hex(0, 39, crash_pc, 8, 12);
        gfx_hex(54, 39, crash_efr, 1, 12);
        gfx_hex(66, 39, crash_ierr, 3, 12);
    }
    gfx_text(0, 48, "AUDIO", 10);
    gfx_dec(36, 48, audio_status, 15);
    gfx_dec(54, 48, (int)(audio_blocks % 100000u), 15);
    gfx_text(0, 57, "LOAD", 10);
    gfx_dec(36, 57, (int)(audio_cycles_peak / (AUDIO_BUDGET_CYCLES / 1000u)), 15);
    gfx_dec(78, 57, (int)audio_overruns, 15);
    first = log_count > 6 ? log_count - 6 : 0;
    for (i = first; i < log_count; i++) gfx_text(0, 70 + 9 * (int)(i - first), log_lines[i % LOG_LINES], 8);
}

static void shutdown(void)
{
    apps_unload();
    gfx_clear(0);
    gfx_text(52, 60, "BYE", 15);
    gfx_present();
    uart1_send(0xB2);                         /* the original firmware's order: B2, DAC mute, C2, hold release */
    audio_mute();
    uart1_send(0xC2);
    delay_us(50000);
    power_off();
    for (;;)
        ;
}

static void poll_inputs(u32 ms)
{
    u64 keys = keys_poll(ms), changed = keys ^ last_keys;
    int i;
    enaga_event_t e;
    e.ms = ms;
    for (i = 0; changed && i < ENAGA_KEY_COUNT; i++)
        if ((changed >> i) & 1) {
            e.type = ENAGA_EVENT_KEY; e.id = (u16)i; e.value = (int)((keys >> i) & 1);
            apps_event(&e);
        }
    last_keys = keys;
    encoders_poll();
    for (i = 0; i < 5; i++)
        if (encoder_count[i] != encoder_last[i]) {
            e.type = ENAGA_EVENT_ENCODER; e.id = (u16)i; e.value = encoder_count[i] - encoder_last[i];
            encoder_last[i] = encoder_count[i];
            apps_event(&e);
        }
    uart1_poll();                             /* pads arrive through on_pad */
    if (!mcu_replies && ms - a0_last_ms >= 2000) {   /* the MCU may have missed the first A0: ask again */
        a0_last_ms = ms;
        uart1_send(0xA0);
        a0_sent++;
    }
}

static void poll_power(u32 ms)
{
    if (!power_key_down()) {
        power_seen_up = 1;                    /* a hand still on the key from switching on never counts */
        power_was_down = 0;
    } else if (power_seen_up) {
        if (!power_was_down) { power_was_down = 1; power_down_since = ms; }
        else if (ms - power_down_since >= 1000) shutdown();
    }
}

void kernel_loop(void)
{
    int busy;
    irq_global_enable();
    for (;;) {
        u32 ms = timer_ms();
        watchdog_feed();
        poll_inputs(ms);
        if (app_current && app_current->update) app_current->update(app_current->context, ms);
        poll_power(ms);
        busy = display_dma ? oled_dma_poll() : 0;   /* also releases the chip select after a frame */
        if (ms - last_draw >= 50 && !busy) {        /* a frame still in flight delays the next draw */
            last_draw = ms;
            if (app_current && app_current->draw) app_current->draw(app_current->context, canvas);
            else status_page(ms);
            if (display_dma) gfx_present_dma(); else gfx_present();
        }
    }
}

void main(void)
{
    char *p;
    power_hold();
    tsc_enable();
    for (p = __bss_start; p < __bss_end; p++) *p = 0;
    irq_init();
    exceptions_enable();
    keys_init();
    timer_init();
    encoders_init();
    if (oled_init() < 0) fault(2);
    gfx_clear(0);
    gfx_text(34, 50, "ENAGA", 15);
    gfx_text(34, 60, ENAGA_VERSION, 8);
    if (gfx_present() < 0) fault(4);
    splash("MCU");
    uart1_init();
    pad_callback = on_pad;
    uart1_send(0x90);                         /* the original firmware's start-up messages, 50 ms apart */
    poll_for(50);
    uart1_send(0xB1);
    poll_for(50);
    uart1_send(0xA0);
    a0_sent = 1;
    a0_last_ms = timer_ms();
    splash("AUDIO");
    audio_init();
    display_dma = audio_status == 0 || audio_status <= -3;   /* -1/-2: EDMA never powered */
    splash("CARD");
    apps_mount();
    last_keys = keys_reset(timer_ms());       /* keys held through start-up do not become events */
    irq_global_enable();                      /* audio blocks start being serviced (silence until an app) */
    log_line("ENAGA " ENAGA_VERSION);
    watchdog_start();
    apps_load_first();
    kernel_loop();
}
