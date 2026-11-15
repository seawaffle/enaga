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

/* Debouncing (the switches have none in hardware). A key's change is taken on the first scan that sees it, so a
 * press adds no latency; the key is then held at its new state for KEYS_SETTLE_MS, so contact bounce after a
 * press or a release never becomes more changes. A change still present when the time is up is taken then:
 * nothing is lost, a release within the settling time is only late. (The original firmware samples each row
 * every 6 ms and reports what it sees.) */
#define KEYS_SETTLE_MS 10u

static u64 debounced, settling;
static u32 changed_ms[48];

u64 keys_reset(void)
{
    settling = 0;
    return debounced = keys_scan();
}

u64 keys_poll(u32 ms)
{
    u64 raw = keys_scan(), changed;
    unsigned i;
    for (i = 0; settling && i < 48; i++)
        if (((settling >> i) & 1) && ms - changed_ms[i] >= KEYS_SETTLE_MS) settling &= ~(1ull << i);
    changed = (raw ^ debounced) & ~settling;
    for (i = 0; changed >> i; i++)
        if ((changed >> i) & 1) changed_ms[i] = ms;
    settling |= changed;
    return debounced ^= changed;
}
