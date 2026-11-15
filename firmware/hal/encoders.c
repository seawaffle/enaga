#include "hal.h"

/* Five quadrature encoders on GPIO (docs/hardware.md "Encoders"):
 * 0 (A) GP3[14]/[15], 1 (B) GP3[12]/[10], 2 (C) GP5[2]/[0], 3 (D) GP5[5]/[4], 4 (VALUE) GP5[7]/[8].
 * Decoded as the original firmware decodes them: phase 3 (both pins high) is the rest position and clears the
 * direction; the first phase after rest sets it (1: -1, 2: +1); every arrival at phase 0 with a direction set
 * counts one step. That is one count per detent on the unit (counting every edge gives four). */
int encoder_count[5];
static u8 last_phase[5];
static signed char direction[5];

void encoders_init(void)
{
    int i;
    pinmux_set(7, 0, 8); pinmux_set(7, 1, 8); pinmux_set(7, 3, 8); pinmux_set(7, 5, 8);
    pinmux_set(11, 7, 8);
    pinmux_set(12, 0, 8); pinmux_set(12, 2, 8); pinmux_set(12, 3, 8); pinmux_set(12, 5, 8); pinmux_set(12, 7, 8);
    pupd_clear_19_20();
    for (i = 0; i < 5; i++) last_phase[i] = 3;
}

static void phases(u8 out[5])
{
    u32 g3 = gpio_read_bank16(3), g5 = gpio_read_bank16(5);
    out[0] = ((g3 >> 14) & 1) | (((g3 >> 15) & 1) << 1);
    out[1] = ((g3 >> 12) & 1) | (((g3 >> 10) & 1) << 1);
    out[2] = ((g5 >> 2) & 1) | (((g5 >> 0) & 1) << 1);
    out[3] = ((g5 >> 5) & 1) | (((g5 >> 4) & 1) << 1);
    out[4] = ((g5 >> 7) & 1) | (((g5 >> 8) & 1) << 1);
}

void encoders_poll(void)
{
    u8 now[5];
    int i;
    phases(now);
    for (i = 0; i < 5; i++) {
        if (now[i] == last_phase[i]) continue;
        if (now[i] == 3) direction[i] = 0;
        else if (last_phase[i] == 3) direction[i] = now[i] == 1 ? -1 : now[i] == 2 ? 1 : 0;
        else if (now[i] == 0 && direction[i]) encoder_count[i] += direction[i];
        last_phase[i] = now[i];
    }
}
