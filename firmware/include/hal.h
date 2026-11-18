/* Enaga HAL: the board's drivers (docs/hardware.md). Polled; the kernel adds interrupts (kernel/irq.c) and
 * the audio engine (kernel/audio.c). No flash access. Every wait on a peripheral is bounded by the CPU
 * time-stamp counter (tsc_read, 372 MHz, PLL0 SYSCLK1); a failure before the display works ends in fault(),
 * which switches the unit off after a delay that encodes the failing stage, so a start with no screen still
 * reports where it stopped. */
#ifndef HAL_H
#define HAL_H
#include "hw.h"

int  psc_enable(u32 base, unsigned module);   /* 0 = enabled; -1 = the PSC never completed */
void pinmux_set(unsigned reg, unsigned nibble, unsigned value);
void pupd_clear_19_20(void);

void gpio_dir_output(unsigned bank, unsigned pin);
void gpio_set(unsigned bank, unsigned pin);
void gpio_clr(unsigned bank, unsigned pin);
u32  gpio_read_bank16(unsigned bank);

void tsc_enable(void);               /* crt0.s: start the CPU time-stamp counter */
u32  tsc_read(void);                 /* its low word: CPU cycles, 372,000 per ms */
#define TICKS_PER_MS 372000u
#define TIMER_PERIOD 240000u         /* TIMER64P0 period: 10 ms at 24 MHz; event 4 (TINT12) each period */
void timer_init(void);               /* TIMER64P0 running continuously, one event per period (the watchdog's tick) */
int  timer64_counts(void);           /* 1 = TIM12 advanced over 1 ms (its 24 MHz input clock is running) */
u32  timer_ticks(void);              /* the time base: tsc_read() */
u32  timer_ms(void);                 /* milliseconds since tsc_enable, maintained by timer_ms() calls */
void delay_us(u32 us);

void power_hold(void);
int  power_key_down(void);
int  power_on_batteries(void);
void power_off(void);
void fault(unsigned code);           /* wait code x 5 s, release the power hold, stop: the stage is read off the clock */

int  oled_init(void);                /* 0; -2 = SPI1's PSC enable failed; -3 = a command byte never completed */
int  oled_flush(const u8 *framebuffer);  /* 8192 bytes, 128 x 128 x 4 bpp, polled SPI; -1 = a byte never completed */
int  oled_dma_start(const u8 *framebuffer);   /* EDMA frame (EDMA must be powered); -1 = one is in flight */
int  oled_dma_poll(void);             /* 1 while a frame is in flight; completes it (chip select released) */
void oled_dma_finish(void);           /* wait for a frame in flight (interrupts may be off) */
extern u32 oled_dma_frames, oled_dma_timeouts;

void uart1_init(void);
void uart1_send(u8 byte);
int  uart1_poll(void);                /* parses what the MCU sent; returns 1 when a message completed */
void uart1_irq_start(void);           /* from now on the receive interrupt collects the bytes */
void uart1_isr(void);
extern u16 pad_value[16];             /* channels 0-14 pads, 15 battery; 14-bit values */
extern u8  mcu_info[6];               /* the A1 reply; bytes 3-5 are the MCU version */
extern u32 pad_messages, mcu_replies, uart1_dropped;   /* dropped: bytes the transmitter never accepted */
extern u32 uart1_rx_bytes;            /* bytes received, whether or not they made a message */
extern u32 uart1_overruns, uart1_ring_lost;   /* receive bytes lost: in the FIFO (LSR OE), ring full */
extern u8 uart1_rx_last;              /* the last of them */
extern int uart1_loopback_ok;         /* 1 = the receiver self-test at set-up passed (internal loopback) */
extern void (*pad_callback)(int channel, unsigned value);
void led_set(u8 a, u8 b, u8 c, u8 value);

