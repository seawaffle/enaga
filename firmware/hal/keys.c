#include "hal.h"

/* The 6 x 8 key matrix on GPIO (docs/hardware.md "Keys"): rows GP2[0..5] are
 * outputs driven one high at a time; columns 0-5 are GP3[1..6], 6 is GP3[9], 7 is GP3[11]; in row 0,
 * bit 4 is the extra key on GP5[10], active low. Key n = row n / 8, column n % 8; bit 27 is HOME. */
#define ROW_MASK 0x3Fu

void keys_init(void)
{
    unsigned k;
    for (k = 2; k < 8; k++) pinmux_set(6, k, 8);     /* GP2[0..5] */
    for (k = 1; k < 7; k++) pinmux_set(8, k, 8);     /* GP3[1..6] */
    pinmux_set(7, 4, 8);                             /* GP3[11] */
    pinmux_set(11, 5, 8);                            /* GP5[10] (the original firmware sets it with the encoders) */
    pupd_clear_19_20();
    for (k = 0; k < 6; k++) gpio_dir_output(2, k);
    REG32(GPIO + 0x1C + 0x28) = ROW_MASK;            /* CLR_DATA pair 1: all rows low */
    REG32(GPIO + 0x18 + 0x28) = 1;                   /* SET_DATA pair 1: row 0 selected */
}

static u32 read_columns(unsigned row)
{
    u32 g3 = gpio_read_bank16(3);
    u32 col = ((g3 >> 1) & 0x3F) | (((g3 >> 9) & 1) << 6) | (((g3 >> 11) & 1) << 7);
    if (row == 0)
        col = (col & ~0x10u) | ((~gpio_read_bank16(5) >> 10) & 1) << 4;
    return col;
}

u64 keys_scan(void)
{
    u64 word = 0;
    unsigned row;
    for (row = 0; row < 6; row++) {
        REG32(GPIO + 0x1C + 0x28) = ROW_MASK;
        REG32(GPIO + 0x18 + 0x28) = 1u << row;
        delay_us(20);                                /* let the column lines settle (the original firmware reads one row per 1 ms tick) */
        word |= (u64)read_columns(row) << (8 * row);
    }
    return word;
}

/* Debouncing, following the original firmware (the switches have none in hardware, and their bounce time is
 * unmeasured). The matrix is sampled every KEYS_SAMPLE_MS, the spacing at which the original samples each key. A
 * press is taken on the first closed sample (0-6 ms); a release needs two consecutive open samples (6-12 ms), so a
 * one-sample opening while a key is held is ignored. The original filters releases this way only for bits 0-31;
 * its STEP keys release on the first open sample, so a one-sample opening there gives a release and a new
 * press. Enaga filters every key. */
#define KEYS_SAMPLE_MS 6u

static u64 debounced, open_once;             /* open_once: held keys whose last sample read open */
static u32 sampled_ms;

u64 keys_reset(u32 ms)
{
    open_once = 0;
    sampled_ms = ms;
    return debounced = keys_scan();
}

u64 keys_poll(u32 ms)
{
    u64 raw, open, released;
    if (ms - sampled_ms < KEYS_SAMPLE_MS) return debounced;
    sampled_ms = ms;
    raw = keys_scan();
    open = debounced & ~raw;
    released = open & open_once;
    open_once = open & ~released;
    return debounced = (debounced | raw) & ~released;
}
