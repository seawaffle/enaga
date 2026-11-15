#include "hal.h"

/* C674x cache controller block operations (as SYS/BIOS Cache_wbInv / Cache_inv do them, SPRUFK5A):
 * L2WIBAR/L2WIWC write back and invalidate a block through L2, which also keeps L1D and L1P coherent for
 * it; L2IBAR/L2IWC invalidate only. The word count is in 32-bit words; the operation is complete when the
 * count register reads 0. Needed after loading code and around buffers the SD controller fills. */
#define L2WIBAR 0x01844010u
#define L2WIWC  0x01844014u
#define L2IBAR  0x01844018u
#define L2IWC   0x0184401Cu

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
