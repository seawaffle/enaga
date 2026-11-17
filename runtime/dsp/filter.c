/* Enaga DSP: filters (include/enaga/dsp.h). Built by cl6x when installed, else GCC. */
#include <enaga/dsp.h>

void enaga_svf_process(enaga_svf *s, const enaga_svf_coefficients *c, int output, const float *in, float *out,
                       int n) {
    /* Coefficients and state in locals, so the loops keep them in registers. */
    const enaga_svf_coefficients k = *c;
    enaga_svf t = *s;
    float low, band, high;
    int i;
    switch (output) {
    case ENAGA_SVF_LOW:
        for (i = 0; i < n; i++) { enaga_svf_step(&t, &k, in[i], &low, &band, &high); out[i] = low; }
        break;
    case ENAGA_SVF_BAND:
        for (i = 0; i < n; i++) { enaga_svf_step(&t, &k, in[i], &low, &band, &high); out[i] = band; }
        break;
    default:
        for (i = 0; i < n; i++) { enaga_svf_step(&t, &k, in[i], &low, &band, &high); out[i] = high; }
        break;
    }
    *s = t;
}

void enaga_svf_cascade(enaga_svf *s1, const enaga_svf_coefficients *c1, enaga_svf *s2,
                       const enaga_svf_coefficients *c2, int output, const float *in, float *out, int n) {
    const enaga_svf_coefficients k1 = *c1, k2 = *c2;
    enaga_svf t1 = *s1, t2 = *s2;
    float o[3], p[3];
    int i;
    switch (output) {
    case ENAGA_SVF_LOW:
        for (i = 0; i < n; i++) {
            enaga_svf_step(&t1, &k1, in[i], &o[0], &o[1], &o[2]);
            enaga_svf_step(&t2, &k2, o[0], &p[0], &p[1], &p[2]);
            out[i] = p[0];
        }
        break;
    case ENAGA_SVF_BAND:
        for (i = 0; i < n; i++) {
            enaga_svf_step(&t1, &k1, in[i], &o[0], &o[1], &o[2]);
            enaga_svf_step(&t2, &k2, o[1], &p[0], &p[1], &p[2]);
            out[i] = p[1];
        }
        break;
    default:
        for (i = 0; i < n; i++) {
            enaga_svf_step(&t1, &k1, in[i], &o[0], &o[1], &o[2]);
            enaga_svf_step(&t2, &k2, o[2], &p[0], &p[1], &p[2]);
            out[i] = p[2];
        }
        break;
    }
    *s1 = t1;
    *s2 = t2;
}

void enaga_one_pole_process(enaga_one_pole *p, float a, const float *in, float *out, int n) {
    float y = p->y;
    int i;
    for (i = 0; i < n; i++) out[i] = y += a * (in[i] - y);
    p->y = y;
}

/* ---- Biquads ---- */

