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
#define MAR(n)   (0x01848000u + 4u * (n))     /* MAR n covers 16 MiB from n << 24; MAR 0-15 are read-only */
#define DDR_MAR  192u                         /* MAR 192-199: 0xC0000000-0xC7FFFFFF */
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

u32 cache_at_entry[11] __attribute__((section(".data")));   /* start.s: L2CFG, L1PCFG, L1DCFG, MAR 192-199 */

static u32 summary(u32 l2, u32 l1p, u32 l1d, const u32 *mars)
{
    u32 n, bits = 0;
    for (n = 0; n < 8; n++) bits |= (mars[n] & 1u) << n;
    return (l2 & 7u) << 24 | (l1p & 7u) << 16 | (l1d & 7u) << 8 | bits;
}

u32 cache_entry_config(void)
{
    return summary(cache_at_entry[0], cache_at_entry[1], cache_at_entry[2], cache_at_entry + 3);
}

u32 cache_config(void)
{
    u32 n, mars[8];
    for (n = 0; n < 8; n++) mars[n] = REG32(MAR(DDR_MAR + n));
    return summary(REG32(L2CFG), REG32(L1PCFG), REG32(L1DCFG), mars);
}

static void global_op(u32 reg)
{
    REG32(reg) = 1;
    while (REG32(reg) & 1)
        ;
}

/* Enaga sets every cache register itself. The bootloader (BOOT 1.0.17, from a static trace and simulation, not
 * yet seen on a unit) leaves L2 all RAM, L1D half cache and MAR 196-198 uncached; the original firmware's own
 * start-up then makes L2 all cache. Enaga uses no on-chip RAM, so everything is cache, and only DDR is
 * cacheable. start.s has already written back L1D and invalidated L1P; L1D and L2 are written back again here
 * so no dirty line depends on how a size change treats it, and so clearing a MAR leaves nothing cached for its
 * range. Each mode is read back, which stalls until the change is complete. */
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
    for (n = 16; n < 256; n++) REG32(MAR(n)) = n >= DDR_MAR && n < DDR_MAR + 8 ? 1u : 0u;
}
