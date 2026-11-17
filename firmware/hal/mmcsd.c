#include "hal.h"

/* SD card on MMCSD0 (0x01C40000), polled, single-block transfers (docs/hardware.md "SD card"). The controller
 * set-up is the original firmware's: PINMUX10 nibbles 0-5 = MMCSD0 pins, nibbles 6-7
 * = GP4[0] card detect and GP4[1] (driven low); PSC0 module 5; MMCCTL reset, MMCCLK rate 232 (186 MHz /
 * 466 = 399 kHz for identification), MMCTOR/MMCTOD maximum, resets released, clock enabled. The card
 * initialisation (CMD0, CMD8, ACMD41, CMD2, CMD3, CMD7, ACMD6, CMD16) is the SD specification's, written from
 * the SD and C6748 register descriptions; it has mounted cards and loaded apps on the unit. Register bits use
 * TI's names (StarterWare hw_mmcsd.h). MMCST0 clears on read, so every
 * read is accumulated in `status` and a flag stays pending until the wait that wants it consumes it:
 * DATDNE can arrive in the same read as the last DRRDY. A write ends with DATDNE (which the manual says waits
 * for the CRC check and the programming, SPRUH79C Table 25-8 note) and then CMD13 until the card reports the
 * tran state, as the original firmware does; BSYDNE is never waited for, because the controller only raises
 * it for a command sent with BSYEXP (SPRUH79C 25.2.9.7.6). No command sets PPLEN: the original firmware drives
 * CMD open-drain throughout, so that is the setting known to work on this board. */
#define MMC   0x01C40000u
#define MMCCTL    0x00
#define MMCCLK    0x04
#define MMCST0    0x08
#define MMCST1    0x0C
#define MMCIM     0x10
#define MMCTOR    0x14
#define MMCTOD    0x18
#define MMCBLEN   0x1C
#define MMCNBLK   0x20
#define MMCDRR    0x28
#define MMCDXR    0x2C
#define MMCCMD    0x30
#define MMCARGHL  0x34
#define MMCRSP01  0x38
#define MMCRSP23  0x3C
#define MMCRSP45  0x40
#define MMCRSP67  0x44
#define MMCFIFOCTL 0x74

#define ST0_DATDNE 0x0001
#define ST0_BSYDNE 0x0002
#define ST0_RSPDNE 0x0004
#define ST0_TOUTRD 0x0008
#define ST0_TOUTRS 0x0010
#define ST0_CRCWR  0x0020
#define ST0_CRCRD  0x0040
#define ST0_CRCRS  0x0080
#define ST0_DXRDY  0x0200
#define ST0_DRRDY  0x0400
#define ST0_ERRORS (ST0_TOUTRD | ST0_TOUTRS | ST0_CRCWR | ST0_CRCRD | ST0_CRCRS)

#define CMD_BSYEXP 0x0100
#define RSP_NONE   0x0000
#define RSP_48     0x0200          /* R1, R6, R7 */
#define RSP_136    0x0400          /* R2 */
#define RSP_48NOCRC 0x0600         /* R3 */
#define CMD_DTRW   0x0800
#define CMD_WDATX  0x2000
#define CMD_INITCK 0x4000
#define CMD_DCLR   0x8000

static int high_capacity, card_ready;
static u32 rca;
static u32 status;                 /* MMCST0 bits seen since the command was issued and not yet consumed */
static u32 last_st;                /* what the last command's wait returned, for the error report */
int sd_error;
u32 sd_error_status;

int sd_present(void) { return !(gpio_read_bank16(4) & 1); }

static u32 wait_status(u32 want, u32 timeout_ms)
{
    u32 st, start = timer_ticks();
    for (;;) {
        status |= REG32(MMC + MMCST0);
        if (status & (want | ST0_ERRORS)) {
            st = status;
            status &= ~want;                     /* errors stay until the next command resets them */
            return st;
        }
        if (timer_ticks() - start > timeout_ms * TICKS_PER_MS) return status | ST0_TOUTRS;
    }
}

/* Send a command; returns the MMCST0 bits seen, with ST0_RSPDNE set and no error on success. */
static u32 command(u32 index, u32 arg, u32 flags)
{
    status = 0;
    (void)REG32(MMC + MMCST0);                 /* drop what the previous transaction left behind */
    REG32(MMC + MMCARGHL) = arg;
    REG32(MMC + MMCCMD) = (index & 0x3F) | flags;
    last_st = wait_status(ST0_RSPDNE, 500);
    return last_st;
}

static int ok(u32 st) { return (st & ST0_RSPDNE) && !(st & ST0_ERRORS); }
/* A data-phase flag counts only without an error: a CRC error or time-out can come in the same MMCST0 read. */
static int data_ok(u32 st, u32 want) { return (st & want) && !(st & ST0_ERRORS); }
static u32 response(void) { return REG32(MMC + MMCRSP67); }   /* bits 39:8 of a 48-bit response */

static int app_command(u32 index, u32 arg, u32 flags)
{
    if (!ok(command(55, rca << 16, RSP_48))) return 0;
    return ok(command(index, arg, flags));
}

