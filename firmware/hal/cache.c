#include "hal.h"

/* C674x cache controller block operations (as SYS/BIOS Cache_wbInv / Cache_inv do them, SPRUFK5A):
 * L2WIBAR/L2WIWC write back and invalidate a block through L2, which also keeps L1D and L1P coherent for
 * it; L2IBAR/L2IWC invalidate only. The word count is in 32-bit words; the operation is complete when the
 * count register reads 0. Needed after loading code and around buffers the SD controller fills. */
#define L2WIBAR 0x01844010u
#define L2WIWC  0x01844014u
#define L2IBAR  0x01844018u
#define L2IWC   0x0184401Cu
#define L2CFG    0x01840000u
#define L1PCFG   0x01840020u
#define L1DCFG   0x01840040u
#define L2WBINV  0x01845004u
#define L1DWBINV 0x01845044u
#define MAR(n)   (0x01848000u + 4u * (n))
#define DDR_MAR  192u                         /* MAR 192-199: 16 MiB each, 0xC0000000-0xC7FFFFFF */
#define L2_ALL   4u                           /* L2CFG L2MODE 4: all 256 KiB is cache (SPRUFK5A Table 4-4) */
#define L1_ALL   4u                           /* L1PMODE / L1DMODE 4: all 32 KiB is cache (Tables 2-2, 3-2) */

static void block_op(u32 bar, u32 wc, void *address, u32 bytes)
{
    u32 a = (u32)address & ~3u, words = (bytes + 3 + ((u32)address & 3)) >> 2;
    while (words) {
        u32 n = words > 0xFFFF ? 0xFFFF : words;      /* the count field is 16 bits */
        REG32(bar) = a;
        REG32(wc) = n;
        while (REG32(wc))
            ;
        a += n << 2;
        words -= n;
    }
}

void cache_wbinv(void *address, u32 bytes) { block_op(L2WIBAR, L2WIWC, address, bytes); }
void cache_inv(void *address, u32 bytes) { block_op(L2IBAR, L2IWC, address, bytes); }

u32 cache_config(void)
{
    u32 n, mars = 0;
    for (n = 0; n < 8; n++) mars |= (REG32(MAR(DDR_MAR + n)) & 1u) << n;
    return (REG32(L2CFG) & 7u) << 24 | (REG32(L1PCFG) & 7u) << 16 | (REG32(L1DCFG) & 7u) << 8 | mars;
}

static void global_op(u32 reg)
{
    REG32(reg) = 1;
    while (REG32(reg) & 1)
        ;
}

/* Enaga sets its own cache configuration rather than trusting the bootloader's (unknown; the 16 KiB L1D RAM
 * of the original firmware comes from its SYS/BIOS start-up, not the bootloader). Everything is cache, since
 * Enaga uses no on-chip RAM, and DDR is cacheable. Dirty lines are written back first, so none depends on
 * how a size change treats it; each mode is read back, which stalls until the change is complete. Turning
 * a MAR's PC bit on needs no maintenance: an uncached range has no lines in the caches. */
void cache_setup(void)
{
    u32 n;
    global_op(L1DWBINV);
    global_op(L2WBINV);
    REG32(L2CFG) = (REG32(L2CFG) & ~7u) | L2_ALL;
    (void)REG32(L2CFG);
    REG32(L1PCFG) = L1_ALL;
    (void)REG32(L1PCFG);
    REG32(L1DCFG) = L1_ALL;
    (void)REG32(L1DCFG);
    for (n = 0; n < 8; n++) REG32(MAR(DDR_MAR + n)) |= 1u;
}
