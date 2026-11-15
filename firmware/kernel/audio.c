#include "kernel.h"

/* The audio engine. Bring-up uses the original firmware's order and values (docs/hardware.md "Audio"): audio
 * GPIOs, EDMA3 CC0 and McASP0 power, the ping-pong rings, the codec over I2C0, McASP0's start handshake, unmute
 * and the analogue power sequence. How Enaga runs it:
 *
 *   - Blocks are AUDIO_FRAMES (64) frames, and the CPU refills them: the transmit ring's completion (TCC 1,
 *     TCINTEN) is enabled in shadow region 1, whose interrupt reaches the DSP as event 8 -> CPU interrupt 8
 *     (irq.c). Each interrupt converts the newest complete receive block to float, calls the app's callback,
 *     and converts its output into the transmit buffer that has just finished playing. That buffer plays
 *     again one block later, so the callback's deadline is one block (1.33 ms, AUDIO_BUDGET_CYCLES).
 *   - The rings keep the original firmware's one-word-short first transfer (127 words here), which puts even
 *     buffer words in slot 1, the left channel (unconfirmed: still to be checked by ear on the unit).
 *     Even words carry L, odd words R; samples are left-aligned 32-bit words, full scale +-2^31.
 *   - Buffers live in DDR behind the caches: the receive block is invalidated before it is read and the
 *     transmit block written back after it is filled.
 *
 * Cost accounting: cycles from interrupt entry to exit per block (audio_cycles_last/peak) against
 * AUDIO_BUDGET_CYCLES; a block that took longer than its budget counts as an overrun. */

#define FRAMES AUDIO_FRAMES
#define WORDS (2 * FRAMES)

#define EDMA_DMAQNUM0 (EDMA + 0x0240)
#define EDMA_EMCR     (EDMA + 0x0308)
#define EDMA_EMCRH    (EDMA + 0x030C)
#define EDMA_QEMCR    (EDMA + 0x0314)
#define EDMA_CCERRCLR (EDMA + 0x031C)
/* Shadow region 1: the C6748 DSP's region, whose completions raise event 8 (EDMA3_0_CC0_INT1). */
#define EDMA_DRAE1    (EDMA + 0x0348)
#define EDMA_S1_EESR  (EDMA + 0x2230)
#define EDMA_S1_SECR  (EDMA + 0x2240)
#define EDMA_S1_IESR  (EDMA + 0x2260)
#define EDMA_S1_IPR   (EDMA + 0x2268)
#define EDMA_S1_ICR   (EDMA + 0x2270)
#define EDMA_S1_IEVAL (EDMA + 0x2278)
#define PARAM(n)      (EDMA + 0x4000 + 0x20 * (n))

#define GBLCTL   (MCASP0 + 0x44)
#define RGBLCTL  (MCASP0 + 0x60)
#define RMASK    (MCASP0 + 0x64)
#define RFMT     (MCASP0 + 0x68)
#define AFSRCTL  (MCASP0 + 0x6C)
#define ACLKRCTL (MCASP0 + 0x70)
#define AHCLKRCTL (MCASP0 + 0x74)
#define RTDM     (MCASP0 + 0x78)
#define RINTCTL  (MCASP0 + 0x7C)
#define RSTAT    (MCASP0 + 0x80)
#define RCLKCHK  (MCASP0 + 0x88)
#define XGBLCTL  (MCASP0 + 0xA0)
#define XMASK    (MCASP0 + 0xA4)
#define XFMT     (MCASP0 + 0xA8)
#define AFSXCTL  (MCASP0 + 0xAC)
#define ACLKXCTL (MCASP0 + 0xB0)
#define AHCLKXCTL (MCASP0 + 0xB4)
#define XTDM     (MCASP0 + 0xB8)
#define XINTCTL  (MCASP0 + 0xBC)
#define XSTAT    (MCASP0 + 0xC0)
#define XCLKCHK  (MCASP0 + 0xC8)
#define SRCTL(n) (MCASP0 + 0x180 + 4 * (n))
#define PFUNC    (MCASP0 + 0x10)
#define PDIR     (MCASP0 + 0x14)


volatile u32 audio_blocks, audio_overruns, audio_cycles_last, audio_cycles_peak;
volatile int audio_in_callback;
volatile u32 audio_callback_start;
int audio_status, audio_codec_errors;