static void controller_init(void)
{
    unsigned k;
    for (k = 0; k < 6; k++) pinmux_set(10, k, 2);    /* MMCSD0 CLK, CMD, DAT0-3 */
    pinmux_set(10, 6, 8);                            /* GP4[0] card detect */
    pinmux_set(10, 7, 8);                            /* GP4[1] */
    gpio_dir_output(4, 1);
    gpio_clr(4, 1);
    psc_enable(PSC0, 5);
    REG32(MMC + MMCCTL) = 3;                         /* CMD and DAT logic in reset */
    REG32(MMC + MMCCLK) = 0;
    REG32(MMC + MMCCTL) = 3;                         /* the original firmware writes the reset twice more; kept */
    REG32(MMC + MMCCTL) = 3;
    REG32(MMC + MMCCLK) = 232;                       /* 399 kHz, clock off */
    REG32(MMC + MMCTOR) = 0x3FFFF;
    REG32(MMC + MMCTOD) = 0xFFFF;
    REG32(MMC + MMCCTL) = 0;                         /* out of reset, 1-bit bus */
    REG32(MMC + MMCCLK) = 0x100 | 232;               /* clock on */
    REG32(MMC + MMCIM) = 0;                          /* polled: no interrupts (the original firmware uses 0x7D) */
}

static int fail(int code)             /* sd_init failures are shown on the status page: step and MMCST0 bits */
{
    sd_error = code;
    sd_error_status = last_st;
    return code;
}

int sd_init(void)
{
    u32 st, ocr, i;
    card_ready = 0;
    last_st = 0;
    if (!sd_present()) return fail(-1);
    controller_init();
    delay_us(2000);
    command(0, 0, RSP_NONE | CMD_INITCK);            /* GO_IDLE_STATE, with 80 initialisation clocks */
    delay_us(1000);
    st = command(8, 0x1AA, RSP_48);      /* SEND_IF_COND: 2.7-3.6 V, check pattern */
    high_capacity = 0;
    if (ok(st) && (response() & 0xFFF) == 0x1AA) {
        for (i = 0; i < 2000; i++) {                 /* ACMD41 with HCS until the card leaves busy */
            if (!app_command(41, 0x40FF8000, RSP_48NOCRC)) return fail(-2);
            ocr = response();
            if (ocr & 0x80000000u) { high_capacity = (ocr >> 30) & 1; break; }
            delay_us(1000);
        }
        if (i == 2000) return fail(-3);
    } else {                                         /* SD v1 or MMC: ACMD41 without HCS */
        for (i = 0; i < 2000; i++) {
            if (!app_command(41, 0x00FF8000, RSP_48NOCRC)) return fail(-4);
            if (response() & 0x80000000u) break;
            delay_us(1000);
        }
        if (i == 2000) return fail(-5);
    }
    if (!ok(command(2, 0, RSP_136))) return fail(-6);          /* ALL_SEND_CID */
    if (!ok(command(3, 0, RSP_48))) return fail(-7);           /* SEND_RELATIVE_ADDR */
    rca = response() >> 16;
    if (!ok(command(7, rca << 16, RSP_48 | CMD_BSYEXP))) return fail(-8);   /* SELECT_CARD */
    if (app_command(6, 2, RSP_48))                          /* SET_BUS_WIDTH 4 */
        REG32(MMC + MMCCTL) = 4;                                       /* WIDTH0: 4-bit bus */
    if (!ok(command(16, 512, RSP_48))) return fail(-9);         /* SET_BLOCKLEN */
    REG32(MMC + MMCCLK) = 0x100 | 3;                 /* 186 MHz / 8 = 23 MHz for data transfers */
    card_ready = 1;
    return 0;
}

static void fifo_setup(int write)
{
    REG32(MMC + MMCFIFOCTL) = 1;                     /* reset */
    REG32(MMC + MMCFIFOCTL) = 0;
    REG32(MMC + MMCFIFOCTL) = 4 | (write ? 2 : 0);   /* 64-byte level, direction, 4-byte access */
}

int sd_read(u8 *buffer, u32 sector, u32 count)
{
    u32 *out = (u32 *)buffer;
    if (!card_ready || ((u32)buffer & 3)) return -1;
    while (count--) {
        u32 i, st;
        fifo_setup(0);
        REG32(MMC + MMCBLEN) = 512;
        REG32(MMC + MMCNBLK) = 1;
        st = command(17, high_capacity ? sector : sector * 512, RSP_48 | CMD_WDATX | CMD_DCLR);
        if (!ok(st)) return -2;
        for (i = 0; i < 512 / 4; i += 16) {          /* 64-byte FIFO fills */
            u32 k;
            if (!data_ok(wait_status(ST0_DRRDY, 500), ST0_DRRDY)) return -3;
            for (k = 0; k < 16; k++) *out++ = REG32(MMC + MMCDRR);
        }
        if (!data_ok(wait_status(ST0_DATDNE, 500), ST0_DATDNE)) return -4;
        sector++;
    }
    return 0;
}

int sd_write(const u8 *buffer, u32 sector, u32 count)
{
    const u32 *in = (const u32 *)buffer;
    if (!card_ready || ((u32)buffer & 3)) return -1;
    while (count--) {
        u32 i, st, start;
        fifo_setup(1);
        REG32(MMC + MMCBLEN) = 512;
        REG32(MMC + MMCNBLK) = 1;
        st = command(24, high_capacity ? sector : sector * 512, RSP_48 | CMD_WDATX | CMD_DTRW | CMD_DCLR);
        if (!ok(st)) return -2;
        for (i = 0; i < 512 / 4; i += 16) {
            u32 k;
            if (!data_ok(wait_status(ST0_DXRDY, 500), ST0_DXRDY)) return -3;
            for (k = 0; k < 16; k++) REG32(MMC + MMCDXR) = *in++;
        }
        if (!data_ok(wait_status(ST0_DATDNE, 500), ST0_DATDNE)) return -4;
        start = timer_ticks();                       /* CMD13 until the card is back in tran (state 4) */
        for (;;) {
            if (!ok(command(13, rca << 16, RSP_48))) return -5;
            if ((response() & 0x1E00) == 0x800) break;
            if (timer_ticks() - start > 1000u * TICKS_PER_MS) return -6;      /* still programming after 1 s */
        }
        sector++;
    }
    return 0;
}
