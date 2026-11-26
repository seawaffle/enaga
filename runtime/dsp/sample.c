/* Enaga DSP: sample formats and rate conversion (include/enaga/dsp.h). Built by cl6x when installed, else GCC.
 * No division: rates go through a shift-and-subtract loop and the window's series through enaga_reciprocal. */
#include <enaga/dsp.h>

/* ---- Sample formats ---- */

static int32_t read_s24(const uint8_t *p) { return (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8; }
static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A float sample's bits, with infinities and NaN read as silence. */
static float finite_float(uint32_t bits) { return (bits & 0x7F800000u) == 0x7F800000u ? 0.0f : enaga_bits_float(bits); }

static int16_t saturate16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

static int16_t float_to_int16(float x) {
    x = enaga_clamp(x * 32768.0f, -32768.0f, 32767.0f);
    return (int16_t)enaga_floor_int(x + 0.5f);
}

void enaga_pcm_to_float(const void *src, int format, int stride, float *restrict out, int n) {
    const uint8_t *p = (const uint8_t *)src;
    int step = enaga_pcm_bytes(format) * stride, i;
    switch (format) {
    case ENAGA_PCM_U8:
        for (i = 0; i < n; i++) out[i] = (float)((int32_t)p[i * step] - 128) * (1.0f / 128.0f);
        break;
    case ENAGA_PCM_S16:
        for (i = 0; i < n; i++)
            out[i] = (float)(int16_t)(p[i * step] | p[i * step + 1] << 8) * (1.0f / 32768.0f);
        break;
    case ENAGA_PCM_S24:
        for (i = 0; i < n; i++) out[i] = (float)read_s24(p + i * step) * (1.0f / 8388608.0f);
        break;
    case ENAGA_PCM_S32:
        for (i = 0; i < n; i++) out[i] = (float)(int32_t)read_u32(p + i * step) * (1.0f / 2147483648.0f);
        break;
    case ENAGA_PCM_FLOAT:
        for (i = 0; i < n; i++) out[i] = finite_float(read_u32(p + i * step));
        break;
    default:
        for (i = 0; i < n; i++) out[i] = 0.0f;
    }
}

void enaga_pcm_to_int16(const void *src, int format, int stride, int16_t *restrict out, int n) {
    const uint8_t *p = (const uint8_t *)src;
    int step = enaga_pcm_bytes(format) * stride, i;
    switch (format) {
    case ENAGA_PCM_U8:
        for (i = 0; i < n; i++) out[i] = (int16_t)(((int32_t)p[i * step] - 128) * 256);
        break;
    case ENAGA_PCM_S16:
        for (i = 0; i < n; i++) out[i] = (int16_t)(p[i * step] | p[i * step + 1] << 8);
        break;
    case ENAGA_PCM_S24:
        for (i = 0; i < n; i++) out[i] = saturate16((read_s24(p + i * step) + 128) >> 8);
        break;
    case ENAGA_PCM_S32:
        for (i = 0; i < n; i++) out[i] = saturate16((((int32_t)read_u32(p + i * step) >> 15) + 1) >> 1);
        break;
    case ENAGA_PCM_FLOAT:
        for (i = 0; i < n; i++) out[i] = float_to_int16(finite_float(read_u32(p + i * step)));
        break;
    default:
        for (i = 0; i < n; i++) out[i] = 0;
    }
}

void enaga_int16_to_float(const int16_t *restrict in, float *restrict out, int n, float gain) {
    float g = gain * (1.0f / 32768.0f);
    int i;
    for (i = 0; i < n; i++) out[i] = (float)in[i] * g;
}

void enaga_float_to_int16(const float *restrict in, int16_t *restrict out, int n) {
    int i;
    for (i = 0; i < n; i++) out[i] = float_to_int16(in[i]);
}

/* ---- Rate conversion ---- */

#define TAPS ENAGA_RESAMPLE_TAPS
#define PHASES ENAGA_RESAMPLE_PHASES
#define HALF (TAPS / 2)
#define PHASE_BITS 6                     /* log2(PHASES) */
#define KAISER_BETA 8.6f                 /* about 86 dB down outside the pass band */

/* num / den in 32.32 fixed point, by shift and subtract (num, den < 2^30). */
static void ratio(uint32_t num, uint32_t den, uint32_t *whole, uint32_t *fraction) {
    uint32_t rem = 0, hi = 0, lo = 0;
    int b;
    for (b = 63; b >= 0; b--) {
        rem = rem << 1 | (b >= 32 ? num >> (b - 32) & 1u : 0u);
        hi = hi << 1 | lo >> 31;
        lo <<= 1;
        if (rem >= den) { rem -= den; lo |= 1u; }
    }
    *whole = hi;
    *fraction = lo;
}

/* I0(sqrt(y4) * 2), the modified Bessel function at z with y4 = z^2 / 4: its power series. */
static float bessel_i0(float y4) {
    float sum = 1.0f, term = 1.0f;
    int k;
    for (k = 1; k < 40; k++) {
        term *= y4 * enaga_reciprocal((float)(k * k));
        sum += term;
    }
    return sum;
}

void enaga_resample_table(float *table, uint32_t in_rate, uint32_t out_rate) {
    /* Cut-off as a fraction of the input's Nyquist: 0.95 of the lower rate's. */
    float cutoff = 0.95f * (out_rate < in_rate ? (float)out_rate * enaga_reciprocal((float)in_rate) : 1.0f);
    float inv_i0 = enaga_reciprocal(bessel_i0(KAISER_BETA * KAISER_BETA * 0.25f));
    int p, k;
    for (p = 0; p <= PHASES; p++) {
        float *row = table + p * TAPS, sum = 0.0f;
        for (k = 0; k < TAPS; k++) {
            float x = (float)(k - (HALF - 1)) - (float)p * (1.0f / PHASES);   /* the tap's distance, in inputs */
            float u = x * (1.0f / HALF), h;
            h = x == 0.0f ? cutoff : enaga_sin_turns(0.5f * cutoff * x) * enaga_reciprocal(ENAGA_PI * x);
            h *= u * u < 1.0f ? bessel_i0(KAISER_BETA * KAISER_BETA * 0.25f * (1.0f - u * u)) * inv_i0 : 0.0f;
            row[k] = h;
            sum += h;
        }
        sum = enaga_reciprocal(sum);                                   /* each phase passes DC at exactly 1 */
        for (k = 0; k < TAPS; k++) row[k] *= sum;
    }
}

void enaga_resampler_init(enaga_resampler *r, const float *table, uint32_t in_rate, uint32_t out_rate) {
    int i;
    r->table = table;
    ratio(in_rate, out_rate, &r->step_int, &r->step_frac);
    r->frac = 0;
    r->skip = HALF + 1;                  /* input 0 reaches the window's centre (tap HALF - 1) */
    r->write = 0;
    for (i = 0; i < 2 * TAPS; i++) r->window[i] = 0.0f;
}

static float resample_one(const enaga_resampler *r) {
    const float *w = r->window + r->write;                                  /* oldest first */
    const float *a = r->table + (r->frac >> (32 - PHASE_BITS)) * TAPS, *b = a + TAPS;
    float sub = (float)(r->frac & ((1u << (32 - PHASE_BITS)) - 1u)) * (1.0f / (float)(1u << (32 - PHASE_BITS)));
    float sa = 0.0f, sb = 0.0f;
    int k;
    for (k = 0; k < TAPS; k++) {
        sa += w[k] * a[k];
        sb += w[k] * b[k];
    }
    return sa + sub * (sb - sa);
}

int enaga_resample(enaga_resampler *r, const float *in, int n_in, float *out, int max_out, int *used) {
    int made = 0, taken = 0;
    for (;;) {
        while (r->skip == 0) {
            uint32_t frac;
            if (made == max_out) goto done;
            out[made++] = resample_one(r);
            frac = r->frac + r->step_frac;
            r->skip = r->step_int + (frac < r->frac);
            r->frac = frac;
        }
        if (taken == n_in) break;
        r->window[r->write] = r->window[r->write + TAPS] = in[taken++];
        r->write = (r->write + 1) & (TAPS - 1);
        r->skip--;
    }
done:
    *used = taken;
    return made;
}

/* ---- The sample player ---- */

#define FRAME ((int64_t)1 << 32)

void enaga_sample_player_start(enaga_sample_player *p, const enaga_sample *s, uint32_t start, uint32_t end, int reverse) {
    if (end == 0 || end > s->frames) end = s->frames;
    if (start > end) start = end;
    p->sample = s;
    p->start = start;
    p->end = end;
    p->loop_start = start;
    p->loop_end = end;
    p->loop = ENAGA_LOOP_OFF;
    p->direction = reverse ? -1 : 1;
    p->position = (int64_t)(reverse ? (end > start ? end - 1 : start) : start) << 32;
    p->active = end > start;
    p->fresh = 1;
    p->gain_l = p->gain_r = 0.0f;
}

/* Catmull-Rom through x0 and x1 at t in [0, 1). */
static float cubic(float xm1, float x0, float x1, float x2, float t) {
    float c1 = 0.5f * (x1 - xm1);
    float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

/* x mod m for x >= 0 and m > 0, by shift and subtract (no division). */
static int64_t modulo(int64_t x, int64_t m) {
    int64_t d = m;
    while (d <= x >> 1) d <<= 1;
    for (; d >= m; d >>= 1)
        if (x >= d) x -= d;
    return x;
}

/* Moves on by step, through the loop if there is one (however many times the step crosses it), and stops at the
 * start or end. A ping-pong loop is a forward one of twice the length, folded at its last frame. */
static void player_advance(enaga_sample_player *p, int64_t step) {
    int64_t before = p->position, at = p->direction > 0 ? before + step : before - step;
    int64_t ls = (int64_t)p->loop_start << 32, le = (int64_t)p->loop_end << 32;
    if (p->loop != ENAGA_LOOP_OFF && p->loop_end > p->loop_start) {
        int64_t last = le - FRAME, length = le - ls, u;
        if (p->loop == ENAGA_LOOP_FORWARD) {
            if (p->direction > 0 && before < le && at >= le) at = ls + modulo(at - ls, length);
            if (p->direction < 0 && before >= ls && at < ls) {
                u = modulo(ls - at, length);
                at = u ? le - u : ls;
            }
        } else if (p->direction > 0 ? before <= last && at > last : before >= ls && at < ls) {
            int64_t half = last - ls;
            u = half > 0 ? modulo(p->direction > 0 ? at - ls : ls - at, 2 * half) : 0;
            p->direction = u <= half ? 1 : -1;
            at = u <= half ? ls + u : ls + 2 * half - u;
        }
    }
    p->position = at;
    if (p->direction > 0 ? at >= (int64_t)p->end << 32 : at < (int64_t)p->start << 32) p->active = 0;
}

int enaga_sample_player_render(enaga_sample_player *p, float *left, float *right, int n, float rate) {
    const enaga_sample *s = p->sample;
    const float *d = s->data;
    int last = (int)s->frames - 1, stereo = s->channels == 2, i, made = 0;
    int ls = (int)p->loop_start, le = (int)p->loop_end, forward = p->loop == ENAGA_LOOP_FORWARD && le > ls;
    int64_t step;
    uint32_t whole;
    rate = enaga_clamp(rate, 0.0f, 1048576.0f);
    whole = (uint32_t)rate;
    step = (int64_t)whole << 32 | (uint32_t)((rate - (float)whole) * 2147483648.0f) << 1;
    for (i = 0; i < n && p->active; i++) {
        int x = (int)(p->position >> 32), a = x - 1, b = x, c = x + 1, e = x + 2;
        if (forward && x >= ls && x < le) {     /* in a forward loop the taps wrap, so its join is seamless */
            if (a < ls) a += le - ls;
            while (c >= le) c -= le - ls;
            while (e >= le) e -= le - ls;
        }
        a = a < 0 ? 0 : a > last ? last : a;
        b = b < 0 ? 0 : b > last ? last : b;
        c = c < 0 ? 0 : c > last ? last : c;
        e = e < 0 ? 0 : e > last ? last : e;
        float t = (float)((uint32_t)p->position >> 8) * (1.0f / 16777216.0f);
        if (stereo) {
            left[i] = cubic(d[2 * a], d[2 * b], d[2 * c], d[2 * e], t);
            right[i] = cubic(d[2 * a + 1], d[2 * b + 1], d[2 * c + 1], d[2 * e + 1], t);
        } else {
            left[i] = cubic(d[a], d[b], d[c], d[e], t);
        }
        player_advance(p, step);
        made++;
    }
    for (; i < n; i++) {
        left[i] = 0.0f;
        if (stereo) right[i] = 0.0f;
    }
    return made;
}

int enaga_sample_player_render_add(enaga_sample_player *p, float *left, float *right, int n, float rate, float gain_l,
                                   float gain_r) {
    float block_l[64], block_r[64], gl, gr, step_l, step_r, scale;
    int done = 0, made = 0, stereo = p->sample->channels == 2;
    if (p->fresh) {
        p->gain_l = gain_l;
        p->gain_r = gain_r;
        p->fresh = 0;
    }
    gl = p->gain_l;
    gr = p->gain_r;
    scale = n > 0 ? enaga_reciprocal((float)n) : 0.0f;
    step_l = (gain_l - gl) * scale;
    step_r = (gain_r - gr) * scale;
    while (done < n && p->active) {
        int count = n - done > 64 ? 64 : n - done, i;
        made += enaga_sample_player_render(p, block_l, block_r, count, rate);
        for (i = 0; i < count; i++) {
            gl += step_l;
            gr += step_r;
            left[done + i] += gl * block_l[i];
            right[done + i] += gr * (stereo ? block_r[i] : block_l[i]);
        }
        done += count;
    }
    p->gain_l = gain_l;
    p->gain_r = gain_r;
    return made;
}
