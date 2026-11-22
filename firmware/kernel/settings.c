#include "kernel.h"
#include "ff.h"

/* System settings (docs/design.md "Settings"): Enaga's own, kept in /ENAGA/SETTINGS.TXT on the card as lines of
 * NAME=VALUE, e.g. "VOLUME=10". A missing file, line or unknown value leaves that setting at its default, so the
 * file can be edited by hand or deleted. The menu's SETTINGS page changes them (applied at once) and writes the
 * file when the page closes, if anything changed.
 *
 *   VOLUME      0-10      output level, 3 dB a step, 0 silent (audio.c)
 *   BRIGHTNESS  1-10      the display's contrast command (0x81)
 *   PADS        LOW MEDIUM HIGH   sensitivity: a press from 1,600 / 1,000 / 600, released below 3/4 of that
 *   AUTO OFF    OFF 5-60 MIN      switch off after that long without input, unless the app is busy (set_busy)
 *   SPEAKER     ON OFF            the built-in speaker (headphones cut it in any case); an app can also mute it
 *   MIDI CH     1-16              the channel apps listen and send on by default (enaga::midi::channel)
 *   MIDI THRU   OFF ON            copy what arrives at MIDI IN to MIDI OUT (SysEx excepted), merged with the app's own
 *   MIDI IN     DIN USB BOTH      which ports' messages (and MIDI clock) reach apps and the clock
 *   MIDI OUT    DIN USB BOTH      where apps' messages (unless an app picks a port) and MIDI clock out go
 *   CLOCK SRC   INT MIDI SYNC     who drives the tempo clock: the app, MIDI clock at MIDI IN, or SYNC IN (clock.c)
 *   CLOCK OUT   OFF ON            send MIDI clock and transport at MIDI OUT
 *   SYNC OUT    ON OFF            pulse SYNC OUT, 2 to the quarter note
 *   BATTERY     ALKALINE NiMH LITHIUM   the batteries in the unit, for the level's thresholds (power.c)
 *
 * The SPI flash is not used: it holds the original firmware's sound content, which Enaga never writes. */
#define SETTINGS_FILE "/ENAGA/SETTINGS.TXT"

static const char *const numbers[] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10" };
static const char *const pads[] = { "LOW", "MEDIUM", "HIGH" };
static const char *const off[] = { "OFF", "5 MIN", "10 MIN", "30 MIN", "60 MIN" };
static const char *const on_off[] = { "ON", "OFF" };
static const char *const off_on[] = { "OFF", "ON" };
static const char *const sources[] = { "INT", "MIDI", "SYNC" };
static const char *const ports[] = { "DIN", "USB", "BOTH" };
static const char *const batteries[] = { "ALKALINE", "NiMH", "LITHIUM" };
static const char *const channels[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", "12", "13", "14",
                                        "15", "16" };

typedef struct {
    const char *name;
    const char *const *labels;
    int first, count, fallback;               /* labels[first .. first + count - 1]; the default's index */
} setting_t;

enum { VOLUME, BRIGHTNESS, PADS, AUTO_OFF, SPEAKER, MIDI_CH, MIDI_THRU, MIDI_IN, MIDI_OUT, CLOCK_SRC, CLOCK_OUT, SYNC_OUT,
       BATTERY, SETTINGS_COUNT };
static const setting_t table[SETTINGS_COUNT] = {
    { "VOLUME", numbers, 0, 11, 10 },
    { "BRIGHTNESS", numbers, 1, 10, 9 },
    { "PADS", pads, 0, 3, 1 },
    { "AUTO OFF", off, 0, 5, 0 },
    { "SPEAKER", on_off, 0, 2, 0 },
    { "MIDI CH", channels, 0, 16, 0 },
    { "MIDI THRU", off_on, 0, 2, 0 },
    { "MIDI IN", ports, 0, 3, 2 },
    { "MIDI OUT", ports, 0, 3, 2 },
    { "CLOCK SRC", sources, 0, 3, 0 },
    { "CLOCK OUT", off_on, 0, 2, 0 },
    { "SYNC OUT", on_off, 0, 2, 0 },
    { "BATTERY", batteries, 0, 3, 0 },
};
static int settings_value[SETTINGS_COUNT], settings_saved[SETTINGS_COUNT];
static int settings_cursor;

u32 pad_press_level = 1000, pad_release_level = 750;
u32 settings_auto_off_ms;

static void apply(int which)
{
    static const float gain[11] = { 0.0f, 0.0447f, 0.0631f, 0.0891f, 0.126f, 0.178f, 0.251f, 0.355f, 0.501f, 0.708f, 1.0f };
    static const u16 press[3] = { 1600, 1000, 600 };
    static const u32 minutes[5] = { 0, 5, 10, 30, 60 };
    int v = settings_value[which];
    switch (which) {
    case VOLUME: audio_volume = gain[v]; break;
    case BRIGHTNESS: oled_contrast((u8)(25 * (v + 1) + 5)); break;   /* 1 -> 55, 10 -> 255 */
    case PADS: pad_press_level = press[v]; pad_release_level = press[v] * 3u / 4u; break;
    case AUTO_OFF: settings_auto_off_ms = minutes[v] * 60000u; break;
    case SPEAKER: audio_speaker_setting = v == 0; audio_speaker_apply(); break;
    case MIDI_CH: midi_channel_setting = v; break;
    case MIDI_THRU: midi_thru = v == 1; break;
    case MIDI_IN: midi_in_setting = v + 1; break;     /* DIN 1, USB 2, BOTH 3: the port bits */
    case MIDI_OUT: midi_out_setting = v + 1; break;
    case CLOCK_SRC:
        if (clock_source_setting != v) { clock_source_setting = v; clock_source_changed(); }
        break;
    case CLOCK_OUT: clock_out_setting = v == 1; break;
    case SYNC_OUT: sync_out_setting = v == 0; break;
    case BATTERY: battery_type_setting = v; battery_type_changed(); break;
    default: break;
    }
}

