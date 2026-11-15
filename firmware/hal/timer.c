#include "hal.h"

/* Time comes from the CPU time-stamp counter (tsc_read, 372 MHz), enabled first thing in main: a clock that
 * cannot fail to run. TIMER64P0 is also configured as the original firmware's free-running counter (TCR:
 * PWID12 = 1, ENAMODE12 = continuous) but only observed: timer64_counts() reports whether it advances, which
 * checks its 24 MHz input without depending on it. */

static u32 last_ticks, ms_count, ms_remainder;

void timer_init(void)
{
    REG32(TIMER0 + 0x20) = 0;              /* TCR: stopped */
    REG32(TIMER0 + 0x18) = 0xFFFFFFFFu;    /* PRD12 */
    REG32(TIMER0 + 0x10) = 0;              /* TIM12 */
    REG32(TIMER0 + 0x04) = 0;              /* EMUMGT */
    REG32(TIMER0 + 0x08) = 0;              /* GPINTGPEN */
    REG32(TIMER0 + 0x0C) = 0;              /* GPDATGPDIR */
    REG32(TIMER0 + 0x24) = 5;              /* TGCR: TIM12 out of reset, dual 32-bit unchained */
    REG32(TIMER0 + 0x20) = 0x90;           /* TCR: continuous, PWID12 = 1 */
    last_ticks = tsc_read();
    ms_count = 0;
    ms_remainder = 0;
}

int timer64_counts(void)
{
    u32 before = REG32(TIMER0 + 0x10);
    delay_us(1000);
    return REG32(TIMER0 + 0x10) != before;
}

u32 timer_ticks(void) { return tsc_read(); }

u32 timer_ms(void)
{
    u32 now = tsc_read();
    ms_remainder += now - last_ticks;      /* wraps correctly in unsigned arithmetic */
    last_ticks = now;
    ms_count += ms_remainder / TICKS_PER_MS;   /* a constant divisor: GCC multiplies, no libgcc routine (0.2.9: the
                                                  subtraction loop it replaces cost a few cycles per elapsed ms) */
    ms_remainder %= TICKS_PER_MS;
    return ms_count;
}

void delay_us(u32 us)
{
    u32 start = tsc_read();
    while (tsc_read() - start < us * 372u)
        ;
}
