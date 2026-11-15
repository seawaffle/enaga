#include "hal.h"

/* The 128 x 128 x 4-bit OLED on SPI1 (docs/hardware.md "Display"), driven as the original firmware drives it:
 * PSC1 module 10; PINMUX5 nibbles 1, 2, 5 = SPI1 SCS0/CLK/SIMO0; SPIGCR0 reset then release; SPIGCR1 = master,
 * internal clock; SPIPC0 = 0x0E01 (CLK, SIMO, SOMI and SCS0 as SPI pins, SPRUH79C 28.2.19 step 4: without it
 * the first command byte never completes); SPIFMT0 = 0x00011308 (8-bit characters, prescale 19 for 9.3 MHz
 * from 186 MHz, clock phase bit 16); SPIDEF = 0xFF; SPIGCR1 enable (bit 24). Then the data/command GPIO GP2[11]
 * (PINMUX5 nibble 4) as an output, and four controller commands with D/C low: B3 F0, 81 FF, A0 07, AF
 * (display clock, contrast, remap, display on; an SSD1327-like controller, part number unconfirmed).
 *
 * Byte protocol: SPIDAT1 byte 3 = 0x10 (CSHOLD) and byte 2 = 0xFE (CSNR, CS0 active), then per
 * byte a write to SPIDAT1 with the character in the low bits and a wait on SPIBUF until not RXEMPTY; at
 * the end CSHOLD = 0 and CSNR = 0xFE again. oled_flush polls (about 7 ms), each byte's wait bounded at
 * 10 ms so a transfer that never completes is reported instead of spun on.
 *
 * Frames by EDMA, as the original firmware sends them: D/C high and the chip select held
 * as above; PaRAM 19 (event 19, SPI1 transmit) sends the 8192 bytes to SPIDAT1, one byte per event (ACNT 1,
 * SRC BIDX 1); PaRAM 18 (event 18, SPI1 receive) drains SPIBUF into a dummy word, so its completion (TCC 18)
 * means every byte has been shifted out; both events in shadow region 1; SPIINT0 DMAREQEN starts it. Nothing
 * interrupts: the main loop polls oled_dma_poll. Needs EDMA3 CC0/TC0 powered (the audio engine does that). */
#define SPIGCR0  0x00
#define SPIGCR1  0x04
#define SPIPC0   0x14
#define SPIDAT1  0x3C
#define SPIBUF   0x40
#define SPIDEF   0x4C
#define SPIFMT0  0x50
#define SPIINT0  0x08
#define RXEMPTY  0x80000000u
#define TXFULL   0x20000000u

#define EDMA_EMCR     (EDMA + 0x0308)
#define EDMA_DRAE1    (EDMA + 0x0348)
#define EDMA_S1_EECR  (EDMA + 0x2228)
#define EDMA_S1_EESR  (EDMA + 0x2230)
#define EDMA_S1_SECR  (EDMA + 0x2240)
#define EDMA_S1_IPR   (EDMA + 0x2268)
#define EDMA_S1_ICR   (EDMA + 0x2270)
#define PARAM(n)      (EDMA + 0x4000 + 0x20 * (n))
#define CH_RX 18
#define CH_TX 19
#define FRAME_BYTES 8192u

static int dma_busy;
static u32 dma_started, dma_dummy;
u32 oled_dma_frames, oled_dma_timeouts;

static int spi1_send(const u8 *buf, u32 len)
{
    u32 i, v, start;
    REG8(SPI1 + SPIDAT1 + 3) = 0x10;
    REG8(SPI1 + SPIDAT1 + 2) = 0xFE;
    for (i = 0; i < len; i++) {
        REG32(SPI1 + SPIDAT1) = 0x10FE0000u | buf[i];
        start = tsc_read();
        do {
            v = REG32(SPI1 + SPIBUF);
            if ((v & RXEMPTY) && tsc_read() - start > 10 * TICKS_PER_MS) {
                REG8(SPI1 + SPIDAT1 + 3) = 0x00;
                REG8(SPI1 + SPIDAT1 + 2) = 0xFE;
                return -1;
            }
        } while (v & RXEMPTY);
    }
    REG8(SPI1 + SPIDAT1 + 3) = 0x00;
    REG8(SPI1 + SPIDAT1 + 2) = 0xFE;
    return 0;
}

static int oled_command(const u8 *bytes, u32 len)
{
    gpio_clr(OLED_DC_BANK, OLED_DC_PIN);
    return spi1_send(bytes, len);
}

int oled_init(void)
{
    static const u8 clock[] = { 0xB3, 0xF0 }, contrast[] = { 0x81, 0xFF }, remap[] = { 0xA0, 0x07 }, on[] = { 0xAF };
    if (psc_enable(PSC1, PSC1_SPI1) < 0) return -2;
    pinmux_set(5, 1, 1);
    pinmux_set(5, 2, 1);
    pinmux_set(5, 5, 1);
    REG32(SPI1 + SPIGCR0) = 0;
    REG32(SPI1 + SPIGCR0) = 1;
    REG32(SPI1 + SPIGCR1) = 3;
    REG32(SPI1 + SPIPC0) = 0x0E01;
    REG32(SPI1 + SPIFMT0) = 0x00011308;
    REG32(SPI1 + SPIDEF) = 0xFF;
    REG32(SPI1 + SPIGCR1) = 0x01000003;
    pinmux_set(5, 4, 8);
    gpio_dir_output(OLED_DC_BANK, OLED_DC_PIN);
    if (oled_command(clock, 2) < 0 || oled_command(contrast, 2) < 0 || oled_command(remap, 2) < 0
        || oled_command(on, 1) < 0) return -3;
    return 0;
}