static u32 tx_a[WORDS] __attribute__((aligned(128)));
static u32 tx_b[WORDS] __attribute__((aligned(128)));
static u32 rx_c[WORDS] __attribute__((aligned(128)));
static u32 rx_d[WORDS] __attribute__((aligned(128)));
static float in_block[WORDS], out_block[WORDS];
static u32 tx_done, rx_done;
static volatile audio_callback_t callback;
static void *volatile callback_context;

static void gpio_setup(void)                           /* the original firmware's board set-up, in its order */
{
    unsigned k;
    for (k = 0; k < 4; k++) pinmux_set(9, k, 8);       /* GP4[15], [14], [13], [12] */
    pinmux_set(9, 5, 8);                               /* GP4[10] */
    pinmux_set(4, 4, 8); pinmux_set(4, 5, 8);          /* GP1[3], GP1[2] */
    pinmux_set(5, 6, 8); pinmux_set(5, 7, 8);          /* GP2[9], GP2[8] */
    pinmux_set(7, 7, 8);                               /* GP3[8], the jack detect input */
    pinmux_set(11, 4, 8);                              /* GP5[11] */
    pinmux_set(16, 2, 8); pinmux_set(16, 3, 8);        /* GP7[15], GP7[14] */
    gpio_dir_output(4, 12); gpio_dir_output(4, 14); gpio_dir_output(4, 15); gpio_dir_output(4, 13); gpio_dir_output(4, 10);
    gpio_dir_output(2, 9); gpio_dir_output(2, 8);
    gpio_dir_output(1, 2); gpio_dir_output(7, 14); gpio_dir_output(7, 15);
    gpio_dir_output(5, 11); gpio_dir_output(1, 3); gpio_dir_output(2, 8);
    gpio_clr(4, 12); gpio_clr(4, 14); gpio_clr(4, 15);  /* analogue power off */
    gpio_set(4, 13);                                   /* fixed high (role unknown) */
    gpio_clr(1, 2);                                    /* built-in mic switch off */
    gpio_set(7, 14); gpio_clr(7, 15);                  /* INPUT L: line front end, +10 dB stage off */
    gpio_clr(5, 11);                                   /* INPUT R mic stage off */
    gpio_set(1, 3);                                    /* line PAD off */
    gpio_set(2, 9); gpio_set(2, 8);                    /* output and speaker muted */
    gpio_clr(4, 10);                                   /* codec held in reset */
}

static int wait_gblctl(u32 value)                      /* the start handshake: each step reads back within 10 ms */
{
    u32 start = tsc_read();
    while ((REG32(GBLCTL) & value) != value)
        if (tsc_read() - start > 10 * TICKS_PER_MS) return -1;
    return 0;
}

static void param_set(unsigned n, u32 opt, u32 src, u32 dst, u32 bcnt, u32 bidx, u32 link)
{
    REG32(PARAM(n) + 0x00) = opt;
    REG32(PARAM(n) + 0x04) = src;
    REG32(PARAM(n) + 0x08) = (bcnt << 16) | 4;         /* ACNT 4: one 32-bit word; BCNT words per transfer */
    REG32(PARAM(n) + 0x0C) = dst;
    REG32(PARAM(n) + 0x10) = bidx;                     /* SRCBIDX | DSTBIDX << 16 */
    REG32(PARAM(n) + 0x14) = (bcnt << 16) | link;      /* BCNTRLD = BCNT (unused with CCNT 1), LINK to PaRAM `link` */
    REG32(PARAM(n) + 0x18) = 0;
    REG32(PARAM(n) + 0x1C) = 1;                        /* CCNT 1 */
}

static void edma_setup(void)
{
    const u32 rx_opt = 0x00100201, tx_opt = 0x00101202;    /* the original values; transmit adds TCINTEN (TCC 1) */
    const u32 first = WORDS - 1;                           /* the original one-word-short first transfer */
    REG32(EDMA_EMCR) = 0xFFFFFFFFu; REG32(EDMA_EMCRH) = 0xFFFFFFFFu;   /* the board set-up's clears */
    REG32(EDMA_QEMCR) = 0xFFFFFFFFu; REG32(EDMA_CCERRCLR) = 0xFFFFFFFFu;
    REG32(EDMA_DMAQNUM0) = 0;                                          /* channels 0-7 on queue 0 */
    param_set(64, rx_opt, MCASP0_DATA, (u32)rx_c, WORDS, 4u << 16, 0x4000 + 0x20 * 65);
    param_set(0, rx_opt, MCASP0_DATA, (u32)rx_c, first, 4u << 16, 0x4000 + 0x20 * 65);
    param_set(65, rx_opt, MCASP0_DATA, (u32)rx_d, WORDS, 4u << 16, 0x4000 + 0x20 * 64);
    param_set(66, tx_opt, (u32)tx_a, MCASP0_DATA, WORDS, 4, 0x4000 + 0x20 * 67);
    param_set(1, tx_opt, (u32)tx_a, MCASP0_DATA, first, 4, 0x4000 + 0x20 * 67);
    param_set(67, tx_opt, (u32)tx_b, MCASP0_DATA, WORDS, 4, 0x4000 + 0x20 * 66);
    REG32(EDMA_DRAE1) |= 3;                   /* channels 0 and 1 belong to shadow region 1 */
    REG32(EDMA_S1_ICR) = 3;
    REG32(EDMA_S1_IESR) = 2;                  /* interrupt on transmit completion only */
}