void keys_init(void);
u64  keys_scan(void);                 /* 48-bit switch word, bit n = row n / 8, column n % 8; bit 27 is F4 */
u64  keys_reset(u32 ms);              /* debouncing starts from the keys held now; returns them */
u64  keys_poll(u32 ms);               /* the debounced switch word, sampled every 6 ms (keys.c); ms from timer_ms */

void encoders_init(void);
void encoders_poll(void);
extern int encoder_count[5];          /* A, B, C, D, VALUE; +1 per quadrature step */

/* Codec over I2C0 (the audio engine in kernel/audio.c drives McASP0 and EDMA). */
int  i2c0_write(u8 reg, u8 value);     /* one codec register write at 100 kHz; 0 = ok, -1 = bus never ready, -2 = NACK */
int  codec_init(void);                 /* the codec's 40 register writes, after releasing GP4[10]; returns failed writes */
void codec_unmute(void);               /* GP2[9] low with DAC volume 0 dB (43/44 = 0) */
void codec_mute(void);                 /* DAC volume muted (43/44 = 0x80) with GP2[9] high, the power-off order */

void cache_wbinv(void *address, u32 bytes);
void cache_inv(void *address, u32 bytes);
void cache_setup(void);               /* L1P, L1D and L2 all cache, DDR cacheable (cache.c) */
u32  cache_config(void);              /* L2, L1P, L1D modes in bits 24, 16, 8; bits 0-7 the PC bits of MAR 192-199 */
u32  cache_entry_config(void);        /* the same, as the bootloader left them (recorded by start.s) */
u32  crc32(const u8 *p, u32 n);

/* SD card (MMCSD0) and the app loader */
int  sd_init(void);                   /* 0 = a card is initialised and readable */
extern int sd_error;                  /* the last sd_init failure: its return code ... */
extern u32 sd_error_status;           /* ... and the MMCST0 bits the failing command saw */
int  sd_present(void);                /* card detect, GP4[0] low */
int  sd_read(u8 *buffer, u32 sector, u32 count);     /* 512-byte sectors; 0 = ok */
int  sd_write(const u8 *buffer, u32 sector, u32 count);
void sd_power_cycle(void);           /* GP4[1] high 250 ms, then low (a guess at the card's power switch) */
extern void (*sd_wait_hook)(void);   /* called while the driver waits on the card (the kernel feeds its watchdog) */
extern int sd_io_error;              /* the last sd_read / sd_write failure: its return code ... */
extern u32 sd_io_status;             /* ... MMCST0 bits, or for -6 the card status (still programming after 1 s) */
extern int sd_write_mode;            /* 1 (default): first 64 bytes in the FIFO before the command; 0: the command,
                                       * then data at DXRDY (on a unit its first CMD24 got no response, session 2) */
extern u32 sd_write_slowest;
extern u32 sd_reads, sd_writes;      /* sectors moved without error since start-up */
extern u32 sd_io_command;            /* the command index in flight at the last sd_read / sd_write failure */         /* the longest one-sector write so far, in time-stamp ticks */
/* Graphics: Enaga and apps draw on a row-major canvas, one byte (0-15) per pixel; gfx_present converts it
 * to the panel's 4-bpp order (gfx.c) and sends it. */
#define CANVAS_W 128
#define CANVAS_H 128
extern u8 canvas[CANVAS_W * CANVAS_H];
extern u8 framebuffer[8192];
int  gfx_present(void);               /* canvas -> framebuffer -> oled_flush, polled; -1 = a byte never completed */
int  gfx_present_dma(void);           /* canvas -> framebuffer -> EDMA; 1 = skipped, the previous frame is in flight */
void gfx_clear(u8 grey);
void gfx_pixel(int x, int y, u8 grey);
void gfx_scale(int s);                /* 1 (default) or 2: font pixels as s x s blocks, advance 6 s */
void gfx_text(int x, int y, const char *s, u8 grey);
void gfx_hex(int x, int y, u32 value, int digits, u8 grey);
void gfx_dec(int x, int y, int value, u8 grey);

#endif
