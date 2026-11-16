#include "kernel.h"

/* Panel LEDs by logical number (enaga_led, abi.h) over the panel MCU's `C1 a b c v` (hal led_set). Addresses are
 * written as n = 48a + 8b + c; the tables are the original firmware's (docs/hardware.md "Panel MCU", where each
 * one's confirmation on a unit is marked). Pads have red, green and blue channels; STEP and the ten button LEDs
 * green at an even c and red at c + 1; VOL L, VOL R and the microphone one channel; the VALUE encoder red, green
 * and blue. The row-0 LEDs (VOL L, VOL R, microphone, VALUE) are written on all three a-planes with one value, as
 * the original firmware does. The 29 addresses it never drives are never written.
 *
 * Levels run 0 (off) to ENAGA_LED_MAX; from 11 a neighbouring LED lights too, so higher values are clamped. The
 * level last sent to each address is kept, and only changes go out (5 bytes at 465,000 baud, about 108 us).
 * Main context only: from an interrupt the bytes could split a message the main context is sending, so calls
 * from interrupt context are ignored. */

#define NONE 0xFFu

static const u8 pad_rgb[15][3] = {      /* by MCU pad channel: red (v1), green (v2), blue (v3) */
    {109, 108, 110}, {106, 105, 107}, {13, 12, 14}, {10, 9, 11}, {61, 60, 62},
    {58, 57, 59}, {116, 115, 117}, {23, 22, 8}, {68, 67, 69}, {71, 70, 56},
    {20, 19, 21}, {119, 118, 104}, {17, 16, 18}, {113, 112, 114}, {65, 64, 66},
};
static const u8 step_green[16] = { 72, 74, 26, 28, 30, 32, 82, 84, 140, 24, 142, 76, 78, 80, 34, 36 };

static u8 led_level[144];                   /* last level sent per address; NONE = unknown */

static u8 key_green(unsigned key)       /* a key's two-channel LED: its green address, or NONE */
{
    if (key >= ENAGA_KEY_STEP1 && key < ENAGA_KEY_STEP1 + 16) return step_green[key - ENAGA_KEY_STEP1];
    switch (key) {
    case ENAGA_KEY_CLR: return 38;
    case ENAGA_KEY_F1: return 40;
    case ENAGA_KEY_F4: return 42;
    case ENAGA_KEY_F7: return 44;
    case ENAGA_KEY_F6: return 46;
    case ENAGA_KEY_OK: return 86;
    case ENAGA_KEY_F2: return 88;
    case ENAGA_KEY_F3: return 90;
    case ENAGA_KEY_F8: return 92;
    case ENAGA_KEY_F5: return 94;
    default: return NONE;
    }
}

static void send(unsigned n, u8 v)
{
    if (led_level[n] == v) return;
    led_level[n] = v;
    led_set((u8)(n / 48), (u8)(n / 8 % 6), (u8)(n % 8), v);
}

static void send_row0(unsigned c, u8 v)  /* one row-0 LED: all three a-planes */
{
    send(c, v);
    send(48 + c, v);
    send(96 + c, v);
}

static u8 clamp(unsigned v) { return v > ENAGA_LED_MAX ? ENAGA_LED_MAX : (u8)v; }

void leds_set(unsigned led, unsigned red, unsigned green, unsigned blue)
{
    u8 r = clamp(red), g = clamp(green), b = clamp(blue), n;
    if (irq_active) return;
    if (led < ENAGA_LED_PAD + 15) {
        send(pad_rgb[led][0], r);
        send(pad_rgb[led][1], g);
        send(pad_rgb[led][2], b);
    } else if (led >= ENAGA_LED_VOL_L && led <= ENAGA_LED_MIC) {
        u8 v = r > g ? r : g;
        send_row0(led - ENAGA_LED_VOL_L, v > b ? v : b);   /* one channel: the brightest of the three */
    } else if (led == ENAGA_LED_VALUE) {
        send_row0(3, b);
        send_row0(4, g);
        send_row0(5, r);
    } else if (led >= ENAGA_LED_KEY && led < ENAGA_LED_KEY + ENAGA_KEY_COUNT
               && (n = key_green(led - ENAGA_LED_KEY)) != NONE) {
        send(n, g);
        send(n + 1u, r);
    }
}

void leds_forget(void)
{
    unsigned n;
    for (n = 0; n < sizeof led_level; n++) led_level[n] = NONE;
}

void leds_off(void)
{
    unsigned led;
    for (led = 0; led < ENAGA_LED_COUNT; led++) leds_set(led, 0, 0, 0);
}
