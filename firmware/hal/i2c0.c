#include "hal.h"

/* I2C0 master, polled, for the codec at address 0x18 (docs/hardware.md "Audio").
 * Set-up as the original firmware's (TI StarterWare's I2CMasterInitExpClk for 24 MHz in, 8 MHz module clock, 100 kHz
 * bus): ICMDR = 0 (reset), ICPSC = 2, ICCLKL = ICCLKH = 35, ICSAR = 0x18, ICMDR = IRS, ICEMDR = 0. A register
 * write is a two-byte transfer: ICCNT = 2, ICMDR = STP | MST | TRX, then IRS, then STT; the original firmware's ISR
 * feeds the two bytes on ICXRDY, this feeds them by polling ICSTR. Every wait is bounded (10 ms). */
#define ICIMR   0x04
#define ICSTR   0x08
#define ICCLKL  0x0C
#define ICCLKH  0x10
#define ICCNT   0x14
#define ICSAR   0x1C
#define ICDXR   0x20
#define ICMDR   0x24
#define ICEMDR  0x2C
#define ICPSC   0x30

#define ICSTR_NACK   0x02u
#define ICSTR_ICXRDY 0x10u
#define ICSTR_SCD    0x20u
#define ICSTR_BB     0x1000u

#define ICMDR_IRS    0x0020u
#define ICMDR_TRX    0x0200u
#define ICMDR_MST    0x0400u
#define ICMDR_STP    0x0800u
#define ICMDR_STT    0x2000u

#define CODEC_ADDRESS 0x18

static void i2c0_init(void)
{
    REG32(I2C0 + ICMDR) = 0;
    REG32(I2C0 + ICPSC) = 2;
    REG32(I2C0 + ICCLKL) = 35;
    REG32(I2C0 + ICCLKH) = 35;
    REG32(I2C0 + ICSAR) = CODEC_ADDRESS;
    REG32(I2C0 + ICMDR) = ICMDR_IRS;
    REG32(I2C0 + ICEMDR) = 0;
}

static int wait_status(u32 bit, int set)          /* 0 when ICSTR's bit reaches the wanted state within 10 ms */
{
    u32 start = tsc_read();
    while (((REG32(I2C0 + ICSTR) & bit) != 0) != (set != 0))
        if (tsc_read() - start > 10 * TICKS_PER_MS) return -1;
    return 0;
}

int i2c0_write(u8 reg, u8 value)
{
    static int initialised;
    if (!initialised) { i2c0_init(); initialised = 1; }
    if (wait_status(ICSTR_BB, 0) < 0) return -1;
    REG32(I2C0 + ICSTR) = ICSTR_SCD | ICSTR_NACK;              /* clear the previous transfer's flags (write 1 to clear) */
    REG32(I2C0 + ICCNT) = 2;
    REG32(I2C0 + ICMDR) = ICMDR_STP | ICMDR_MST | ICMDR_TRX;   /* mode, then IRS, then start */
    REG32(I2C0 + ICMDR) |= ICMDR_IRS;
    REG32(I2C0 + ICIMR) = 0x30;                                /* SCD and ICXRDY, as the original firmware enables them (unused: polled) */
    REG32(I2C0 + ICMDR) |= ICMDR_STT;
    if (wait_status(ICSTR_ICXRDY, 1) < 0) return -1;
    REG32(I2C0 + ICDXR) = reg;
    if (wait_status(ICSTR_ICXRDY, 1) < 0) return -1;
    REG32(I2C0 + ICDXR) = value;
    if (wait_status(ICSTR_SCD, 1) < 0) return -1;
    if (REG32(I2C0 + ICSTR) & ICSTR_NACK) { REG32(I2C0 + ICSTR) = ICSTR_NACK; return -2; }
    return 0;
}