void enaga_biquad_design(enaga_biquad_coefficients *c, int type, float frequency, float q, float gain_db,
                         float rate) {
    const float w = enaga_clamp(frequency / rate, 0.0001f, 0.49f);      /* turns per sample */
    /* 1 - cos w as 2 sin^2(w / 2): exact at low frequencies, where 1 - cos w would cancel. */
    const float h = enaga_sin_turns(0.5f * w), dip = 2.0f * h * h, cs = 1.0f - dip;
    const float alpha = enaga_sin_turns(w) / (2.0f * q);
    const float A = enaga_exp2(gain_db * 0.0830482024f);                 /* 10^(gain / 40) */
    const float root = 2.0f * enaga_exp2(gain_db * 0.0415241012f) * alpha; /* 2 sqrt(A) alpha */
    float b0, b1, b2, a0, a1, a2;
    switch (type) {
    case ENAGA_BIQUAD_LOW_PASS:
        b1 = dip; b0 = b2 = 0.5f * dip;
        a0 = 1.0f + alpha; a1 = -2.0f * cs; a2 = 1.0f - alpha;
        break;
    case ENAGA_BIQUAD_HIGH_PASS:
        b1 = dip - 2.0f; b0 = b2 = -0.5f * b1;
        a0 = 1.0f + alpha; a1 = -2.0f * cs; a2 = 1.0f - alpha;
        break;
    case ENAGA_BIQUAD_BAND_PASS:
        b0 = alpha; b1 = 0.0f; b2 = -alpha;
        a0 = 1.0f + alpha; a1 = -2.0f * cs; a2 = 1.0f - alpha;
        break;
    case ENAGA_BIQUAD_NOTCH:
        b0 = b2 = 1.0f; b1 = -2.0f * cs;
        a0 = 1.0f + alpha; a1 = -2.0f * cs; a2 = 1.0f - alpha;
        break;
    case ENAGA_BIQUAD_ALL_PASS:
        b0 = 1.0f - alpha; b1 = -2.0f * cs; b2 = 1.0f + alpha;
        a0 = 1.0f + alpha; a1 = -2.0f * cs; a2 = 1.0f - alpha;
        break;
    case ENAGA_BIQUAD_PEAK:
        b0 = 1.0f + alpha * A; b1 = -2.0f * cs; b2 = 1.0f - alpha * A;
        a0 = 1.0f + alpha / A; a1 = -2.0f * cs; a2 = 1.0f - alpha / A;
        break;
    case ENAGA_BIQUAD_LOW_SHELF:
        b0 = A * ((A + 1.0f) - (A - 1.0f) * cs + root);
        b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cs);
        b2 = A * ((A + 1.0f) - (A - 1.0f) * cs - root);
        a0 = (A + 1.0f) + (A - 1.0f) * cs + root;
        a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cs);
        a2 = (A + 1.0f) + (A - 1.0f) * cs - root;
        break;
    default:    /* ENAGA_BIQUAD_HIGH_SHELF */
        b0 = A * ((A + 1.0f) + (A - 1.0f) * cs + root);
        b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cs);
        b2 = A * ((A + 1.0f) + (A - 1.0f) * cs - root);
        a0 = (A + 1.0f) - (A - 1.0f) * cs + root;
        a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cs);
        a2 = (A + 1.0f) - (A - 1.0f) * cs - root;
        break;
    }
    const float inv = 1.0f / a0;
    c->b0 = b0 * inv;
    c->b1 = b1 * inv;
    c->b2 = b2 * inv;
    c->a1 = a1 * inv;
    c->a2 = a2 * inv;
}

void enaga_biquad_process(enaga_biquad *s, const enaga_biquad_coefficients *c, const float *in, float *out,
                          int n) {
    const enaga_biquad_coefficients k = *c;
    enaga_biquad t = *s;
    int i;
    for (i = 0; i < n; i++) out[i] = enaga_biquad_step(&t, &k, in[i]);
    *s = t;
}

void enaga_biquad_cascade(enaga_biquad *s1, const enaga_biquad_coefficients *c1, enaga_biquad *s2,
                          const enaga_biquad_coefficients *c2, const float *in, float *out, int n) {
    const enaga_biquad_coefficients k1 = *c1, k2 = *c2;
    enaga_biquad t1 = *s1, t2 = *s2;
    int i;
    for (i = 0; i < n; i++) out[i] = enaga_biquad_step(&t2, &k2, enaga_biquad_step(&t1, &k1, in[i]));
    *s1 = t1;
    *s2 = t2;
}

/* ---- FIR ----
 *
 * The history holds the last length - 1 inputs, then the chunk being filtered. Eight outputs at a time share each
 * tap's load and keep eight sums in flight, which hides the adder's latency; each sum still runs k = 0, 1, .... */

void enaga_fir_process(enaga_fir *f, const float *in, float *out, int n) {
    const float *restrict taps = f->taps;
    float *restrict h = f->history;
    const int length = f->length, keep = length - 1;
    while (n > 0) {
        const int m = n < ENAGA_FIR_CHUNK ? n : ENAGA_FIR_CHUNK;
        int i, k;
        for (i = 0; i < m; i++) h[keep + i] = in[i];
        for (i = 0; i + 8 <= m; i += 8) {
            const float *x = h + keep + i;
            float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f, s4 = 0.0f, s5 = 0.0f, s6 = 0.0f, s7 = 0.0f;
            for (k = 0; k < length; k++) {
                const float t = taps[k];
                s0 += t * x[-k];
                s1 += t * x[1 - k];
                s2 += t * x[2 - k];
                s3 += t * x[3 - k];
                s4 += t * x[4 - k];
                s5 += t * x[5 - k];
                s6 += t * x[6 - k];
                s7 += t * x[7 - k];
            }
            out[i] = s0;
            out[i + 1] = s1;
            out[i + 2] = s2;
            out[i + 3] = s3;
            out[i + 4] = s4;
            out[i + 5] = s5;
            out[i + 6] = s6;
            out[i + 7] = s7;
        }
        for (; i < m; i++) {
            const float *x = h + keep + i;
            float s = 0.0f;
            for (k = 0; k < length; k++) s += taps[k] * x[-k];
            out[i] = s;
        }
        for (i = 0; i < keep; i++) h[i] = h[m + i];
        in += m;
        out += m;
        n -= m;
    }
}
