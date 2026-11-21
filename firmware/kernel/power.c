#include "kernel.h"

/* The battery (docs/hardware.md "Power"). The panel MCU reports the battery voltage as analogue channel 15 among the
 * pad readings, and GP3[7] is high while the unit runs on batteries. As the original firmware does (smpltrek-re
 * "Power management", XF-8; its battery_sample run in c6sim): once every battery_sample_ms the latest reading is
 * taken, readings under 2000 ignored, and each eight make an average. The level (0-4, 4 full) falls a step while the
 * average is below the falling threshold under it and rises a step only when the average reaches the rising one
 * above it, so a battery that sags under load and recovers does not flicker between levels. The first average sets
 * the level outright. The thresholds are the original firmware's raw values, per BATTERY TYPE.
 *
 * At level 0 on batteries Enaga warns on screen and, if the level stays there for battery_empty_ms, switches off
 * the way POWER does, so the app's files are closed before the supply gives out (main.c). */

static const u16 falling[3][4] = {           /* below [type][n]: level n + 1 falls to n */
    { 2630, 2830, 2980, 3180 },               /* ALKALINE */
    { 2840, 2915, 2940, 2990 },               /* NiMH */
    { 3030, 3280, 3360, 3490 },               /* LITHIUM */
};
static const u16 rising[3][4] = {            /* at or above [type][n]: level n rises to n + 1 */
    { 2730, 2930, 3080, 3280 },
    { 2855, 2930, 2955, 3005 },
    { 3080, 3330, 3410, 3540 },
};

int battery_type_setting;                     /* BATTERY: 0 ALKALINE, 1 NiMH, 2 LITHIUM */
u32 battery_sample_ms = 1024;                 /* the original firmware's period; tests shorten it */
u32 battery_empty_ms = 30000;                 /* at level 0 this long: switch off */
u32 battery_average;                          /* the last average, 0 before the first */
static volatile u32 battery_raw;
static u32 sum, samples, last_sample, empty_since;
static int level = 4, empty;

void battery_reading(unsigned raw) { battery_raw = raw; }   /* uart1, through main.c's on_pad: channel 15 */

static void step(void)
{
    int before = level;
    const u16 *down = falling[battery_type_setting], *up = rising[battery_type_setting];
    while (level > 0 && battery_average < down[level - 1]) level--;
    while (level < 4 && battery_average >= up[level]) level++;
    if (level != before && power_on_batteries()) {
        char line[] = "BATTERY 0 0000";
        u32 v = battery_average;
        int i;
        line[8] = (char)('0' + level);
        for (i = 13; i >= 10; i--, v /= 10) line[i] = (char)('0' + v % 10);
        log_line(line);
    }
}

void battery_type_changed(void) { if (battery_average) step(); }

int battery_level(void) { return power_on_batteries() ? level : -1; }

/* Main loop. 1 when the battery has been empty for battery_empty_ms: time to switch off. */
int battery_poll(u32 ms)
{
    if (ms - last_sample >= battery_sample_ms) {
        u32 raw = battery_raw;
        last_sample = ms;
        if (raw >= 2000) {
            sum += raw;
            if (++samples == 8) {
                battery_average = sum >> 3;
                sum = samples = 0;
                step();
            }
        }
    }
    if (battery_level() != 0) { empty = 0; return 0; }
    if (!empty) { empty = 1; empty_since = ms; log_line("BATTERY EMPTY"); }
    return ms - empty_since >= battery_empty_ms;
}

/* Seconds until the switch-off while the battery is empty, else -1 (for the warning on screen). */
int battery_seconds_left(u32 ms)
{
    u32 gone;
    if (!empty || battery_level() != 0) return -1;
    gone = ms - empty_since;
    return gone >= battery_empty_ms ? 0 : (int)((battery_empty_ms - gone + 999u) / 1000u);
}
