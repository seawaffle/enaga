#include "hal.h"

/* SYSCFG0 PINMUXn: read-modify-write of one nibble. The original firmware never writes the KICK registers
 * (the bootloader leaves SYSCFG unlocked), so neither does this. */
void pinmux_set(unsigned reg, unsigned nibble, unsigned value)
{
    u32 a = SYSCFG0 + 0x120 + 4 * reg;
    u32 v = REG32(a);
    v &= ~(0xFu << (4 * nibble));
    v |= (value & 0xF) << (4 * nibble);
    REG32(a) = v;
}

/* SYSCFG1 PUPD_ENA bits 19 and 20 cleared, as both the key-matrix and encoder set-up do (pupd_set_19_20(0)). */
void pupd_clear_19_20(void)
{
    REG32(SYSCFG1 + 0x0C) &= ~(3u << 19);
}