int oled_flush(const u8 *fb)
{
    gpio_set(OLED_DC_BANK, OLED_DC_PIN);
    return spi1_send(fb, 8192);
}

static void dma_stop(void)
{
    u32 start;
    REG32(SPI1 + SPIINT0) = 0;
    REG32(EDMA_S1_EECR) = (1u << CH_RX) | (1u << CH_TX);
    REG32(EDMA_S1_SECR) = (1u << CH_RX) | (1u << CH_TX);
    REG32(EDMA_EMCR) = (1u << CH_RX) | (1u << CH_TX);
    REG32(EDMA_S1_ICR) = (1u << CH_RX) | (1u << CH_TX);
    /* Stopped mid-frame (a timeout): let the byte in the shift register and the one queued behind it finish
     * (about 1 us each), then drain what they returned, so a polled transfer starts on an idle SPI. */
    start = tsc_read();
    while ((REG32(SPI1 + SPIBUF) & TXFULL) && tsc_read() - start < TICKS_PER_MS)
        ;
    delay_us(5);
    while (!(REG32(SPI1 + SPIBUF) & RXEMPTY) && tsc_read() - start < 2 * TICKS_PER_MS)
        ;
    REG8(SPI1 + SPIDAT1 + 3) = 0x00;           /* release the chip select */
    REG8(SPI1 + SPIDAT1 + 2) = 0xFE;
    dma_busy = 0;
}

int oled_dma_start(const u8 *fb)
{
    if (dma_busy) return -1;
    cache_wbinv((void *)fb, FRAME_BYTES);      /* the EDMA reads DDR, not the cache */
    while (!(REG32(SPI1 + SPIBUF) & RXEMPTY))  /* nothing left over from a polled transfer */
        ;
    gpio_set(OLED_DC_BANK, OLED_DC_PIN);
    REG8(SPI1 + SPIDAT1 + 3) = 0x10;
    REG8(SPI1 + SPIDAT1 + 2) = 0xFE;
    REG32(EDMA_DRAE1) |= (1u << CH_RX) | (1u << CH_TX);
    REG32(EDMA_S1_ICR) = (1u << CH_RX) | (1u << CH_TX);
    REG32(EDMA_S1_SECR) = (1u << CH_RX) | (1u << CH_TX);
    REG32(EDMA_EMCR) = (1u << CH_RX) | (1u << CH_TX);
    REG32(PARAM(CH_TX) + 0x00) = 0x00113000u;                  /* TCC 19, TCINTEN, A-synchronised */
    REG32(PARAM(CH_TX) + 0x04) = (u32)fb;
    REG32(PARAM(CH_TX) + 0x08) = (FRAME_BYTES << 16) | 1;      /* BCNT bytes of ACNT 1 */
    REG32(PARAM(CH_TX) + 0x0C) = SPI1 + SPIDAT1;
    REG32(PARAM(CH_TX) + 0x10) = 1;                            /* SRC BIDX 1, DST BIDX 0 */
    REG32(PARAM(CH_TX) + 0x14) = 0xFFFF;                       /* no link */
    REG32(PARAM(CH_TX) + 0x18) = 0;
    REG32(PARAM(CH_TX) + 0x1C) = 1;
    REG32(PARAM(CH_RX) + 0x00) = 0x00112000u;                  /* TCC 18, TCINTEN */
    REG32(PARAM(CH_RX) + 0x04) = SPI1 + SPIBUF;
    REG32(PARAM(CH_RX) + 0x08) = (FRAME_BYTES << 16) | 1;
    REG32(PARAM(CH_RX) + 0x0C) = (u32)&dma_dummy;
    REG32(PARAM(CH_RX) + 0x10) = 0;
    REG32(PARAM(CH_RX) + 0x14) = 0xFFFF;
    REG32(PARAM(CH_RX) + 0x18) = 0;
    REG32(PARAM(CH_RX) + 0x1C) = 1;
    REG32(EDMA_S1_EESR) = (1u << CH_RX) | (1u << CH_TX);
    dma_busy = 1;
    dma_started = tsc_read();
    REG32(SPI1 + SPIINT0) = 0x00010000;                        /* DMAREQEN: the transmit event starts it */
    return 0;
}

int oled_dma_poll(void)
{
    if (!dma_busy) return 0;
    if (REG32(EDMA_S1_IPR) & (1u << CH_RX)) {                  /* the last byte has come back: all sent */
        dma_stop();
        oled_dma_frames++;
        return 0;
    }
    if (tsc_read() - dma_started > 50 * TICKS_PER_MS) {        /* 7 ms expected */
        dma_stop();
        oled_dma_timeouts++;
        return 0;
    }
    return 1;
}

/* Wait for a frame in flight instead of cutting it short: the panel's write position only wraps after a full
 * 8192 bytes (no address commands are sent, as in the original firmware), so a partial frame would shift every later frame.
 * Works with interrupts off. A frame that never completes (50 ms) is stopped and counted as a timeout. */
void oled_dma_finish(void)
{
    while (oled_dma_poll())
        ;
}
