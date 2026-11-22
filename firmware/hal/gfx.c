#include "hal.h"

/* 128 x 128, 4 bits per pixel, 0 is black. The panel's byte order (docs/hardware.md "Display", confirmed with a
 * test pattern on the unit): the frame is 64 blocks of 128 bytes, block y >> 1 holding the row pair (2k, 2k+1);
 * within a block the first 64 bytes are the even columns and the next 64 the odd columns, one byte per column,
 * the even row of the pair in the LOW nibble and the odd row in the HIGH nibble. A row-major layout (64 bytes
 * per row, two horizontal neighbours per byte) scrambles every glyph.
 * Enaga draws on `canvas` instead and converts on present, so neither apps nor Enaga see the panel order. */
u8 framebuffer[8192];
u8 canvas[CANVAS_W * CANVAS_H];          /* Enaga: row-major, one byte (grey 0-15) per pixel */

void gfx_clear(u8 grey)
{
    u32 i;
    for (i = 0; i < sizeof canvas; i++) canvas[i] = (u8)(grey & 0xF);
}

void gfx_pixel(int x, int y, u8 grey)
{
    if ((unsigned)x >= CANVAS_W || (unsigned)y >= CANVAS_H) return;
    canvas[y * CANVAS_W + x] = (u8)(grey & 0xF);
}

static void convert(void)                 /* the canvas in the panel's order (above) */
{
    int y, x;
    for (y = 0; y < CANVAS_H; y += 2) {
        const u8 *even = &canvas[y * CANVAS_W], *odd = even + CANVAS_W;
        u8 *block = &framebuffer[(y >> 1) << 7];
        for (x = 0; x < CANVAS_W; x++)
            block[((x & 1) << 6) | (x >> 1)] = (u8)((even[x] & 0x0F) | (odd[x] << 4));
    }
}

int gfx_present(void)
{
    oled_dma_finish();
    convert();
    return oled_flush(framebuffer);
}

int gfx_present_dma(void)                 /* framebuffer is only rewritten while no frame is in flight */
{
    if (oled_dma_poll()) return 1;
    convert();
    return oled_dma_start(framebuffer) < 0 ? 1 : 0;
}

