/* Enaga DSP: delay lines, the stereo delay and the reverb (include/enaga/dsp.h). Built by cl6x when installed,
 * else GCC.
 *
 * Both effects work in chunks no longer than their shortest delay: every sample a chunk reads was written before
 * it, so each chunk reads its delay lines first, computes, and writes after. That gives the same samples as
 * reading and writing sample by sample, and keeps loads and stores of the same buffer out of the same loop, so
 * the loops pipeline. */
#include <enaga/dsp.h>

void enaga_delay_read(const enaga_delay_line *d, uint32_t delay, float *restrict out, int n) {
    const float *buf = d->buffer;
    const uint32_t mask = d->mask, from = d->write - delay;
    int i;
    for (i = 0; i < n; i++) out[i] = buf[(from + (uint32_t)i) & mask];
}

void enaga_delay_write_block(enaga_delay_line *d, const float *restrict in, int n) {
    float *buf = d->buffer;
    const uint32_t mask = d->mask, w = d->write;
    int i;
    for (i = 0; i < n; i++) buf[(w + 1 + (uint32_t)i) & mask] = in[i];
    d->write = (w + (uint32_t)n) & mask;
}

/* ---- Stereo delay ---- */

#define DELAY_CHUNK 16          /* enaga_stereo_delay_set keeps the delay at 16 samples or more */

static uint32_t stereo_delay_size(float max_seconds, float rate) {
    return enaga_delay_size((uint32_t)(max_seconds * rate) + 4);
}

uint32_t enaga_stereo_delay_memory(float max_seconds, float rate) { return 2 * stereo_delay_size(max_seconds, rate); }

void enaga_stereo_delay_init(enaga_stereo_delay *s, float *memory, float max_seconds, float rate) {
    const uint32_t size = stereo_delay_size(max_seconds, rate);
    enaga_delay_init(&s->left, memory, size);
    enaga_delay_init(&s->right, memory + size, size);
    s->damp_l.y = s->damp_r.y = 0.0f;
    s->rate = rate;
    s->current = s->target = 0.25f * rate;
    s->feedback = 0.0f;
    s->tone = 1.0f;
    s->ping_pong = 0;
}

void enaga_stereo_delay_set(enaga_stereo_delay *s, float seconds, float feedback, float tone, int ping_pong) {
    s->target = enaga_clamp(seconds * s->rate, 16.0f, (float)(enaga_delay_capacity(&s->left) - 2));
    s->feedback = enaga_clamp(feedback, 0.0f, 0.95f);
    tone = enaga_clamp(tone, 0.0f, 1.0f);
    s->tone = enaga_one_pole_coefficient(enaga_lerp(800.0f, 16000.0f, tone * tone), s->rate);
    s->ping_pong = ping_pong != 0;
}

static void stereo_delay_chunk(enaga_stereo_delay *s, const float *restrict in_l, const float *restrict in_r,
                               float *restrict out_l, float *restrict out_r, int m, float mix) {
    float dl[DELAY_CHUNK], dr[DELAY_CHUNK], wl[DELAY_CHUNK], wr[DELAY_CHUNK];
    const float *bl = s->left.buffer, *br = s->right.buffer;
    const uint32_t ml = s->left.mask, mr = s->right.mask, pl = s->left.write, pr = s->right.write;
    const float target = s->target, tone = s->tone, feedback = s->feedback;
    float current = s->current, yl = s->damp_l.y, yr = s->damp_r.y;
    int i;
    /* Reads: the gliding time, the interpolated taps (as enaga_delay_tap_linear at sample i), the tone filter. */
    for (i = 0; i < m; i++) {
        current += 0.0005f * (target - current);
        const int whole = (int)current;
        const float frac = current - (float)whole;
        const uint32_t at_l = pl + (uint32_t)i - (uint32_t)whole, at_r = pr + (uint32_t)i - (uint32_t)whole;
        const float al = bl[at_l & ml], ar = br[at_r & mr];
        const float l = al + (bl[(at_l - 1) & ml] - al) * frac, r = ar + (br[(at_r - 1) & mr] - ar) * frac;
        dl[i] = l;
        dr[i] = r;
        yl += tone * (l - yl);
        yr += tone * (r - yr);
        wl[i] = yl;                 /* the filtered echoes, made into the writes below */
        wr[i] = yr;
    }
    s->current = current;
    s->damp_l.y = yl;
    s->damp_r.y = yr;
    if (s->ping_pong) {
        for (i = 0; i < m; i++) {
            const float fl = wl[i];
            wl[i] = 0.5f * (in_l[i] + in_r[i]) + feedback * wr[i];
            wr[i] = feedback * fl;
        }
    } else {
        for (i = 0; i < m; i++) {
            wl[i] = in_l[i] + feedback * wl[i];
            wr[i] = in_r[i] + feedback * wr[i];
        }
    }
    enaga_delay_write_block(&s->left, wl, m);
    enaga_delay_write_block(&s->right, wr, m);
    for (i = 0; i < m; i++) {
        out_l[i] += mix * dl[i];
        out_r[i] += mix * dr[i];
    }
}