static void mcasp_setup(void)                          /* the original values (SPRUH79 23.1 fields) */
{
    REG32(RGBLCTL) = 0; REG32(XGBLCTL) = 0;
    REG32(RMASK) = 0xFFFFFFFFu; REG32(RFMT) = 0x000180F0;             /* 1-bit delay, MSB first, 32-bit slots, DMA port */
    REG32(XMASK) = 0xFFFFFFFFu; REG32(XFMT) = 0x000180F0;
    REG32(AFSRCTL) = 0x110; REG32(AFSXCTL) = 0x110;                   /* 2-slot TDM, word-wide frame sync, external, rising edge */
    REG32(ACLKRCTL) = 0; REG32(AHCLKRCTL) = 0;                        /* receive clocks from the transmit side (ASYNC 0) */
    REG32(RCLKCHK) = 0x00FF0005;
    REG32(ACLKXCTL) = 0; REG32(AHCLKXCTL) = 0;
    REG32(ACLKXCTL) = 0x80;                                           /* CLKXP: shift out on the falling edge; external clock */
    REG32(XCLKCHK) = 0x00FF0005;
    REG32(RTDM) = 3; REG32(XTDM) = 3;                                 /* slots 0 and 1 */
    REG32(SRCTL(14)) = 0; REG32(SRCTL(14)) = 2;                       /* AXR14 receives */
    REG32(SRCTL(13)) = 0; REG32(SRCTL(13)) = 1;                       /* AXR13 transmits */
    REG32(PFUNC) = 0; REG32(PDIR) = 0x2000;                           /* AXR13 is the only output pin */
    REG32(XINTCTL) = 0x0F; REG32(RINTCTL) = 0x0F;                     /* error flags (no interrupt is taken here) */
}

static int mcasp_start(void)                           /* the original start order; SPRUH79 23.0.21.1.2 steps 6-10 */
{
    REG32(GBLCTL) = 0x002; if (wait_gblctl(0x002) < 0) return -1;     /* RHCLKRST */
    REG32(GBLCTL) = 0x202; if (wait_gblctl(0x202) < 0) return -1;     /* XHCLKRST */
    REG32(EDMA_S1_SECR) = 1; REG32(EDMA_EMCR) = 1; REG32(EDMA_S1_EESR) = 1;   /* event 0 (receive) armed */
    REG32(EDMA_S1_SECR) = 2; REG32(EDMA_EMCR) = 3; REG32(EDMA_S1_EESR) = 3;   /* event 1 (transmit) armed */
    REG32(RSTAT) = 0xFFFF; REG32(GBLCTL) = 0x206; if (wait_gblctl(0x206) < 0) return -1;     /* RSRCLR */
    REG32(XSTAT) = 0xFFFF; REG32(GBLCTL) = 0x606; if (wait_gblctl(0x606) < 0) return -1;     /* XSRCLR */
    REG32(GBLCTL) = 0x60E; if (wait_gblctl(0x60E) < 0) return -1;     /* RSMRST */
    REG32(GBLCTL) = 0x61E; if (wait_gblctl(0x61E) < 0) return -1;     /* RFRST */
    REG32(GBLCTL) = 0xE1E; if (wait_gblctl(0xE1E) < 0) return -1;     /* XSMRST */
    REG32(GBLCTL) = 0x1E1E; if (wait_gblctl(0x1E1E) < 0) return -1;   /* XFRST */
    return 0;
}

static void silence(void)
{
    u32 i;
    for (i = 0; i < WORDS; i++) { tx_a[i] = 0; tx_b[i] = 0; rx_c[i] = 0; rx_d[i] = 0; }
    cache_wbinv(tx_a, sizeof tx_a); cache_wbinv(tx_b, sizeof tx_b);
    cache_wbinv(rx_c, sizeof rx_c); cache_wbinv(rx_d, sizeof rx_d);
}

