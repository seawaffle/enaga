#include "hal.h"

/* Time comes from the CPU time-stamp counter (tsc_read, 372 MHz), enabled first thing in main: a clock that
 * cannot fail to run. TIMER64P0 runs continuously with a 10 ms period (TCR: PWID12 = 1, ENAMODE12 =
 * continuous; PRD12 = 240,000 at its believed 24 MHz input) and raises event 4 (TINT12) at each period, the
 * original firmware's clock tick: the kernel's watchdog interrupt (kernel/watchdog.c). Nothing measures time
 * with it, so a wrong input frequency only changes how often the watchdog looks. timer64_counts() reports
 * whether it advances. */

static u32 last_ticks, ms_count, ms_remainder;

u32  irq_global_disable(void);                /* start.s */
void irq_global_enable(void);

void timer_init(void)
{
    REG32(TIMER0 + 0x20) = 0;              /* TCR: stopped */
    REG32(TIMER0 + 0x18) = TIMER_PERIOD;   /* PRD12 */
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

u32 timer_ms(void)                          /* both contexts call it (USB MIDI from the audio interrupt, an app's ms()) */
{
    u32 gie = irq_global_disable(), result;
    u32 now = tsc_read();
    ms_remainder += now - last_ticks;      /* wraps correctly in unsigned arithmetic */
    last_ticks = now;
    ms_count += ms_remainder / TICKS_PER_MS;   /* a constant divisor: GCC multiplies, no libgcc routine (0.2.9: the
                                                  subtraction loop it replaces cost a few cycles per elapsed ms) */
    ms_remainder %= TICKS_PER_MS;
    result = ms_count;
    if (gie) irq_global_enable();
    return result;
}

void delay_us(u32 us)
{
    u32 start = tsc_read();
    while (tsc_read() - start < us * 372u)
        ;
}