void enaga_stereo_delay_process(enaga_stereo_delay *s, const float *in_l, const float *in_r, float *out_l,
                                float *out_r, int n, float mix) {
    while (n > 0) {
        const int m = n < DELAY_CHUNK ? n : DELAY_CHUNK;
        stereo_delay_chunk(s, in_l, in_r, out_l, out_r, m, mix);
        in_l += m;
        in_r += m;
        out_l += m;
        out_r += m;
        n -= m;
    }
}

/* ---- Reverb ---- */

#define REVERB_CHUNK 64
static const uint32_t kLengths[ENAGA_REVERB_LINES] = {1123, 1361, 1597, 1823, 2083, 2357, 2689, 3037};   /* primes */
static const uint32_t kDiffuse[ENAGA_REVERB_DIFFUSERS] = {149, 211, 293, 397};

uint32_t enaga_reverb_memory(float rate) {
    const float scale = rate / 48000.0f;
    uint32_t total = 0;
    int i;
    for (i = 0; i < ENAGA_REVERB_LINES; i++) total += enaga_delay_size((uint32_t)((float)kLengths[i] * scale));
    for (i = 0; i < ENAGA_REVERB_DIFFUSERS; i++) total += enaga_delay_size((uint32_t)((float)kDiffuse[i] * scale));
    return total;
}

void enaga_reverb_init(enaga_reverb *r, float *memory, float rate) {
    const float scale = rate / 48000.0f;
    int i;
    r->rate = rate;
    for (i = 0; i < ENAGA_REVERB_LINES; i++) {
        const uint32_t size = enaga_delay_size(r->length[i] = (uint32_t)((float)kLengths[i] * scale));
        enaga_delay_init(&r->lines[i], memory, size);
        memory += size;
        r->damp[i].y = 0.0f;
    }
    for (i = 0; i < ENAGA_REVERB_DIFFUSERS; i++) {
        const uint32_t size = enaga_delay_size(r->diffuse_length[i] = (uint32_t)((float)kDiffuse[i] * scale));
        enaga_delay_init(&r->diffusers[i], memory, size);
        memory += size;
    }
    enaga_reverb_set(r, 2.0f, 0.5f);
}

void enaga_reverb_set(enaga_reverb *r, float decay_s, float damping) {
    int i;
    decay_s = enaga_clamp(decay_s, 0.2f, 30.0f);
    for (i = 0; i < ENAGA_REVERB_LINES; i++)       /* 10^(-3 length / (T60 rate)) = 2^(-9.966 length / (T60 rate)) */
        r->gain[i] = enaga_exp2(-9.96578428f * (float)r->length[i] / (decay_s * r->rate));
    r->damping = enaga_one_pole_coefficient(enaga_lerp(14000.0f, 1500.0f, enaga_clamp(damping, 0.0f, 1.0f)), r->rate);
}

