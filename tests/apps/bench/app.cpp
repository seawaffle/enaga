// Benchmark app: times the DSP library's block functions, and the inline per-sample functions in a loop compiled
// by GCC as an app's own code would be, on 64-frame blocks. Logs one line per item, "name cycles", the fewest
// cycles of several calls (TSCL). tools/bench.py runs it on the emulator with the library built by cl6x and by GCC.
#include <enaga/app.hpp>
#include <enaga/dsp.h>

#include <cmath>
#include <cstdint>

namespace {

constexpr int N = 64;
constexpr int kRepeats = 4;

inline uint32_t tscl() {
    uint32_t t;
    asm volatile("mvc .s2 TSCL, %0" : "=b"(t));
    return t;
}

void append(char *&out, const char *text) { while (*text) *out++ = *text++; }
void append(char *&out, uint32_t value) {
    char digits[12];
    int n = 0;
    do { digits[n++] = char('0' + value % 10); value /= 10; } while (value);
    while (n) *out++ = digits[--n];
}
void report(const char *name, uint32_t cycles) {
    char text[64], *out = text;
    append(out, name);
    append(out, " ");
    append(out, cycles);
    *out = 0;
    enaga::log(text);
}

template <class F> void time(const char *name, F f) {
    uint32_t best = ~0u;
    for (int r = 0; r < kRepeats; r++) {
        uint32_t t0 = tscl();
        f();
        uint32_t t = tscl() - t0;
        if (t < best) best = t;
    }
    uint32_t t0 = tscl();                               // the cost of timing nothing
    uint32_t empty = tscl() - t0;
    report(name, best > empty ? best - empty : 0);
}

float a[N], b[N], c[N], d[N], l[N], r[N];
float g16[16], k16[16];
enaga_svf_coefficients svfc16[16];
float pool[112 * 1024] __attribute__((aligned(8)));
float table[2049], table2[2049];
float taps[128];
int16_t pcm16[N * 2];
volatile float sink;

void fill(float *x, float scale) {
    uint32_t s = 12345;
    for (int i = 0; i < N; i++) x[i] = scale * enaga_bipolar(enaga_xorshift(&s));
}

}  // namespace

