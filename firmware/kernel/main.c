#include "kernel.h"

/* Enaga start-up and the main loop.
 *
 * Start-up follows an order proven on the unit: power hold, time-stamp counter, keys, timer, encoders,
 * display, the panel MCU link (90, B1, A0), audio, card. Before any of it Enaga sets its own cache
 * configuration (hal/cache.c) and logs the one it was handed. Then interrupts are enabled, the
 * boot policy picks an app (apps.c apps_boot) or opens the menu, and kernel_loop runs: inputs become events for the app, `update` runs every pass,
 * `draw` every 50 ms with the frame sent by EDMA (the loop keeps running while it goes out; a frame still in
 * flight delays the next draw), and the power key held one second switches the unit off. With no app
 * (none found, or one that failed) Enaga shows its own status page. Each pass feeds the watchdog (watchdog.c).
 *
 * Pads are delivered raw and as press / release with velocity (on_pad). Keys are debounced in keys.c.
 *
 * F1 + POWER opens the Enaga menu (menu.c) and closes it again; F1 held through start-up opens it instead of an
 * app. While the menu is open the app is paused: inputs go to the menu, and update and draw are not called. An
 * app gets a release for every key and pad press it was given (when the menu opens, at once), and none for a press
 * it was not. */

extern char __bss_start[], __bss_end[];
static u64 last_keys;
static int encoder_last[5];
static u32 a0_last_ms, a0_sent;
static int power_seen_up, power_was_down, power_chord;
static u64 app_keys;                         /* keys down that the app was told of */
static u16 app_pads;                         /* pads likewise */
volatile u32 kernel_stage, kernel_passes;
static u32 power_down_since, last_draw;
static int display_dma;                      /* frames by EDMA once the audio engine has powered it */

static void log_cache(u32 was, u32 now)     /* "CACHE 443 00>444 FF": L2, L1P, L1D modes and DDR MARs */
{
    static const char hex[] = "0123456789ABCDEF";
    char line[] = "CACHE 000 00>000 00";
    u32 words[2] = { was, now }, i;
    for (i = 0; i < 2; i++) {
        char *p = line + 6 + 7 * i;
        p[0] = hex[words[i] >> 24 & 15];
        p[1] = hex[words[i] >> 16 & 15];
        p[2] = hex[words[i] >> 8 & 15];
        p[4] = hex[words[i] >> 4 & 15];
        p[5] = hex[words[i] & 15];
    }
    log_line(line);
}

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

/* Pads. A press starts at the original firmware's 800 and ends below 600. A pad's reading rises over several of
 * the MCU's reports (each pad every 3.8 ms), so the first report over 800 says little about how hard it was hit
 * (on the unit it was nearly always just over 800: session 1, docs/hardware.md "Pad readings"). The press is
 * therefore sent at the next report, with velocity from the higher of the two, 127 at PAD_FULL or more: at most
 * one report (3.8 ms) later. A pad released before that still gives its press, then the release. Measured on the
 * unit: a light press reads about 1,500, a hard one about 3,400. */
#define PAD_PRESS    800u
#define PAD_RELEASE  600u
#define PAD_FULL     3300u
enum { PAD_UP, PAD_RISING, PAD_DOWN };
static u8 pad_state[15];
static u16 pad_peak[15];

/* An input event to the menu or the app. With no app and the menu closed (the status page), OK opens the menu. */
static void deliver(const enaga_event_t *e)
{
    if (menu_active) { menu_event(e); return; }
    if (!app_current) {
        if (e->type == ENAGA_EVENT_KEY && e->value && e->id == ENAGA_KEY_OK) menu_open(0);
        return;
    }
    if (e->type == ENAGA_EVENT_KEY) {
        u64 bit = (u64)1 << e->id;
        if (e->value) app_keys |= bit;
        else if (app_keys & bit) app_keys &= ~bit;
        else return;
    } else if (e->type == ENAGA_EVENT_PAD_PRESS) {
        app_pads |= (u16)(1u << e->id);
    } else if (e->type == ENAGA_EVENT_PAD_RELEASE) {
        if (!(app_pads & (1u << e->id))) return;
        app_pads &= (u16)~(1u << e->id);
    }
    apps_event(e);
}