/* 5 x 7 font, one byte per column, bit 0 the top row. Written for this project. */
#define G(a, b, c, d, e) { a, b, c, d, e }
static const u8 font[][5] = {
    G(0x00, 0x00, 0x00, 0x00, 0x00),   /* space */
    G(0x00, 0x00, 0x5F, 0x00, 0x00),   /* ! */
    G(0x14, 0x7F, 0x14, 0x7F, 0x14),   /* # */
    G(0x08, 0x08, 0x3E, 0x08, 0x08),   /* + */
    G(0x08, 0x08, 0x08, 0x08, 0x08),   /* - */
    G(0x00, 0x60, 0x60, 0x00, 0x00),   /* . */
    G(0x20, 0x10, 0x08, 0x04, 0x02),   /* / */
    G(0x3E, 0x51, 0x49, 0x45, 0x3E),   /* 0 */
    G(0x00, 0x42, 0x7F, 0x40, 0x00),   /* 1 */
    G(0x42, 0x61, 0x51, 0x49, 0x46),   /* 2 */
    G(0x21, 0x41, 0x45, 0x4B, 0x31),   /* 3 */
    G(0x18, 0x14, 0x12, 0x7F, 0x10),   /* 4 */
    G(0x27, 0x45, 0x45, 0x45, 0x39),   /* 5 */
    G(0x3C, 0x4A, 0x49, 0x49, 0x30),   /* 6 */
    G(0x01, 0x71, 0x09, 0x05, 0x03),   /* 7 */
    G(0x36, 0x49, 0x49, 0x49, 0x36),   /* 8 */
    G(0x06, 0x49, 0x49, 0x29, 0x1E),   /* 9 */
    G(0x00, 0x36, 0x36, 0x00, 0x00),   /* : */
    G(0x14, 0x14, 0x14, 0x14, 0x14),   /* = */
    G(0x02, 0x01, 0x51, 0x09, 0x06),   /* ? */
    G(0x7E, 0x11, 0x11, 0x11, 0x7E),   /* A */
    G(0x7F, 0x49, 0x49, 0x49, 0x36),   /* B */
    G(0x3E, 0x41, 0x41, 0x41, 0x22),   /* C */
    G(0x7F, 0x41, 0x41, 0x22, 0x1C),   /* D */
    G(0x7F, 0x49, 0x49, 0x49, 0x41),   /* E */
    G(0x7F, 0x09, 0x09, 0x09, 0x01),   /* F */
    G(0x3E, 0x41, 0x49, 0x49, 0x7A),   /* G */
    G(0x7F, 0x08, 0x08, 0x08, 0x7F),   /* H */
    G(0x00, 0x41, 0x7F, 0x41, 0x00),   /* I */
    G(0x20, 0x40, 0x41, 0x3F, 0x01),   /* J */
    G(0x7F, 0x08, 0x14, 0x22, 0x41),   /* K */
    G(0x7F, 0x40, 0x40, 0x40, 0x40),   /* L */
    G(0x7F, 0x02, 0x0C, 0x02, 0x7F),   /* M */
    G(0x7F, 0x04, 0x08, 0x10, 0x7F),   /* N */
    G(0x3E, 0x41, 0x41, 0x41, 0x3E),   /* O */
    G(0x7F, 0x09, 0x09, 0x09, 0x06),   /* P */
    G(0x3E, 0x41, 0x51, 0x21, 0x5E),   /* Q */
    G(0x7F, 0x09, 0x19, 0x29, 0x46),   /* R */
    G(0x46, 0x49, 0x49, 0x49, 0x31),   /* S */
    G(0x01, 0x01, 0x7F, 0x01, 0x01),   /* T */
    G(0x3F, 0x40, 0x40, 0x40, 0x3F),   /* U */
    G(0x1F, 0x20, 0x40, 0x20, 0x1F),   /* V */
    G(0x3F, 0x40, 0x38, 0x40, 0x3F),   /* W */
    G(0x63, 0x14, 0x08, 0x14, 0x63),   /* X */
    G(0x07, 0x08, 0x70, 0x08, 0x07),   /* Y */
    G(0x61, 0x51, 0x49, 0x45, 0x43),   /* Z */
    G(0x23, 0x13, 0x08, 0x64, 0x62),   /* % */
    G(0x00, 0x41, 0x22, 0x14, 0x08),   /* > */
    G(0x08, 0x14, 0x22, 0x41, 0x00),   /* < */
    G(0x00, 0x1C, 0x22, 0x41, 0x00),   /* ( */
    G(0x00, 0x41, 0x22, 0x1C, 0x00),   /* ) */
    G(0x00, 0x50, 0x30, 0x00, 0x00),   /* , */
    G(0x40, 0x40, 0x40, 0x40, 0x40),   /* _ */
    G(0x00, 0x00, 0x07, 0x00, 0x00),   /* ' */
    G(0x14, 0x08, 0x3E, 0x08, 0x14),   /* * */
    G(0x00, 0x7F, 0x41, 0x41, 0x00),   /* [ */
    G(0x00, 0x41, 0x41, 0x7F, 0x00),   /* ] */
};
/* Lower case draws as upper case; anything else missing as '?'. */
static const char font_chars[] = " !#+-./0123456789:=?ABCDEFGHIJKLMNOPQRSTUVWXYZ%><(),_'*[]";

static const u8 *glyph(char c)
{
    const char *p;
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for (p = font_chars; *p; p++)
        if (*p == c) return font[p - font_chars];
    return font[19];                      /* '?' */
}

static int scale = 1;

void gfx_scale(int s)                     /* font pixels drawn as s x s blocks; the status page uses 2 */
{
    scale = s;
}

void gfx_text(int x, int y, const char *s, u8 grey)
{
    for (; *s; s++, x += 6 * scale) {
        const u8 *g = glyph(*s);
        int cx, cy, sx, sy;
        for (cx = 0; cx < 5; cx++)
            for (cy = 0; cy < 7; cy++)
                if (g[cx] & (1 << cy))
                    for (sx = 0; sx < scale; sx++)
                        for (sy = 0; sy < scale; sy++)
                            gfx_pixel(x + cx * scale + sx, y + cy * scale + sy, grey);
    }
}

void gfx_hex(int x, int y, u32 value, int digits, u8 grey)
{
    char s[9];
    int i;
    for (i = digits - 1; i >= 0; i--, value >>= 4) s[i] = "0123456789ABCDEF"[value & 0xF];
    s[digits] = 0;
    gfx_text(x, y, s, grey);
}

void gfx_dec(int x, int y, int value, u8 grey)
{
    char s[12];
    int i = 11;
    unsigned v = value < 0 ? (unsigned)(-value) : (unsigned)value;
    s[i] = 0;
    do {                                  /* no division: repeated subtraction keeps libgcc out of it */
        unsigned q = 0;
        while (v >= 10) { v -= 10; q++; }
        s[--i] = (char)('0' + v);
        v = q;
    } while (v);
    if (value < 0) s[--i] = '-';
    gfx_text(x, y, s + i, grey);
}
