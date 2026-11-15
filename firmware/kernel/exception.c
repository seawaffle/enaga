#include "kernel.h"

/* CPU exceptions. start.s nmi_entry records the state and calls enaga_exception on its own stack.
 *
 * An exception at an address inside the app window is the app's fault: the app is dropped exactly as when
 * it calls fatal() (from either context), the reason and the faulting PC are kept for Enaga's status page, and
 * the kernel loop continues on a fresh stack. Anywhere else (Enaga's own code, or a jump to nowhere) it is a
 * kernel crash: audio is silenced and muted, the crash screen shows what happened, and the unit waits for the
 * power key. Only internal and software exceptions are enabled (TSR.GEE); a fault in Enaga code called by an
 * app (a host function given a bad pointer) currently counts as a kernel crash. */

u32 exception_record[6];
u64 exception_stack[1024];                    /* 8 KiB; start.s starts SP at its last 8-byte slot */
u32 crash_pc, crash_efr, crash_ierr;
int kernel_crashed;

static char *hex8(char *out, u32 v)
{
    int i;
    for (i = 7; i >= 0; i--) *out++ = "0123456789ABCDEF"[(v >> (4 * i)) & 15];
    return out;
}

static void line(int y, const char *label, u32 value, u8 grey)
{
    char text[24], *p = text;
    while (*label) *p++ = *label++;
    p = hex8(p, value);
    *p = 0;
    gfx_text(0, y, text, grey);
}

void kernel_crash(const char *reason)
{
    u32 down_since = 0;
    int down = 0;
    irq_global_disable();
    kernel_crashed = 1;
    audio_silence_now();
    codec_mute();
    oled_dma_finish();
    log_line("KERNEL CRASH");
    log_line(reason);
    gfx_clear(0);
    gfx_text(0, 0, "ENAGA CRASH", 15);
    gfx_text(0, 12, reason, 15);
    line(30, "PC   ", exception_record[0], 12);
    line(40, "EFR  ", exception_record[1], 12);
    line(50, "IERR ", exception_record[2], 12);
    line(60, "NTSR ", exception_record[3], 12);
    line(70, "SP   ", exception_record[4], 12);
    line(80, "B3   ", exception_record[5], 12);
    gfx_text(0, 100, "HOLD POWER TO SWITCH", 8);
    gfx_text(0, 109, "OFF", 8);
    gfx_present();
    for (;;) {                                /* the power key, held one second, switches off */
        if (!power_key_down()) { down = 0; continue; }
        if (!down) { down = 1; down_since = tsc_read(); }
        else if (tsc_read() - down_since > 1000 * TICKS_PER_MS) { power_off(); for (;;) ; }
    }
}

void enaga_exception(void)
{
    u32 pc = exception_record[0];
    if (app_current && pc >= ENAGA_APP_BASE && pc < ENAGA_APP_LIMIT) {
        crash_pc = pc;
        crash_efr = exception_record[1];
        crash_ierr = exception_record[2];
        apps_drop("CPU EXCEPTION");
        if (irq_active) irq_unwind();
        exception_resume(kernel_loop, KERNEL_SP);
    }
    kernel_crash(exception_record[1] & 1 ? "SOFTWARE EXCEPTION" : "CPU EXCEPTION");
}
