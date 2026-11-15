#include "hal.h"

/* Module enable, as the original firmware does it for every module (docs/hardware.md): MDCTL(n) = ENABLE,
 * PTCMD = GO0, wait while PTSTAT.GOSTAT0, then wait until MDSTAT(n).STATE == ENABLE. Each wait is bounded
 * at 100 ms; -1 means the PSC never completed. */
int psc_enable(u32 base, unsigned module)
{
    u32 start;
    REG32(base + 0xA00 + 4 * module) = 3;
    REG32(base + 0x120) = 1;
    for (start = tsc_read(); REG32(base + 0x128) & 1;)
        if (tsc_read() - start > 100 * TICKS_PER_MS) return -1;
    for (start = tsc_read(); (REG32(base + 0x800 + 4 * module) & 0x1F) != 3;)
        if (tsc_read() - start > 100 * TICKS_PER_MS) return -1;
    return 0;
}