static void open_menu(const char *why)       /* the app first lets go of everything it was holding */
{
    enaga_event_t e;
    int i;
    e.ms = timer_ms();
    for (i = 0; i < ENAGA_KEY_COUNT; i++)
        if ((app_keys >> i) & 1) { e.type = ENAGA_EVENT_KEY; e.id = (u16)i; e.value = 0; apps_event(&e); }
    for (i = 0; i < 15; i++)
        if ((app_pads >> i) & 1) { e.type = ENAGA_EVENT_PAD_RELEASE; e.id = (u16)i; e.value = 0; apps_event(&e); }
    app_keys = 0;
    app_pads = 0;
    menu_open(why);
}

static void pad_press(int channel, enaga_event_t *e)
{
    unsigned peak = pad_peak[channel] < PAD_FULL ? pad_peak[channel] : PAD_FULL;
    pad_state[channel] = PAD_DOWN;
    e->type = ENAGA_EVENT_PAD_PRESS;
    e->value = 1 + (int)(126u * (peak - PAD_PRESS) / (PAD_FULL - PAD_PRESS));
    deliver(e);
}

static void on_pad(int channel, unsigned value)
{
    enaga_event_t e;
    if (channel >= 15) return;                /* channel 15 is the battery */
    e.id = (u16)channel; e.ms = timer_ms();
    if (pad_state[channel] == PAD_UP && value >= PAD_PRESS) {
        pad_state[channel] = PAD_RISING;
        pad_peak[channel] = (u16)value;
    } else if (pad_state[channel] == PAD_RISING) {
        if (value > pad_peak[channel]) pad_peak[channel] = (u16)value;
        pad_press(channel, &e);
    }
    if (pad_state[channel] == PAD_DOWN && value < PAD_RELEASE) {
        pad_state[channel] = PAD_UP;
        e.type = ENAGA_EVENT_PAD_RELEASE; e.value = 0;
        deliver(&e);
    }
    e.type = ENAGA_EVENT_PAD; e.value = (int)value;
    deliver(&e);
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
    /* The SD card: sectors read and written since start-up, and the last failure (code, command, MMCST0 low byte):
     * "SD R1234 W56 E-2 C24 10". */
    gfx_text(0, 66, "SD", 10);
    gfx_text(18, 66, "R", 8); gfx_dec(24, 66, (int)sd_reads, 15);
    gfx_text(66, 66, "W", 8); gfx_dec(72, 66, (int)sd_writes, 15);
    if (sd_io_error) {
        gfx_text(0, 75, "E", 8); gfx_dec(6, 75, sd_io_error, 15);
        gfx_text(30, 75, "C", 8); gfx_dec(36, 75, (int)sd_io_command, 15);
        gfx_hex(60, 75, sd_io_status, 4, 15);
    }
    first = log_count > 5 ? log_count - 5 : 0;
    for (i = first; i < log_count; i++) gfx_text(0, 84 + 9 * (int)(i - first), log_lines[i % LOG_LINES], 8);
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
            deliver(&e);
        }
    last_keys = keys;
    encoders_poll();
    for (i = 0; i < 5; i++)
        if (encoder_count[i] != encoder_last[i]) {
            e.type = ENAGA_EVENT_ENCODER; e.id = (u16)i; e.value = encoder_count[i] - encoder_last[i];
            encoder_last[i] = encoder_count[i];
            deliver(&e);
        }
    uart1_poll();                             /* pads arrive through on_pad */
    if (!mcu_replies && ms - a0_last_ms >= 2000) {   /* the MCU may have missed the first A0: ask again */
        a0_last_ms = ms;
        uart1_send(0xA0);
        a0_sent++;
    }
}

/* POWER held a second switches off. Pressed while F1 is down it opens or closes the menu instead (on the press,
 * F1 first), and holding it on then does not switch off. */
