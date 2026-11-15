#include "hal.h"

/* GP4[11] is the power hold; the bootloader has already driven it high and set it as an output by the time
 * Enaga runs, so only its level is written (docs/hardware.md "Power"). */
void power_hold(void) { gpio_set(POWER_HOLD_BANK, POWER_HOLD_PIN); }
int power_key_down(void) { return !(gpio_read_bank16(POWER_KEY_BANK) & (1u << POWER_KEY_PIN)); }
int power_on_batteries(void) { return (gpio_read_bank16(POWER_SRC_BANK) >> POWER_SRC_PIN) & 1; }
void power_off(void) { gpio_clr(POWER_HOLD_BANK, POWER_HOLD_PIN); }

/* A stage that cannot be reported on the display: wait code x 5 s on the time-stamp counter, then release
 * the hold. The unit switches off that long after the bootloader's jump, so the stage can be read from a
 * stopwatch when there is no screen to show it. */
void fault(unsigned code)
{
    while (code--) {
        u32 start = tsc_read();
        while (tsc_read() - start < 5000 * TICKS_PER_MS)
            ;
    }
    power_off();
    for (;;)
        ;
}
