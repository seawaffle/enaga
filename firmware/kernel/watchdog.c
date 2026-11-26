#include "kernel.h"

/* The watchdog. TIMER64P0's period interrupt (every 10 ms, CPU interrupt 4: timer.c, irq.c) checks three things:
 *
 *   - the app's audio callback has not been running for WATCHDOG_AUDIO_MS. The callback runs with this
 *     interrupt allowed to nest (audio.c), so a callback that never returns is seen;
 *   - the main loop has called watchdog_feed within WATCHDOG_MAIN_MS;
 *   - an app's stop() has not run longer than APPS_STOP_MS (apps.c), fed or not.
 *
 * A hang while an app is loaded, starting or stopping is the app's: it is dropped as with fatal() and the
 * kernel loop continues. With no app it is Enaga's own: the kernel crash screen. Times come from the
 * time-stamp counter (32 bits, 11.5 s).
 *
 * Not covered: code that hangs with interrupts disabled (a kernel handler, or an app that clears GIE). That
 * needs the C6748's hardware watchdog (TIMER64P1 in watchdog mode), which resets the chip. */
#define WATCHDOG_MAIN_MS  1000u
#define WATCHDOG_AUDIO_MS 100u

volatile u32 watchdog_ticks;
u32 watchdog_irp;
int watchdog_tripped;
static volatile u32 fed;

void watchdog_feed(void) { fed = tsc_read(); }

void watchdog_start(void)
{
    watchdog_feed();
    irq_enable(1u << IRQ_WATCHDOG);
}

void watchdog_isr(void)
{
    u32 now = tsc_read();
    watchdog_ticks++;
    if (audio_in_callback && now - audio_callback_start > WATCHDOG_AUDIO_MS * TICKS_PER_MS)
        apps_fatal("APP HUNG IN AUDIO");
    if (app_stopping && now - app_stop_start > APPS_STOP_MS * TICKS_PER_MS)
        apps_fatal("APP STOP TOO LONG");
    if (now - fed > WATCHDOG_MAIN_MS * TICKS_PER_MS) {
        if (app_current || app_busy) apps_fatal("APP NOT RESPONDING");
        watchdog_irp = irp_read();
        watchdog_tripped = 1;
        kernel_crash("WATCHDOG");
    }
}