static void poll_power(u32 ms)
{
    if (!power_key_down()) {
        power_seen_up = 1;                    /* a hand still on the key from switching on never counts */
        power_was_down = power_chord = 0;
    } else if (power_seen_up) {
        if (!power_was_down) {
            power_was_down = 1;
            power_down_since = ms;
            if ((last_keys >> ENAGA_KEY_F1) & 1) {
                power_chord = 1;
                if (!menu_active) open_menu(0);
                else menu_close();
            }
        } else if (!power_chord && ms - power_down_since >= 1000) {
            shutdown();
        }
    }
}

void kernel_loop(void)
{
    int busy, filing;
    irq_global_enable();
    if (!app_current) {                          /* start-up with no app, or an app was just dropped */
        leds_off();
        app_keys = 0;
        app_pads = 0;
        if (crash_reason[0]) {                   /* a crash: the status page shows why (OK: the menu) */
            menu_active = menu_status = 0;
            last_keys = keys_reset(timer_ms());  /* a key the app was handling when it hung is not pressed again */
        }
    }
    for (;;) {
        u32 ms = timer_ms();
        kernel_passes++;
        kernel_stage = 1;
        watchdog_feed();
        poll_inputs(ms);
        kernel_stage = 2;
        if (app_current && !menu_active && app_current->update) app_current->update(app_current->context, ms);
        apps_service(ms);
        kernel_stage = 8;
        filing = files_service(ms);                 /* the card-detect pin, then queued file work (files.c) */
        kernel_stage = 3;
        poll_power(ms);
        kernel_stage = 4;
        busy = display_dma ? oled_dma_poll() : 0;   /* also releases the chip select after a frame */
        if (ms - last_draw >= 50 && !busy) {        /* a frame still in flight delays the next draw */
            last_draw = ms;
            kernel_stage = 5;
            if (menu_active && !menu_status) menu_draw();
            else if (app_current && !menu_active && app_current->draw) app_current->draw(app_current->context, canvas);
            else status_page(ms);
            kernel_stage = 6;
            if (display_dma) gfx_present_dma(); else gfx_present();
        }
        kernel_stage = 7;
        /* Sleep until the next interrupt: the audio block (every 1.33 ms), UART1 (each byte from the panel MCU,
         * about 12,600 a second on the unit), the timer (10 ms). So a pass still runs at least 750 times a second,
         * more often than the original firmware polls the encoders (each 1 ms tick). Work an interrupt leaves
         * after this pass's polls waits at most for the next one; nothing is lost (the UART ring holds 80 ms).
         * Not while file requests are queued: they continue on the next pass at once. */
        if (!filing) __asm__ volatile("idle" ::: "memory");
    }
}

void main(void)
{
    char *p, why[40];
    int boot_menu;
    tsc_enable();
    /* GPIO on before anything uses it, as the original firmware's board set-up does. The bootloader leaves it
     * on (code that never enables it has driven the power hold and read keys on a unit), so this changes
     * nothing there, but it lets Enaga start from a power-on state too. */
    psc_enable(PSC1, PSC1_GPIO);
    power_hold();
    for (p = __bss_start; p < __bss_end; p++) *p = 0;
    cache_setup();
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
    leds_forget();                            /* whatever the panel shows now: every LED off */
    leds_off();
    splash("AUDIO");
    audio_init();
    display_dma = audio_status == 0 || audio_status <= -3;   /* -1/-2: EDMA never powered */
    splash("CARD");
    files_init();
    apps_mount();
    files_write_probe();
    last_keys = keys_reset(timer_ms());       /* keys held through start-up do not become events */
    boot_menu = (last_keys >> ENAGA_KEY_F1) & 1;   /* F1 held through start-up: the menu, no app */
    uart1_irq_start();                        /* the MCU's bytes by interrupt from here (start-up may have lost some) */
    irq_enable(1u << IRQ_UART1);
    irq_global_enable();                      /* audio blocks start being serviced (silence until an app) */
    log_line("ENAGA " ENAGA_VERSION);
    log_cache(cache_entry_config(), cache_config());
    watchdog_start();
    if (apps_boot(boot_menu, why, sizeof why)) menu_open(why);
    kernel_loop();
}
