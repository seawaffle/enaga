/* Enaga DSP: envelopes (include/enaga/dsp.h). Built by cl6x when installed, else GCC.
 *
 * The same values as enaga_envelope_step, without branches in the loops: attack, decay and sustain share one
 * loop that selects between the attack and decay updates (decay and sustain are the same update; only the
 * stage's name changes), and release clamps at zero, after which it stays there as an idle envelope would. */
#include <enaga/dsp.h>

void enaga_envelope_render(enaga_envelope *e, const enaga_envelope_shape *s, float *restrict out, int n) {
    float l = e->level;
    int i;
    switch (e->stage) {
    case ENAGA_ENV_ATTACK:
    case ENAGA_ENV_DECAY:
    case ENAGA_ENV_SUSTAIN: {
        const float a = s->attack, d = s->decay, sustain = s->sustain;
        int attack = e->stage == ENAGA_ENV_ATTACK, decay = e->stage == ENAGA_ENV_DECAY;
        for (i = 0; i < n; i++) {
            float la = l + a * (ENAGA_ENV_ATTACK_TARGET - l), ld = l + d * (sustain - l);
            int peak = attack & (la >= 1.0f);
            int settled = (!attack) & decay & (ld - sustain < 1e-4f);
            l = attack ? (peak ? 1.0f : la) : ld;
            decay = (decay | peak) & (!settled);
            attack &= (!peak);
            out[i] = l;
        }
        e->stage = attack ? ENAGA_ENV_ATTACK : decay ? ENAGA_ENV_DECAY : ENAGA_ENV_SUSTAIN;
        break;
    }
    case ENAGA_ENV_RELEASE: {
        const float r = s->release;
        for (i = 0; i < n; i++) {
            float lr = l + r * (-0.001f - l);
            l = lr <= 0.0f ? 0.0f : lr;
            out[i] = l;
        }
        if (l <= 0.0f) e->stage = ENAGA_ENV_IDLE;
        break;
    }
    default:
        for (i = 0; i < n; i++) out[i] = l;
        break;
    }
    e->level = l;
}

void enaga_smoother_render(enaga_smoother *s, float *out, int n) {
    float v = s->value, t = s->target, a = s->a;
    int i;
    for (i = 0; i < n; i++) {
        float w = v + a * (t - v);
        out[i] = v = w == v ? t : w;
    }
    s->value = v;
}
