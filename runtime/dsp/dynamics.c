/* Enaga DSP: the bitcrusher, the compressor and the limiter (include/enaga/dsp.h). Built by cl6x when installed,
 * else GCC.
 *
 * The compressor and limiter work in chunks on the stack: the level of each sample, its decibels (or the gain it
 * needs), and the gains in loops of their own, around the one loop that has to go sample by sample (the gain's
 * attack and release), so the others pipeline. */
#include <enaga/dsp.h>

/* The one-pole coefficient for a time constant of `seconds` (63 % of the way there), stepped `rate` times a second. */
static float time_constant(float seconds, float rate) {
    const float steps = seconds * rate;
    return steps <= 1.0f ? 1.0f : 1.0f - enaga_exp(-enaga_reciprocal(steps));
}

/* ---- Bitcrusher ---- */

void enaga_crusher_init(enaga_crusher *c) {
    c->step = c->inv_step = 0.0f;
    c->held_l = c->held_r = 0.0f;
    c->phase = 0;
    enaga_crusher_set(c, 24.0f, 48000.0f, 48000.0f);
}

void enaga_crusher_set(enaga_crusher *c, float bits, float rate_hz, float sample_rate) {
    bits = enaga_clamp(bits, 1.0f, 24.0f);
    c->step = enaga_exp2(1.0f - bits);                 /* 2^bits steps across -1 to 1 */
    c->inv_step = enaga_exp2(bits - 1.0f);
    c->increment = (uint32_t)(enaga_clamp(rate_hz * enaga_reciprocal(sample_rate), 1e-4f, 1.0f) * 2147483648.0f);
}

static inline float crush(float x, float step, float inv_step) {
    return (float)enaga_floor_int(enaga_clamp(x * inv_step + 0.5f, -1e9f, 1e9f)) * step;
}

void enaga_crusher_process(enaga_crusher *c, float *restrict left, float *restrict right, int n) {
    const float step = c->step, inv_step = c->inv_step;
    const uint32_t increment = c->increment;
    uint32_t phase = c->phase;
    float hl = c->held_l, hr = c->held_r;
    int i;
    /* Every sample is quantised and the ones taken kept: no branch, so the loops pipeline. */
    if (right) {
        for (i = 0; i < n; i++) {
            const float ql = crush(left[i], step, inv_step), qr = crush(right[i], step, inv_step);
            const int take = (phase += increment) >= 0x80000000u;
            phase &= 0x7FFFFFFFu;
            left[i] = hl = take ? ql : hl;
            right[i] = hr = take ? qr : hr;
        }
    } else {
        for (i = 0; i < n; i++) {
            const float ql = crush(left[i], step, inv_step);
            const int take = (phase += increment) >= 0x80000000u;
            phase &= 0x7FFFFFFFu;
            left[i] = hl = take ? ql : hl;
        }
    }
    c->phase = phase;
    c->held_l = hl;
    c->held_r = hr;
}

/* ---- Compressor ---- */

#define COMPRESSOR_CHUNK 32

void enaga_compressor_init(enaga_compressor *c, float rate) {
    c->rate = rate;
    c->reduction = 0.0f;
    enaga_compressor_set(c, -12.0f, 4.0f, 0.005f, 0.1f, 6.0f, 0.0f);
}

void enaga_compressor_set(enaga_compressor *c, float threshold_db, float ratio, float attack_s, float release_s,
                          float knee_db, float makeup_db) {
    c->threshold = threshold_db;
    c->slope = 1.0f - enaga_reciprocal(ratio > 1.0f ? ratio : 1.0f);
    knee_db = knee_db > 0.0f ? knee_db : 0.0f;
    c->half_knee = 0.5f * knee_db;
    c->knee_scale = knee_db > 0.0f ? c->slope * enaga_reciprocal(2.0f * knee_db) : 0.0f;
    c->attack = time_constant(attack_s, c->rate);
    c->release = time_constant(release_s, c->rate);
    c->makeup = makeup_db;
}

