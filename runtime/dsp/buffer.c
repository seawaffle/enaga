/* Enaga DSP: buffer arithmetic and noise (include/enaga/dsp.h). Built by cl6x when installed, else GCC. */
#include <enaga/dsp.h>

void enaga_clear(float *buf, int n) {
    int i;
    for (i = 0; i < n; i++) buf[i] = 0.0f;
}

void enaga_add(float *restrict out, const float *restrict in, int n) {
    int i;
    for (i = 0; i < n; i++) out[i] += in[i];
}

void enaga_mix(float *restrict out, const float *restrict in, int n, float gain) {
    int i;
    for (i = 0; i < n; i++) out[i] += gain * in[i];
}

void enaga_scale(float *buf, int n, float gain) {
    int i;
    for (i = 0; i < n; i++) buf[i] *= gain;
}

void enaga_multiply(float *restrict buf, const float *restrict gain, int n) {
    int i;
    for (i = 0; i < n; i++) buf[i] *= gain[i];
}

void enaga_soft_clip_buffer(float *buf, int n, float drive) {
    int i;
    for (i = 0; i < n; i++) buf[i] = enaga_soft_clip(drive * buf[i]);
}

void enaga_ramp_stereo_add(const float *restrict in, float *restrict left, float *restrict right, int n, float *gl,
                           float *gr, float step_l, float step_r) {
    float l = *gl, r = *gr;
    int i;
    for (i = 0; i < n; i++) {
        l += step_l;
        r += step_r;
        left[i] += in[i] * l;
        right[i] += in[i] * r;
    }
    *gl = l;
    *gr = r;
}

void enaga_noise_add(uint32_t *state, float *restrict out, int n, float level) {
    uint32_t x = *state;
    int i;
    for (i = 0; i < n; i++) out[i] += level * enaga_bipolar(enaga_xorshift(&x));
    *state = x;
}

void enaga_exp2_buffer(float *restrict buf, int n) {
    int i;
    for (i = 0; i < n; i++) buf[i] = enaga_exp2(buf[i]);
}

void enaga_reciprocal_buffer(float *restrict buf, int n) {
    int i;
    for (i = 0; i < n; i++) buf[i] = enaga_reciprocal(buf[i]);
}

void enaga_tan_pi_buffer(float *restrict buf, int n) {
    int i;
    for (i = 0; i < n; i++) buf[i] = enaga_tan_pi(buf[i]);
}

void enaga_mix_product(float *restrict out, const float *restrict a, const float *restrict b, int n, float gain) {
    int i;
    for (i = 0; i < n; i++) out[i] += gain * a[i] * b[i];
}
