#include "kernel.h"

/* Which device of the family Enaga runs on (docs/hardware.md "Installing and recovery"). The SmplTrek, Lofi-12 XT
 * and CyDrums are one board; only their bootloaders differ, and each opens its own update file by name. The
 * bootloader is still in DDR at the hand-over (BOOT 1.0.17: loaded at 0xC7074628-0xC70AEA60, "SmplTrek.bin" at
 * 0xC70A8BA0, from the smpltrek project's flash dump), and nothing overwrites it until Enaga loads an app, so
 * main() looks for the file names once, before that. The other bootloaders were never dumped, but are built from
 * the same code base: the search covers 2 MiB around where BOOT 1.0.17 sits, word-aligned (the name is a constant
 * string), and takes the first name found. About 50 ms on the emulator when no name is there (16 MiB took 0.4 s). */
#define DEVICE_SEARCH_START 0xC7000000u
#define DEVICE_SEARCH_END   0xC7200000u

static const struct { const char *file, *name; u32 id; } known[] = {
    { "SmplTrek.bin", "SMPLTREK", ENAGA_DEVICE_SMPLTREK },
    { "Lofi-12 XT.bin", "LOFI-12 XT", ENAGA_DEVICE_LOFI12XT },
    { "CyDrums.bin", "CYDRUMS", ENAGA_DEVICE_CYDRUMS },
};

u32 device_id = ENAGA_DEVICE_UNKNOWN;
const char *device_name = "UNKNOWN DEVICE";

static int names_file(const u8 *p, const char *file)   /* p holds `file` and its terminating zero */
{
    unsigned i;
    for (i = 0; file[i]; i++)
        if (p + i >= (const u8 *)DEVICE_SEARCH_END || p[i] != (u8)file[i]) return 0;
    return p + i < (const u8 *)DEVICE_SEARCH_END && p[i] == 0;
}

void device_detect(void)
{
    const u32 *w;
    unsigned k;
    u32 first[3];
    for (k = 0; k < 3; k++) {
        const char *f = known[k].file;
        first[k] = (u32)(u8)f[0] | (u32)(u8)f[1] << 8 | (u32)(u8)f[2] << 16 | (u32)(u8)f[3] << 24;
    }
    for (w = (const u32 *)DEVICE_SEARCH_START; w < (const u32 *)DEVICE_SEARCH_END; w++)
        for (k = 0; k < 3; k++)
            if (*w == first[k] && names_file((const u8 *)w, known[k].file)) {
                device_id = known[k].id;
                device_name = known[k].name;
                return;
            }
}