void enaga_compressor_process(enaga_compressor *c, float *left, float *right, int n, const float *key_l,
                              const float *key_r) {
    float g[COMPRESSOR_CHUNK];
    const float *kl = key_l ? key_l : left, *kr = key_l ? key_r : right;
    const float threshold = c->threshold, slope = c->slope, half_knee = c->half_knee, knee_scale = c->knee_scale;
    const float attack = c->attack, release = c->release, makeup = c->makeup;
    float reduction = c->reduction;
    int o, i;
    for (o = 0; o < n; o += COMPRESSOR_CHUNK) {
        const int m = n - o < COMPRESSOR_CHUNK ? n - o : COMPRESSOR_CHUNK;
        if (kr) {
            for (i = 0; i < m; i++) {
                const float a = enaga_abs(kl[o + i]), b = enaga_abs(kr[o + i]);
                g[i] = a > b ? a : b;
            }
        } else {
            for (i = 0; i < m; i++) g[i] = enaga_abs(kl[o + i]);
        }
        for (i = 0; i < m; i++) g[i] = enaga_gain_to_db(g[i]);
        /* The reduction the level calls for (the knee a quadratic joining 0 below it to the slope above), and
         * the reduction moving towards it, quickly when it rises, slowly when it falls. */
        for (i = 0; i < m; i++) {
            const float over = g[i] - threshold, x = over + half_knee;
            const float want = over >= half_knee ? slope * over : x > 0.0f ? knee_scale * x * x : 0.0f;
            reduction += (want > reduction ? attack : release) * (want - reduction);
            g[i] = makeup - reduction;
        }
        for (i = 0; i < m; i++) g[i] = enaga_db_to_gain(g[i]);
        for (i = 0; i < m; i++) left[o + i] *= g[i];
        if (right)
            for (i = 0; i < m; i++) right[o + i] *= g[i];
    }
    c->reduction = reduction;
}

/* ---- Limiter ----
 *
 * Each sample needs gain min(1, ceiling / peak). That need is held for at least the look-ahead L (the minimum
 * over the current block of 16 samples and the last K whole blocks, K x 16 >= L), released slowly where it
 * rises, and averaged over the last L samples (a box filter). The signal is delayed L - 1 samples: when a peak
 * leaves, the box holds L values all held from its need or lower, so its gain is down to the need, and it
 * reached there smoothly over L samples. L is at least a block, so each block reads its delay lines and the box
 * (all written before it) in loops apart from its writes, and the buffers are a block longer than L. A final clamp at the ceiling catches what rounding lets through.
 * The box keeps gain - 1, and the release fades the reduction to exactly 0, so with nothing to limit the box's sum
 * is exactly 0 and the gain exactly 1. */

#define LIMITER_BLOCK 16

static uint32_t limiter_length(float lookahead_s, float rate) {
    const float samples = lookahead_s * rate + 0.5f;
    return samples < (float)LIMITER_BLOCK ? LIMITER_BLOCK : samples > 4096.0f ? 4096u : (uint32_t)samples;
}

static uint32_t limiter_size(uint32_t length) {
    uint32_t size = 1;
    while (size < length + LIMITER_BLOCK) size <<= 1;
    return size;
}

uint32_t enaga_limiter_memory(float lookahead_s, float rate) {
    const uint32_t length = limiter_length(lookahead_s, rate);
    return 3 * limiter_size(length) + (length + LIMITER_BLOCK - 1) / LIMITER_BLOCK;
}

void enaga_limiter_init(enaga_limiter *l, float *memory, float lookahead_s, float rate) {
    const uint32_t length = limiter_length(lookahead_s, rate), size = limiter_size(length);
    uint32_t i;
    l->delay_l = memory;
    l->delay_r = memory + size;
    l->box = memory + 2 * size;
    l->blocks = memory + 3 * size;
    l->mask = size - 1;
    l->length = length;
    l->write = 0;
    l->block_count = (int32_t)((length + LIMITER_BLOCK - 1) / LIMITER_BLOCK);
    l->block = l->in_block = 0;
    for (i = 0; i < (uint32_t)l->block_count; i++) l->blocks[i] = 1.0f;
    l->partial = l->window = 1.0f;
    l->depth = 0.0f;
    l->sum = 0.0f;
    l->inv_length = enaga_reciprocal((float)length);
    l->rate = rate;
    l->lowest = 1.0f;
    enaga_limiter_set(l, 0.0f, 0.1f);
}

void enaga_limiter_set(enaga_limiter *l, float ceiling_db, float release_s) {
    l->ceiling = enaga_db_to_gain(ceiling_db);
    l->release = time_constant(release_s, l->rate);
}

