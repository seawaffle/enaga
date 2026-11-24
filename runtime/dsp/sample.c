/* Enaga DSP: sample formats and rate conversion (include/enaga/dsp.h). Built by cl6x when installed, else GCC.
 * No division: rates go through a shift-and-subtract loop and the window's series through enaga_reciprocal. */
#include <enaga/dsp.h>

/* ---- Sample formats ---- */

static int32_t read_s24(const uint8_t *p) { return (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8; }
static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

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
        for (i = 0; i < n; i++) out[i] = enaga_bits_float(read_u32(p + i * step));
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
        for (i = 0; i < n; i++) out[i] = float_to_int16(enaga_bits_float(read_u32(p + i * step)));
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
