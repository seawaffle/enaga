#include "hal.h"

/* The TLV320AIC3106 as the original firmware configures it (docs/hardware.md "Audio"), in the order written;
 * decoded against TI SLAS509G. The codec is the clock master at 48 kHz from a 24.576 MHz MCLK (P = 2, J = 8), I2S
 * 32-bit words, both DACs on the direct DAC_L3/R3 paths to LEFT_LOP/M and RIGHT_LOP/M (register 41 = 0x50), line
 * inputs LINE1L/R fully differential into the two ADCs, the programmable high-pass filter on. GP4[10] (the codec's
 * RESET, unconfirmed) is released one millisecond before the first write, as the original firmware does; register 1 = 0x80 is the software
 * reset and gets a millisecond too. The DAC volume stays muted (reset state) until codec_unmute. */
static const u8 codec_setup[][2] = {
    { 0, 0x00 }, { 1, 0x80 },                                  /* page 0; software reset */
    { 9, 0x37 }, { 10, 0x00 },                                 /* I2S, 32-bit, continuous bit clock, soft re-sync; offset 0 */
    { 2, 0x00 }, { 3, 0xA2 }, { 4, 0x20 }, { 5, 0x00 }, { 6, 0x00 }, { 7, 0x0A }, { 8, 0xD0 },   /* fs(ref), PLL P 2 J 8 D 0, 48 kHz, master, clocks run when powered down */
    { 15, 0x00 }, { 16, 0x00 },                                /* ADC PGAs unmuted, 0 dB */
    { 0, 0x01 },                                               /* page 1: the ADC high-pass coefficients */
    { 65, 0x7F }, { 66, 0xD5 }, { 67, 0x80 }, { 68, 0x2B }, { 69, 0x7F }, { 70, 0xAA },
    { 71, 0x7F }, { 72, 0xD5 }, { 73, 0x80 }, { 74, 0x2B }, { 75, 0x7F }, { 76, 0xAA },
    { 0, 0x00 }, { 107, 0xC0 },                                /* page 0; programmable coefficients for both ADCs */
    { 17, 0xFF }, { 18, 0xFF },                                /* MIC3L/R not connected */
    { 21, 0xF8 }, { 24, 0xF8 },                                /* cross line inputs not connected, differential */
    { 19, 0x84 }, { 22, 0x84 },                                /* LINE1L -> left ADC, LINE1R -> right ADC, 0 dB, powered */
    { 37, 0xC0 }, { 41, 0x50 }, { 42, 0x9C },                  /* DACs powered; DAC_L3/R3 to the line outputs; 800 ms driver delay */
    { 86, 0x0B }, { 93, 0x0B },                                /* LEFT_LOP/M and RIGHT_LOP/M 0 dB, unmuted, powered */
    { 12, 0x50 },                                              /* ADC high-pass on */
};

int codec_init(void)
{
    unsigned i;
    int failed = 0;
    gpio_set(CODEC_RESET_BANK, CODEC_RESET_PIN);     /* one millisecond before the codec's software reset */
    delay_us(1000);
    for (i = 0; i < sizeof codec_setup / sizeof codec_setup[0]; i++) {
        if (i2c0_write(codec_setup[i][0], codec_setup[i][1]) < 0) failed++;
        if (codec_setup[i][0] == 1) delay_us(1000);    /* the software reset: one millisecond */
    }
    return failed;
}

void codec_unmute(void)                              /* GP2[9] low, then DAC volume 43 = 44 = 0 dB */
{
    gpio_clr(OUT_MUTE_BANK, OUT_MUTE_PIN);
    i2c0_write(43, 0x00);
    i2c0_write(44, 0x00);
}

void codec_mute(void)                                /* the power-off order: GP2[9] high, DAC volume 43 = 44 muted */
{
    gpio_set(OUT_MUTE_BANK, OUT_MUTE_PIN);
    i2c0_write(43, 0x80);
    i2c0_write(44, 0x80);
}