static int same(const char *a, const char *b, unsigned n)   /* a's first n bytes are the string b */
{
    unsigned i;
    for (i = 0; i < n && b[i] && a[i] == b[i]; i++) {}
    return i == n && !b[i];
}

static void parse_line(const char *line, unsigned n)
{
    unsigned eq, k;
    int i;
    for (eq = 0; eq < n && line[eq] != '='; eq++) {}
    if (eq == n) return;
    for (k = 0; k < SETTINGS_COUNT; k++) {
        if (!same(line, table[k].name, eq)) continue;
        for (i = 0; i < table[k].count; i++)
            if (same(line + eq + 1, table[k].labels[table[k].first + i], n - eq - 1)) settings_value[k] = i;
    }
}

void settings_load(void)                      /* after the card is mounted (or not: the defaults) */
{
    static FIL f;
    static char text[512];
    UINT got = 0;
    unsigned i, start;
    int k;
    for (k = 0; k < SETTINGS_COUNT; k++) settings_value[k] = table[k].fallback;
    if (files_mounted() && f_open(&f, SETTINGS_FILE, FA_READ) == FR_OK) {
        if (f_read(&f, text, sizeof text - 1, &got) != FR_OK) got = 0;
        f_close(&f);
    }
    for (i = start = 0; i <= got; i++)
        if (i == got || text[i] == '\n' || text[i] == '\r') {
            if (i > start) parse_line(text + start, i - start);
            start = i + 1;
        }
    for (k = 0; k < SETTINGS_COUNT; k++) { settings_saved[k] = settings_value[k]; apply(k); }
}

static void save(void)
{
    static FIL f;
    char text[320];                           /* all lines at their longest: about 180 bytes */
    unsigned n = 0, i;
    int k;
    UINT put;
    for (k = 0; k < SETTINGS_COUNT; k++) {
        const char *label = table[k].labels[table[k].first + settings_value[k]];
        for (i = 0; table[k].name[i]; i++) text[n++] = table[k].name[i];
        text[n++] = '=';
        for (i = 0; label[i]; i++) text[n++] = label[i];
        text[n++] = '\n';
    }
    if (!files_mounted() || f_open(&f, SETTINGS_FILE, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) { log_line("SETTINGS NOT SAVED"); return; }
    f_write(&f, text, n, &put);
    if (f_close(&f) != FR_OK || put != n) { log_line("SETTINGS NOT SAVED"); return; }
    for (k = 0; k < SETTINGS_COUNT; k++) settings_saved[k] = settings_value[k];
    log_line("SETTINGS SAVED");
}

/* The SETTINGS page: UP / DOWN choose a setting, LEFT / RIGHT or VALUE change it, OK or CLR go back (saving). */
void settings_open(void) { settings_cursor = 0; }

static void change(int by)
{
    int v = settings_value[settings_cursor] + by;
    if (v < 0) v = 0;
    if (v >= table[settings_cursor].count) v = table[settings_cursor].count - 1;
    if (v == settings_value[settings_cursor]) return;
    settings_value[settings_cursor] = v;
    apply(settings_cursor);
}

int settings_event(const enaga_event_t *e)    /* 1: the page closed */
{
    int k;
    if (e->type == ENAGA_EVENT_ENCODER && e->id == 4) { change(e->value); return 0; }
    if (e->type != ENAGA_EVENT_KEY || !e->value) return 0;
    switch (e->id) {
    case ENAGA_KEY_UP: if (settings_cursor > 0) settings_cursor--; break;
    case ENAGA_KEY_DOWN: if (settings_cursor < SETTINGS_COUNT - 1) settings_cursor++; break;
    case ENAGA_KEY_LEFT: change(-1); break;
    case ENAGA_KEY_RIGHT: change(1); break;
    case ENAGA_KEY_OK:
    case ENAGA_KEY_CLR:
        for (k = 0; k < SETTINGS_COUNT; k++) if (settings_value[k] != settings_saved[k]) { save(); break; }
        return 1;
    default: break;
    }
    return 0;
}

void settings_draw(void)
{
    int k, y;
    gfx_clear(0);
    gfx_text(0, 0, "SETTINGS", 15);
    /* Nine rows fit; the list scrolls to keep the cursor on screen. */
    static int top;
    if (settings_cursor < top) top = settings_cursor;
    if (settings_cursor > top + 8) top = settings_cursor - 8;
    for (k = top, y = 22; k < SETTINGS_COUNT && k < top + 9; k++, y += 9) {
        int here = k == settings_cursor;
        if (here) gfx_text(0, y, ">", 15);
        gfx_text(8, y, table[k].name, here ? 15 : 8);
        gfx_text(74, y, table[k].labels[table[k].first + settings_value[k]], here ? 15 : 10);
    }
    gfx_text(0, 110, "LEFT RIGHT: CHANGE", 6);
    gfx_text(0, 119, "OK: BACK", 6);
}