static void reverb_chunk(enaga_reverb *r, const float *restrict in_l, const float *restrict in_r,
                         float *restrict out_l, float *restrict out_r, int m, float mix) {
    float x[REVERB_CHUNK], t[REVERB_CHUNK], v[REVERB_CHUNK];
    float s[ENAGA_REVERB_LINES][REVERB_CHUNK];
    int i, j;
    for (i = 0; i < m; i++) x[i] = 0.25f * (in_l[i] + in_r[i]);
    /* Schroeder all-passes, g = 0.6. */
    for (j = 0; j < ENAGA_REVERB_DIFFUSERS; j++) {
        enaga_delay_read(&r->diffusers[j], r->diffuse_length[j] - 1, t, m);
        for (i = 0; i < m; i++) {
            const float u = x[i] + 0.6f * t[i];
            v[i] = u;
            x[i] = t[i] - 0.6f * u;
        }
        enaga_delay_write_block(&r->diffusers[j], v, m);
    }
    /* The network: each line's output, damped and scaled; the eight one-poles run side by side. */
    for (j = 0; j < ENAGA_REVERB_LINES; j++) enaga_delay_read(&r->lines[j], r->length[j] - 1, s[j], m);
    {
        const float a = r->damping;
        float y0 = r->damp[0].y, y1 = r->damp[1].y, y2 = r->damp[2].y, y3 = r->damp[3].y;
        float y4 = r->damp[4].y, y5 = r->damp[5].y, y6 = r->damp[6].y, y7 = r->damp[7].y;
        const float g0 = r->gain[0], g1 = r->gain[1], g2 = r->gain[2], g3 = r->gain[3];
        const float g4 = r->gain[4], g5 = r->gain[5], g6 = r->gain[6], g7 = r->gain[7];
        for (i = 0; i < m; i++) {
            y0 += a * (s[0][i] - y0); s[0][i] = y0 * g0;
            y1 += a * (s[1][i] - y1); s[1][i] = y1 * g1;
            y2 += a * (s[2][i] - y2); s[2][i] = y2 * g2;
            y3 += a * (s[3][i] - y3); s[3][i] = y3 * g3;
            y4 += a * (s[4][i] - y4); s[4][i] = y4 * g4;
            y5 += a * (s[5][i] - y5); s[5][i] = y5 * g5;
            y6 += a * (s[6][i] - y6); s[6][i] = y6 * g6;
            y7 += a * (s[7][i] - y7); s[7][i] = y7 * g7;
        }
        r->damp[0].y = y0; r->damp[1].y = y1; r->damp[2].y = y2; r->damp[3].y = y3;
        r->damp[4].y = y4; r->damp[5].y = y5; r->damp[6].y = y6; r->damp[7].y = y7;
    }
    /* The 8-point Hadamard transform scaled by 1 / sqrt(8), the outputs, and what goes back into the lines. */
    for (i = 0; i < m; i++) {
        float a0 = s[0][i] + s[1][i], a1 = s[0][i] - s[1][i], a2 = s[2][i] + s[3][i], a3 = s[2][i] - s[3][i];
        float a4 = s[4][i] + s[5][i], a5 = s[4][i] - s[5][i], a6 = s[6][i] + s[7][i], a7 = s[6][i] - s[7][i];
        float b0 = a0 + a2, b2 = a0 - a2, b1 = a1 + a3, b3 = a1 - a3;
        float b4 = a4 + a6, b6 = a4 - a6, b5 = a5 + a7, b7 = a5 - a7;
        const float h0 = (b0 + b4) * 0.35355339f, h4 = (b0 - b4) * 0.35355339f;
        const float h1 = (b1 + b5) * 0.35355339f, h5 = (b1 - b5) * 0.35355339f;
        const float h2 = (b2 + b6) * 0.35355339f, h6 = (b2 - b6) * 0.35355339f;
        const float h3 = (b3 + b7) * 0.35355339f, h7 = (b3 - b7) * 0.35355339f;
        out_l[i] += mix * (h0 + h2 + h4 + h6);
        out_r[i] += mix * (h1 + h3 + h5 + h7);
        s[0][i] = h0 + x[i]; s[1][i] = h1 + x[i]; s[2][i] = h2 + x[i]; s[3][i] = h3 + x[i];
        s[4][i] = h4 + x[i]; s[5][i] = h5 + x[i]; s[6][i] = h6 + x[i]; s[7][i] = h7 + x[i];
    }
    for (j = 0; j < ENAGA_REVERB_LINES; j++) enaga_delay_write_block(&r->lines[j], s[j], m);
}

void enaga_reverb_process(enaga_reverb *r, const float *in_l, const float *in_r, float *out_l, float *out_r, int n,
                          float mix) {
    /* Chunks no longer than the shortest delay (the first diffuser's, 149 samples at 48 kHz). */
    int chunk = REVERB_CHUNK, j;
    for (j = 0; j < ENAGA_REVERB_DIFFUSERS; j++)
        if ((int)r->diffuse_length[j] < chunk) chunk = (int)r->diffuse_length[j];
    if (chunk < 1) chunk = 1;
    while (n > 0) {
        const int m = n < chunk ? n : chunk;
        reverb_chunk(r, in_l, in_r, out_l, out_r, m, mix);
        in_l += m;
        in_r += m;
        out_l += m;
        out_r += m;
        n -= m;
    }
}