int audio_init(void)
{
    audio_status = -1;
    gpio_setup();
    if (psc_enable(PSC0, PSC0_EDMA_CC) < 0 || psc_enable(PSC0, PSC0_EDMA_TC0) < 0) return audio_status = -2;
    pinmux_set(4, 2, 2); pinmux_set(4, 3, 2);          /* I2C0 SCL, SDA */
    pinmux_set(0, 1, 1); pinmux_set(0, 3, 1); pinmux_set(0, 5, 1); pinmux_set(0, 6, 1);   /* ACLKX0, AFSX0, AHCLKX0, AMUTE0 */
    pinmux_set(1, 1, 1); pinmux_set(1, 2, 1);          /* AXR14, AXR13 */
    if (psc_enable(PSC1, PSC1_MCASP0) < 0) return audio_status = -3;
    silence();
    edma_setup();
    audio_codec_errors = codec_init();
    if (audio_codec_errors) return audio_status = -4;
    irq_enable(1u << IRQ_AUDIO);              /* taken once the kernel sets GIE */
    mcasp_setup();
    if (mcasp_start() < 0) return audio_status = -5;
    codec_unmute();
    gpio_set(4, 14); delay_us(2000);                   /* the analogue power sequence: 2 and 5 ms apart */
    gpio_set(4, 15); delay_us(5000);
    gpio_set(4, 12);
    return audio_status = 0;
}

void audio_set_callback(audio_callback_t fn, void *context)
{
    u32 gie = irq_global_disable();
    callback = 0;
    callback_context = context;
    callback = fn;
    audio_cycles_peak = 0;
    if (gie) irq_global_enable();
}

void audio_mute(void)
{
    audio_set_callback(0, 0);
    delay_us(3000);                           /* two blocks of silence through the ring */
    codec_mute();
}

void audio_isr(void)
{
    u32 start = tsc_read(), pending = REG32(EDMA_S1_IPR), i, cycles;
    REG32(EDMA_S1_ICR) = pending & 3;
    if (pending & 1) rx_done++;
    if (pending & 2) {
        u32 *tx = tx_done & 1 ? tx_b : tx_a;  /* completion n finished buffer A, B, A, ...: refill it */
        audio_callback_t fn = callback;
        tx_done++;
        if (rx_done) {
            const u32 *rx = (rx_done - 1) & 1 ? rx_d : rx_c;
            cache_inv((void *)rx, WORDS * 4);
            for (i = 0; i < WORDS; i++) in_block[i] = (float)(int)rx[i] * (1.0f / 2147483648.0f);
        } else {
            for (i = 0; i < WORDS; i++) in_block[i] = 0.0f;
        }
        if (fn) {                             /* the watchdog may interrupt the app's callback (watchdog.c) */
            u32 saved[3];
            audio_callback_start = tsc_read();
            audio_in_callback = 1;
            irq_nest_begin(saved, 1u << IRQ_WATCHDOG);
            fn(callback_context, in_block, out_block, FRAMES);
            irq_nest_end(saved);
            audio_in_callback = 0;
        } else for (i = 0; i < WORDS; i++) out_block[i] = 0.0f;
        for (i = 0; i < WORDS; i++) {
            float v = out_block[i] * 2147483648.0f;
            tx[i] = v >= 2147483647.0f ? 0x7FFFFFFFu : v <= -2147483648.0f ? 0x80000000u : (u32)(int)v;
        }
        cache_wbinv(tx, WORDS * 4);
        audio_blocks++;
    }
    REG32(EDMA_S1_IEVAL) = 1;                 /* raise the interrupt again if another completion is pending */
    cycles = tsc_read() - start;
    audio_cycles_last = cycles;
    if (cycles > audio_cycles_peak) audio_cycles_peak = cycles;
    if (cycles > AUDIO_BUDGET_CYCLES) audio_overruns++;
}

void audio_irq_rearm(void)                    /* an app failed inside the callback: keep the interrupt alive */
{
    if (audio_status == 0) REG32(EDMA_S1_IEVAL) = 1;
}

void audio_silence_now(void)                  /* with interrupts off the ring replays its two buffers: clear them */
{
    u32 i;
    callback = 0;
    audio_in_callback = 0;
    for (i = 0; i < WORDS; i++) { tx_a[i] = 0; tx_b[i] = 0; }
    cache_wbinv(tx_a, sizeof tx_a);
    cache_wbinv(tx_b, sizeof tx_b);
}
