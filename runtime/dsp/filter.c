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
