#include "hal.h"

/* Register offsets per bank pair n: DIR 0x10 + 0x28n, OUT_DATA +0x14, SET_DATA +0x18, CLR_DATA +0x1C,
 * IN_DATA +0x20 (TI SPRUFL8B). A DIR bit of 1 is an input. */
static u32 pair_reg(unsigned bank, unsigned offset) { return GPIO + offset + 0x28 * (bank >> 1); }
static u32 bit(unsigned bank, unsigned pin) { return 1u << (((bank & 1) << 4) + pin); }

void gpio_dir_output(unsigned bank, unsigned pin) { REG32(pair_reg(bank, 0x10)) &= ~bit(bank, pin); }
void gpio_set(unsigned bank, unsigned pin) { REG32(pair_reg(bank, 0x18)) = bit(bank, pin); }
void gpio_clr(unsigned bank, unsigned pin) { REG32(pair_reg(bank, 0x1C)) = bit(bank, pin); }
u32 gpio_read_bank16(unsigned bank) { return (REG32(pair_reg(bank, 0x20)) >> ((bank & 1) << 4)) & 0xFFFF; }