class Bench : public enaga::App {
public:
    bool init() override {
        asm volatile("mvc .s2 %0, TSCL" : : "b"(0u));       // the counter runs once written
        fill(a, 0.5f); fill(b, 0.5f); fill(c, 0.5f);
        float *free = pool;
        auto take = [&](uint32_t floats) {
            float *p = free;
            free += (floats + 1) & ~1u;
            if (free > pool + sizeof pool / sizeof pool[0]) enaga::fatal("bench pool too small");
            return p;
        };

        // Buffers
        time("clear", [] { enaga_clear(c, N); });
        time("add", [] { enaga_add(c, a, N); });
        time("mix", [] { enaga_mix(c, a, N, 0.5f); });
        time("scale", [] { enaga_scale(c, N, 0.99f); });
        time("multiply", [] { enaga_multiply(c, a, N); });
        time("soft_clip_buffer", [] { fill(c, 0.5f); enaga_soft_clip_buffer(c, N, 2.0f); });
        time("mix_product", [] { enaga_mix_product(c, a, b, N, 0.5f); });
        time("exp2_buffer", [] { fill(c, 4.0f); enaga_exp2_buffer(c, N); });
        time("reciprocal_buffer", [] { for (int i = 0; i < N; i++) c[i] = 1.0f + i; enaga_reciprocal_buffer(c, N); });
        time("tan_pi_buffer", [] { fill(c, 0.45f); enaga_tan_pi_buffer(c, N); });
        time("tan_pi_buffer16", [] { for (int i = 0; i < 16; i++) g16[i] = 0.01f * i; enaga_tan_pi_buffer(g16, 16); });
        time("sqrt_buffer", [] { for (int i = 0; i < N; i++) c[i] = 1.0f + i; enaga_sqrt_buffer(c, N); });
        time("tanh_buffer", [] { fill(c, 1.0f); enaga_tanh_buffer(c, N, 2.0f); });
        time("svf_coefficients16", [] {
            for (int i = 0; i < 16; i++) { g16[i] = 0.1f + 0.01f * i; k16[i] = 1.0f; }
            enaga_svf_coefficients_buffer(g16, k16, svfc16, 16);
        });
        static float gl = 0.2f, gr = 0.3f;
        time("ramp_stereo_add", [] { enaga_ramp_stereo_add(a, l, r, N, &gl, &gr, 1e-4f, -1e-4f); });
        static uint32_t noise = 1;
        time("noise_add", [] { enaga_noise_add(&noise, c, N, 0.1f); });

        // Oscillators
        static enaga_oscillator osc = {0}, master = {0x40000000u};
        static float carry = 0;
        const float dt = 440.0f / 48000.0f, inv = 48000.0f / 440.0f;
        static const char *const waves[] = {"saw", "pulse", "triangle", "sine"};
        static char names[4][32], sync_names[4][32];
        for (int w = 0; w < 4; w++) {
            char *o = names[w]; append(o, "osc_"); append(o, waves[w]); *o = 0;
            o = sync_names[w]; append(o, "sync_"); append(o, waves[w]); *o = 0;
            time(names[w], [=] { enaga_oscillator_add(&osc, c, N, w, dt, inv, 0.5f, 0.5f); });
            time(sync_names[w], [=] {
                enaga_oscillator_sync_add(&osc, &carry, &master, dt * 0.7f, inv / 0.7f, c, N, w, dt, inv, 0.5f, 0.5f);
            });
        }
        for (int i = 0; i <= 2048; i++) table[i] = table2[i] = enaga_sin_turns(i / 2048.0f);
        time("wavetable", [=] { enaga_wavetable_add(&osc, table, 11, c, N, dt, 0.5f); });
        time("wavetable_morph", [=] { enaga_wavetable_morph_add(&osc, table, table2, 11, 0.3f, c, N, dt, 0.5f); });

        // Filters
        static enaga_svf s1, s2;
        static enaga_svf_coefficients sc = {0.8f, 0.2f, 0.05f, 1.0f};
        time("svf_process", [] { enaga_svf_process(&s1, &sc, ENAGA_SVF_LOW, a, c, N); });
        time("svf_cascade", [] { enaga_svf_cascade(&s1, &sc, &s2, &sc, ENAGA_SVF_LOW, a, c, N); });
        static enaga_one_pole op;
        time("one_pole", [] { enaga_one_pole_process(&op, 0.1f, a, c, N); });
        static enaga_dc_blocker dc;
        time("dc_blocker", [] { enaga_dc_blocker_process(&dc, 0.999f, a, c, N); });
        static enaga_biquad bq1, bq2;
        static enaga_biquad_coefficients bc;
        time("biquad_design", [] { enaga_biquad_design(&bc, ENAGA_BIQUAD_PEAK, 1000.0f, 0.7f, 6.0f, 48000.0f); });
        time("biquad_process", [] { enaga_biquad_process(&bq1, &bc, a, c, N); });
        time("biquad_cascade", [] { enaga_biquad_cascade(&bq1, &bc, &bq2, &bc, a, c, N); });
        for (int i = 0; i < 128; i++) taps[i] = 1.0f / (1 + i);
        static enaga_fir f16, f128;
        enaga_fir_init(&f16, taps, 16, take(enaga_fir_memory(16)));
        enaga_fir_init(&f128, taps, 128, take(enaga_fir_memory(128)));
        time("fir16", [] { enaga_fir_process(&f16, a, c, N); });
        time("fir128", [] { enaga_fir_process(&f128, a, c, N); });

        // Delays and effects
        static enaga_delay_line dl;
        enaga_delay_init(&dl, take(4096), 4096);
        time("delay_write", [] { enaga_delay_write_block(&dl, a, N); });
        time("delay_read", [] { enaga_delay_read(&dl, 1000, c, N); });
        for (int i = 0; i < N; i++) d[i] = 500.0f + 0.37f * i;
        time("delay_read_linear", [] { enaga_delay_read_linear(&dl, d, c, N); });
        static enaga_stereo_delay sd;
        enaga_stereo_delay_init(&sd, take(enaga_stereo_delay_memory(0.2f, 48000.0f)), 0.2f, 48000.0f);
        enaga_stereo_delay_set(&sd, 0.15f, 0.5f, 0.5f, 1);
        time("stereo_delay", [] { enaga_stereo_delay_process(&sd, a, b, l, r, N, 0.5f); });
        static enaga_reverb rv;
        enaga_reverb_init(&rv, take(enaga_reverb_memory(48000.0f)), 48000.0f);
        enaga_reverb_set(&rv, 2.0f, 0.5f);
        time("reverb", [] { enaga_reverb_process(&rv, a, b, l, r, N, 0.5f); });
        static enaga_chorus ch;
        enaga_chorus_init(&ch, take(enaga_chorus_memory(48000.0f)), 48000.0f);
        enaga_chorus_set(&ch, 0.5f, 0.5f);
        time("chorus", [] { enaga_chorus_process(&ch, a, b, l, r, N, 0.5f); });
        static enaga_crusher cr;
        enaga_crusher_init(&cr);
        enaga_crusher_set(&cr, 8.0f, 12000.0f, 48000.0f);
        time("crusher_stereo", [] { enaga_crusher_process(&cr, l, r, N); });
        static enaga_compressor co;
        enaga_compressor_init(&co, 48000.0f);
        time("compressor_stereo", [] { fill(l, 0.9f); fill(r, 0.9f); enaga_compressor_process(&co, l, r, N, nullptr, nullptr); });
        static enaga_limiter li;
        enaga_limiter_init(&li, take(enaga_limiter_memory(0.002f, 48000.0f)), 0.002f, 48000.0f);
        enaga_limiter_set(&li, -0.3f, 0.1f);
        time("limiter_stereo", [] { fill(l, 1.5f); fill(r, 1.5f); enaga_limiter_process(&li, l, r, N); });

        // Envelopes and smoothing
        static enaga_envelope env = {ENAGA_ENV_ATTACK, 0.0f};
        static enaga_envelope_shape shape = {0.001f, 0.001f, 0.5f, 0.001f};
        time("envelope", [] { enaga_envelope_render(&env, &shape, c, N); });
        static enaga_smoother sm = {0.0f, 1.0f, 0.001f};
        time("smoother", [] { enaga_smoother_render(&sm, c, N); });

        // Sample formats
        for (int i = 0; i < 2 * N; i++) pcm16[i] = int16_t(i * 300);
        time("pcm16_to_float", [] { enaga_pcm_to_float(pcm16, ENAGA_PCM_S16, 1, c, N); });
        time("pcm24_to_float", [] { enaga_pcm_to_float(pcm16, ENAGA_PCM_S24, 1, c, N); });
        time("int16_to_float", [] { enaga_int16_to_float(pcm16, c, N, 1.0f); });
        time("float_to_int16", [] { enaga_float_to_int16(a, pcm16, N); });

        // Resampling: 64 outputs from 44.1 kHz
        static float *rt = take(enaga_resample_table_size());
        enaga_resample_table(rt, 44100, 48000);
        static enaga_resampler rs;
        enaga_resampler_init(&rs, rt, 44100, 48000);
        time("resample64", [] { int used; fill(c, 0.5f); enaga_resample(&rs, c, 60, d, N, &used); });

        // Sample player: mono and stereo, rate 1 and an off rate, plain and added with gains
        static enaga_sample mono = {take(8000), 8000, 1, 48000}, stereo = {take(16000), 8000, 2, 48000};
        for (uint32_t i = 0; i < 8000; i++) mono.data[i] = stereo.data[2 * i] = stereo.data[2 * i + 1] = 0.001f * (i % 1000);
        static enaga_sample_player pm, ps;
        enaga_sample_player_start(&pm, &mono, 0, 0, 0);
        enaga_sample_player_start(&ps, &stereo, 0, 0, 0);
        pm.loop = ps.loop = ENAGA_LOOP_FORWARD;
        pm.loop_start = ps.loop_start = 100; pm.loop_end = ps.loop_end = 7000;
        time("player_mono", [] { enaga_sample_player_render(&pm, l, nullptr, N, 1.0f); });
        time("player_mono_rate", [] { enaga_sample_player_render(&pm, l, nullptr, N, 1.2345f); });
        time("player_stereo_rate", [] { enaga_sample_player_render(&ps, l, r, N, 1.2345f); });
        time("player_add_mono", [] { enaga_sample_player_render_add(&pm, l, r, N, 1.2345f, 0.5f, 0.4f); });
        time("player_add_stereo", [] { enaga_sample_player_render_add(&ps, l, r, N, 1.2345f, 0.5f, 0.4f); });

        // The inline functions in an app's own loop (always GCC)
        time("app_exp2_loop", [] { for (int i = 0; i < N; i++) c[i] = enaga_exp2(a[i]); });
        time("app_sin_loop", [] { for (int i = 0; i < N; i++) c[i] = enaga_sin(a[i]); });
        time("app_tanh_loop", [] { for (int i = 0; i < N; i++) c[i] = enaga_tanh(a[i]); });
        time("app_tan_pi_loop", [] { for (int i = 0; i < N; i++) c[i] = enaga_tan_pi(0.4f * a[i]); });
        time("app_reciprocal_loop", [] { for (int i = 0; i < N; i++) c[i] = enaga_reciprocal(2.0f + a[i]); });
        time("app_sqrt_loop", [] { for (int i = 0; i < N; i++) c[i] = enaga_sqrt(1.0f + a[i]); });
        time("app_log2_loop", [] { for (int i = 0; i < N; i++) c[i] = enaga_log2(1.0f + a[i]); });
        time("app_svf_step_loop", [] {
            float lo, bp, hp;
            for (int i = 0; i < N; i++) { enaga_svf_step(&s1, &sc, a[i], &lo, &bp, &hp); c[i] = lo; }
        });
        time("app_biquad_step_loop", [] { for (int i = 0; i < N; i++) c[i] = enaga_biquad_step(&bq1, &bc, a[i]); });
        time("app_envelope_step_loop", [] { for (int i = 0; i < N; i++) c[i] = enaga_envelope_step(&env, &shape); });
        time("app_divide_loop", [] { for (int i = 0; i < N; i++) c[i] = a[i] / (2.0f + b[i]); });
        time("libm_sinf_loop", [] { for (int i = 0; i < N; i++) c[i] = std::sin(a[i]); });
        time("libm_expf_loop", [] { for (int i = 0; i < N; i++) c[i] = std::exp(a[i]); });
        time("libm_tanhf_loop", [] { for (int i = 0; i < N; i++) c[i] = std::tanh(a[i]); });
        time("libm_powf_loop", [] { for (int i = 0; i < N; i++) c[i] = std::pow(1.0f + a[i], b[i]); });
        sink = c[0] + l[0] + r[0] + d[0];
        enaga::log("done");
        return true;
    }
};

ENAGA_APP(Bench)