void enaga_limiter_process(enaga_limiter *l, float *left, float *right, int n) {
    float g[LIMITER_BLOCK], old[LIMITER_BLOCK], kept[LIMITER_BLOCK], late_l[LIMITER_BLOCK], late_r[LIMITER_BLOCK];
    float *restrict const dl = l->delay_l, *restrict const dr = l->delay_r, *restrict const box = l->box;
    const uint32_t mask = l->mask, length = l->length, late = length - 1;
    const float ceiling = l->ceiling, need_ceiling = ceiling * 0.99999f, keep = 1.0f - l->release;
    const float inv_length = l->inv_length;
    float partial = l->partial, window = l->window, depth = l->depth, sum = l->sum, lowest = 1.0f;
    uint32_t w = l->write;
    int o = 0, i;
    while (o < n) {
        const int m = n - o < LIMITER_BLOCK - l->in_block ? n - o : LIMITER_BLOCK - l->in_block;
        float *restrict const xl = left + o, *restrict const xr = right ? right + o : 0;
        if (xr) {
            for (i = 0; i < m; i++) {
                const float a = enaga_abs(xl[i]), b = enaga_abs(xr[i]);
                g[i] = a > b ? a : b;
            }
        } else {
            for (i = 0; i < m; i++) g[i] = enaga_abs(xl[i]);
        }
        for (i = 0; i < m; i++) g[i] = g[i] > need_ceiling ? need_ceiling * enaga_reciprocal(g[i]) : 1.0f;
        for (i = 0; i < m; i++) old[i] = box[(w + (uint32_t)i - length) & mask];
        for (i = 0; i < m; i++) {
            float held, fading;
            partial = g[i] < partial ? g[i] : partial;
            held = partial < window ? partial : window;
            /* The reduction, 1 - gain: down to what is held at once, and fading away (to exactly 0) after. */
            fading = keep * depth;
            fading = fading < 1e-7f ? 0.0f : fading;
            depth = 1.0f - held > fading ? 1.0f - held : fading;
            kept[i] = -depth;
            sum += kept[i] - old[i];
            g[i] = 1.0f + sum * inv_length;
        }
        for (i = 0; i < m; i++) box[(w + (uint32_t)i) & mask] = kept[i];
        /* The delay: the samples leaving, read before this block's are written (late < L, so they were). Where
         * late is under the block, some leaving samples are this block's own, read from the input. */
        for (i = 0; i < m; i++) {
            const uint32_t from = w + (uint32_t)i - late;
            late_l[i] = (int)late > i ? dl[from & mask] : xl[i - (int)late];
        }
        if (xr)
            for (i = 0; i < m; i++) {
                const uint32_t from = w + (uint32_t)i - late;
                late_r[i] = (int)late > i ? dr[from & mask] : xr[i - (int)late];
            }
        for (i = 0; i < m; i++) dl[(w + (uint32_t)i) & mask] = xl[i];
        if (xr)
            for (i = 0; i < m; i++) dr[(w + (uint32_t)i) & mask] = xr[i];
        for (i = 0; i < m; i++) {
            xl[i] = enaga_clamp(late_l[i] * g[i], -ceiling, ceiling);
            lowest = g[i] < lowest ? g[i] : lowest;
        }
        if (xr)
            for (i = 0; i < m; i++) xr[i] = enaga_clamp(late_r[i] * g[i], -ceiling, ceiling);
        w += (uint32_t)m;
        o += m;
        if ((l->in_block += m) == LIMITER_BLOCK) {
            /* A whole block: its minimum joins the window's, and the box's sum is made afresh, so rounding in
             * the running sum never builds up. */
            float s = 0.0f;
            uint32_t j;
            int k;
            l->blocks[l->block] = partial;
            l->block = l->block + 1 == l->block_count ? 0 : l->block + 1;
            l->in_block = 0;
            partial = 1.0f;
            window = 1.0f;
            for (k = 0; k < l->block_count; k++) window = l->blocks[k] < window ? l->blocks[k] : window;
            for (j = 1; j <= length; j++) s += box[(w - j) & mask];
            sum = s;
        }
    }
    l->write = w & mask;
    l->partial = partial;
    l->window = window;
    l->depth = depth;
    l->sum = sum;
    l->lowest = lowest;
}
