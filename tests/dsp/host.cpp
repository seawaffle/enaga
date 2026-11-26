// The DSP library (include/enaga/dsp) on the host: accuracy of the math, oscillator pitch, filter response,
// envelope times, delay time and reverb decay, and the block functions (runtime/dsp) giving exactly what the
// per-sample steps give. Run by tests/test_dsp.py; prints one line per failed check and
// exits non-zero if any failed.
#include <enaga/dsp.hpp>
#include <enaga/music.hpp>
#include <enaga/queue.hpp>
#include <enaga/sequencer.hpp>
#include <enaga/wav.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

using namespace enaga::dsp;

// runtime/libm.c, built with ENAGA_LIBM_HOST: its functions as enaga_host_<name>, beside the host's libm.
extern "C" {
#define UNARY(f) float enaga_host_##f(float);
#define BINARY(f) float enaga_host_##f(float, float);
UNARY(sinf) UNARY(cosf) UNARY(tanf) UNARY(asinf) UNARY(acosf) UNARY(atanf) UNARY(sinhf) UNARY(coshf) UNARY(tanhf)
UNARY(expf) UNARY(exp2f) UNARY(expm1f) UNARY(logf) UNARY(log2f) UNARY(log10f) UNARY(log1pf) UNARY(sqrtf) UNARY(cbrtf)
UNARY(fabsf) UNARY(truncf) UNARY(floorf) UNARY(ceilf) UNARY(roundf) UNARY(rintf) UNARY(nearbyintf)
BINARY(atan2f) BINARY(powf) BINARY(hypotf) BINARY(copysignf) BINARY(fminf) BINARY(fmaxf) BINARY(fdimf) BINARY(fmodf)
#undef UNARY
#undef BINARY
void enaga_host_sincosf(float, float *, float *);
long enaga_host_lroundf(float);
long enaga_host_lrintf(float);
float enaga_host_ldexpf(float, int);
float enaga_host_scalbnf(float, int);
float enaga_host_frexpf(float, int *);
float enaga_host_modff(float, float *);
}

static int failures = 0;
static void check(bool ok, const char *what, double got, double want) {
    if (!ok) {
        std::printf("FAIL %s: got %.9g, want %.9g\n", what, got, want);
        failures++;
    }
}
static void near(const char *what, double got, double want, double tolerance) {
    check(std::fabs(got - want) <= tolerance, what, got, want);
}

static constexpr float kRate = 48000.0f;

// Amplitude of a steady sine at `frequency` through f, after a settling time.
template <class F> static double sine_gain(double frequency, F &&f) {
    double peak = 0;
    for (int i = 0; i < 48000; i++) {
        float y = f(float(std::sin(2 * M_PI * frequency * i / kRate)));
        if (i > 24000) peak = std::fmax(peak, std::fabs(y));
    }
    return peak;
}

static void math() {
    double worst = 0;
    for (float x = -20; x <= 20; x += 0.0013f) worst = std::fmax(worst, std::fabs(exp2(x) / std::exp2(double(x)) - 1));
    near("exp2 relative error", worst, 0, 3e-7);
    worst = 0;
    for (float x = 1e-3f; x < 1e4f; x *= 1.0137f) worst = std::fmax(worst, std::fabs(log2(x) - std::log2(double(x))));
    near("log2 absolute error", worst, 0, 1e-5);
    worst = 0;
    for (float x = 1e-30f; x < 1e30f; x *= 1.00071f) {
        double want = std::log2(double(x));
        worst = std::fmax(worst, std::fabs(log2(x) - want) / std::fmax(1.0, std::fabs(want)));
    }
    near("log2 error, scaled by |log2 x| beyond 1", worst, 0, 2e-7);
    worst = 0;
    for (float x = 0.5f; x <= 2; x += 0.00013f) worst = std::fmax(worst, std::fabs(log2(x) - std::log2(double(x))));
    near("log2 absolute error, 0.5 to 2", worst, 0, 2e-7);
    near("log(e)", log(float(M_E)), 1, 3e-7);
    near("log10(1000)", log10(1000.0f), 3, 1e-6);
    worst = 0;
    for (float x = 1e-3f; x < 1e3f; x *= 1.0137f)
        for (float y = -4; y <= 4; y += 0.37f)
            worst = std::fmax(worst, std::fabs(pow(x, y) / std::pow(double(x), double(y)) - 1));
    near("pow relative error (x 1e-3 to 1e3, y -4 to 4)", worst, 0, 6e-6);
    check(pow(0.0f, 2.0f) == 0 && pow(-2.0f, 2.0f) == 0, "pow of x <= 0 is 0", pow(-2.0f, 2.0f), 0);
    near("db_to_gain(-6.0206)", db_to_gain(-6.0206f), 0.5, 1e-6);
    near("gain_to_db(10)", gain_to_db(10.0f), 20, 1e-5);
    near("gain_to_db(0)", gain_to_db(0.0f), -600, 1e-3);
    check(abs(-2.5f) == 2.5f && copysign(3.0f, -0.0f) == -3 && copysign(-3.0f, 1.0f) == 3, "abs, copysign", copysign(3.0f, -0.0f), -3);
    check(wrap(2.25f) == 0.25f && wrap(-0.25f) == 0.75f && wrap(-1e-9f) == 0, "wrap", wrap(-1e-9f), 0);
    worst = 0;
    for (float x = 1e-30f; x < 1e30f; x *= 1.00071f) {
        worst = std::fmax(worst, std::fabs(sqrt(x) / std::sqrt(double(x)) - 1));
        worst = std::fmax(worst, std::fabs(rsqrt(x) * std::sqrt(double(x)) - 1));
    }
    near("sqrt and rsqrt relative error", worst, 0, 3e-7);
    check(sqrt(0.0f) == 0 && sqrt(-1.0f) == 0, "sqrt of 0 and below", sqrt(-1.0f), 0);
    worst = 0;
    for (float x = -100; x <= 100; x += 0.0071f) {
        double e = 4e-6 + 1e-7 * std::fabs(x);
        worst = std::fmax(worst, std::fabs(sin(x) - std::sin(double(x))) / e);
        worst = std::fmax(worst, std::fabs(cos(x) - std::cos(double(x))) / e);
    }
    near("sin and cos absolute error, in 4e-6 + 1e-7 |x|", worst, 0, 1);
    worst = 0;
    for (float x = -1.5f; x <= 1.5f; x += 0.00071f) worst = std::fmax(worst, std::fabs(tan(x) / std::tan(double(x)) - 1) * (x != 0));
    near("tan relative error, |x| <= 1.5", worst, 0, 2e-5);
    worst = 0;
    for (float x = -1000; x <= 1000; x += x > -2 && x < 2 ? 0.0001f : 0.13f)
        worst = std::fmax(worst, std::fabs(atan(x) - std::atan(double(x))));
    near("atan absolute error", worst, 0, 5e-7);
    worst = 0;
    for (float a = -3.14159f; a <= 3.14159f; a += 0.00113f)
        for (float r : {1e-3f, 1.0f, 1e4f}) {
            float x = float(r * std::cos(double(a))), y = float(r * std::sin(double(a)));
            worst = std::fmax(worst, std::fabs(atan2(y, x) - std::atan2(double(y), double(x))));
        }
    near("atan2 absolute error", worst, 0, 5e-7);
    check(atan2(0.0f, 0.0f) == 0 && std::fabs(atan2(0.0f, -1.0f) - M_PI) < 1e-6 && atan2(-1.0f, 0.0f) < 0, "atan2 axes",
          atan2(0.0f, -1.0f), M_PI);
    worst = 0;
    double edge = 0;
    for (float x = -1; x <= 1; x += 0.00013f) {
        double e = std::fmax(std::fabs(asin(x) - std::asin(double(x))), std::fabs(acos(x) - std::acos(double(x))));
        if (std::fabs(x) < 1 - 1e-6f) worst = std::fmax(worst, e); else edge = std::fmax(edge, e);
    }
    for (float x : {1.0f, -1.0f, 0.9999999f, -0.9999999f})
        edge = std::fmax(edge, std::fmax(std::fabs(asin(x) - std::asin(double(x))), std::fabs(acos(x) - std::acos(double(x)))));
    near("asin and acos absolute error", worst, 0, 1e-6);
    near("asin and acos absolute error near +-1", edge, 0, 4e-4);
    worst = 0;
    for (float x = -12; x <= 12; x += x > -1 && x < 1 ? 0.00007f : 0.0013f)
        worst = std::fmax(worst, std::fabs(tanh(x) / std::tanh(double(x)) - 1) * (x != 0));
    near("tanh relative error", worst, 0, 6e-7);
    check(tanh(0.0f) == 0 && tanh(100.0f) == 1 && tanh(-100.0f) == -1, "tanh limits", tanh(100.0f), 1);
    worst = 0;
    for (float t = -3; t <= 3; t += 0.00071f) worst = std::fmax(worst, std::fabs(sin_turns(t) - std::sin(2 * M_PI * t)));
    near("sin_turns absolute error", worst, 0, 4e-6);
    worst = 0;
    for (float x = 0; x <= 0.49f; x += 0.0007f) worst = std::fmax(worst, std::fabs(tan_pi(x) / std::tan(M_PI * x) - 1) * (x > 0));
    near("tan_pi relative error", worst, 0, 1e-4);
    worst = 0;
    for (float x = 1e-30f; x < 1e30f; x *= 1.0007f)
        for (float y : {x, -x}) worst = std::fmax(worst, std::fabs(double(reciprocal(y)) * y - 1));
    near("reciprocal relative error", worst, 0, 2e-7);
    near("semitones(12)", semitones(12), 2, 1e-6);
    near("soft_clip(0.1)", soft_clip(0.1f), 0.1, 2e-4);
    near("soft_clip(5)", soft_clip(5), 1, 1e-6);
    near("floor_int(-0.5)", floor_int(-0.5f), -1, 0);
}

static void oscillators() {
    for (Wave wave : {Wave::saw, Wave::pulse, Wave::triangle, Wave::sine}) {
        Oscillator osc;
        std::vector<float> out(48000, 0.0f);
        float dt = 440.0f / kRate;
        for (int i = 0; i < 48000; i += 16) osc.render_add(&out[i], 16, wave, dt, 1.0f / dt, 1.0f, 0.25f);
        int rising = 0;
        double mean = 0, peak = 0;
        for (int i = 2; i < 48000; i++) {
            rising += out[i - 1] < 0 && out[i] >= 0;   // once a cycle for each wave, the saw at mid-cycle
            mean += out[i] / 48000;
            peak = std::fmax(peak, std::fabs(out[i]));
        }
        near("oscillator cycles in a second", rising, 440, 1);
        near("oscillator mean", mean, 0, 0.01);
        check(peak < 1.6, "oscillator peak", peak, 1.6);
    }
}

static void filters() {
    Svf svf;
    auto c = SvfCoefficients::make(1000, 0, kRate);   // resonance 0: k = 2, a gain of 1 / k at the cutoff
    near("low-pass at DC", sine_gain(1, [&](float x) { return svf.low(x, c); }), 1, 0.01);
    near("low-pass at cutoff", sine_gain(1000, [&](float x) { return svf.low(x, c); }), 0.5, 0.01);
    // 12 dB/oct, steeper towards Nyquist than an analogue filter: 1 / |1 - w^2 + 2jw| with w = tan(pi f / rate) / g.
    near("low-pass a decade up", sine_gain(10000, [&](float x) { return svf.low(x, c); }), 0.00724, 0.0003);
    near("high-pass at cutoff", sine_gain(1000, [&](float x) { return svf.step(x, c).high; }), 0.5, 0.01);
    auto r = SvfCoefficients::make(1000, 0.9f, kRate);
    check(sine_gain(1000, [&](float x) { return svf.low(x, r); }) > 4, "resonance peaks at the cutoff", 0, 4);
    OnePole pole;
    float a = OnePole::coefficient(100, kRate);
    near("one-pole at cutoff", sine_gain(100, [&](float x) { return pole.step(x, a); }), 0.7071, 0.01);
}

static void modulation() {
    auto shape = EnvelopeShape::make(0.01f, 0.1f, 0.5f, 0.2f, kRate);
    Envelope env;
    env.gate_on();
    int i = 0;
    while (env.stage() == Envelope::attack) { env.step(shape); i++; }
    near("attack time (s)", i / kRate, 0.01, 0.0002);
    for (i = 0; i < 48000; i++) env.step(shape);
    near("sustain level", env.level(), 0.5, 1e-4);
    env.gate_off();
    for (i = 0; env.level() > 0.005f; i++) env.step(shape);
    // To 1% of the sustain level; aiming 0.001 below zero, that is ln(0.501 / 0.006) / ln(100) x 0.2 s.
    near("release to 1% of sustain (s)", i / kRate, 0.1922, 0.001);
    while (env.active()) env.step(shape);
    near("release ends at zero", env.level(), 0, 0);

    auto slow = EnvelopeShape::make(0.5f, 0.5f, 1.0f, 0.5f, kRate / 16);   // at a 16-sample control rate
    env.kill();
    env.gate_on();
    for (i = 0; env.stage() == Envelope::attack; i++) env.step(slow);
    near("attack at control rate (s)", i * 16 / kRate, 0.5, 0.001);

    Lfo lfo;
    Random random;
    double previous = lfo.value(LfoShape::sine), crossings = 0;
    for (i = 0; i < 3000; i++) {
        double v = lfo.step(LfoShape::sine, 2.0f / 3000, random);
        crossings += previous < 0 && v >= 0;
        previous = v;
    }
    near("lfo cycles", crossings, 2, 0);
}

static void effects() {
    StereoDelay delay;
    delay.init(1.0f, kRate);
    delay.set(0.1f, 0.5f, 1.0f, false);
    std::vector<float> l(48000), r(48000), zero(48000);
    std::vector<float> in(48000);
    in[0] = 1;
    // The time glides from its 0.25 s default to 0.1 s; run it in first.
    for (int i = 0; i < 48000; i += 64) delay.process(&zero[i], &zero[i], &l[i], &r[i], 64, 1.0f);
    std::fill(l.begin(), l.end(), 0.0f);
    for (int i = 0; i < 48000; i += 64) delay.process(&in[i], &in[i], &l[i], &r[i], 64, 1.0f);
    int first = 0;
    while (first < 48000 && std::fabs(l[first]) < 0.1f) first++;
    near("delay time (samples)", first, 4800, 2);
    double echo = 0;
    for (int i = 9500; i < 9700; i++) echo += l[i];
    near("second echo at feedback 0.5", echo, 0.5, 0.05);
    StereoDelay short_delay;                            // a delay shorter than the 0.25 s default starts at its max
    short_delay.init(0.1f, kRate);
    std::fill(l.begin(), l.end(), 0.0f);
    for (int i = 0; i < 48000; i += 64) short_delay.process(&in[i], &in[i], &l[i], &r[i], 64, 1.0f);
    first = 0;
    while (first < 48000 && std::fabs(l[first]) < 0.1f) first++;
    near("a short delay's first time (samples)", first, 4800, 2);

    Reverb reverb;
    reverb.init(kRate);
    reverb.set(1.0f, 0.0f);
    std::vector<float> rl(3 * 48000), rr(3 * 48000), rin(3 * 48000);
    rin[0] = 1;
    for (size_t i = 0; i < rl.size(); i += 64) reverb.process(&rin[i], &rin[i], &rl[i], &rr[i], 64, 1.0f);
    auto energy = [&](int from) {
        double e = 0;
        for (int i = from; i < from + 4800; i++) e += rl[i] * rl[i] + rr[i] * rr[i];
        return e;
    };
    // T60 = 1 s: 60 dB over a second. Undamped, the tail should fall 60 dB between 0.2-0.3 s and 1.2-1.3 s.
    double drop = 10 * std::log10(energy(9600) / energy(57600));
    near("reverb decay over 1 s (dB)", drop, 60, 6);
    check(energy(9600) > 1e-4, "reverb has a tail", energy(9600), 1e-4);
}

// Exact equality of two runs: the block functions must reproduce the per-sample code bit for bit.
static void same(const char *what, const std::vector<float> &a, const std::vector<float> &b) {
    size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) i++;
    check(i == a.size() && i == b.size(), what, double(i), double(a.size()));
}

// The delay and reverb as they were written per sample (before the block functions), as references.
namespace ref {
class DelayLine {
public:
    explicit DelayLine(uint32_t max_samples = 0) { allocate(max_samples); }
    void allocate(uint32_t max_samples) {
        uint32_t size = 1;
        while (size < max_samples + 2) size <<= 1;
        buffer_.assign(size, 0.0f);
        mask_ = size - 1;
        write_ = 0;
    }
    uint32_t capacity() const { return mask_ - 1; }
    void write(float x) { buffer_[write_ = (write_ + 1) & mask_] = x; }
    float tap(uint32_t delay) const { return buffer_[(write_ - delay) & mask_]; }
    float tap_linear(float delay) const {
        int whole = int(delay);
        float frac = delay - float(whole);
        return lerp(tap(uint32_t(whole)), tap(uint32_t(whole) + 1), frac);
    }

private:
    std::vector<float> buffer_;
    uint32_t mask_ = 0, write_ = 0;
};

class StereoDelay {
public:
    void init(float max_seconds, float sample_rate) {
        rate_ = sample_rate;
        left_.allocate(uint32_t(max_seconds * sample_rate) + 4);
        right_.allocate(uint32_t(max_seconds * sample_rate) + 4);
        current_ = target_ = std::min(0.25f * sample_rate, max_seconds * sample_rate);
    }
    void set(float seconds, float feedback, float tone, bool ping_pong) {
        target_ = clamp(seconds * rate_, 16.0f, float(left_.capacity() - 2));
        feedback_ = clamp(feedback, 0.0f, 0.95f);
        tone_ = OnePole::coefficient(lerp(800.0f, 16000.0f, clamp(tone, 0.0f, 1.0f) * clamp(tone, 0.0f, 1.0f)), rate_);
        ping_pong_ = ping_pong;
    }
    void process(const float *in_l, const float *in_r, float *out_l, float *out_r, int n, float mix) {
        for (int i = 0; i < n; i++) {
            current_ += 0.0005f * (target_ - current_);
            float dl = left_.tap_linear(current_), dr = right_.tap_linear(current_);
            float fl = damp_l_.step(dl, tone_), fr = damp_r_.step(dr, tone_);
            if (ping_pong_) {
                left_.write(0.5f * (in_l[i] + in_r[i]) + feedback_ * fr);
                right_.write(feedback_ * fl);
            } else {
                left_.write(in_l[i] + feedback_ * fl);
                right_.write(in_r[i] + feedback_ * fr);
            }
            out_l[i] += mix * dl;
            out_r[i] += mix * dr;
        }
    }

private:
    DelayLine left_, right_;
    OnePole damp_l_, damp_r_;
    float rate_ = 48000.0f, current_ = 0.0f, target_ = 0.0f, feedback_ = 0.0f, tone_ = 1.0f;
    bool ping_pong_ = false;
};

class Reverb {
public:
    static constexpr int kLines = 8;
    void init(float sample_rate) {
        rate_ = sample_rate;
        float scale = sample_rate / 48000.0f;
        for (int i = 0; i < kLines; i++) {
            length_[i] = uint32_t(float(kLengths[i]) * scale);
            lines_[i].allocate(length_[i]);
        }
        for (int i = 0; i < 4; i++) {
            diffuse_length_[i] = uint32_t(float(kDiffuse[i]) * scale);
            diffusers_[i].allocate(diffuse_length_[i]);
        }
        set(2.0f, 0.5f);
    }
    void set(float decay_s, float damping) {
        decay_s = clamp(decay_s, 0.2f, 30.0f);
        for (int i = 0; i < kLines; i++) gain_[i] = exp2(-9.96578428f * float(length_[i]) / (decay_s * rate_));
        damping_ = OnePole::coefficient(lerp(14000.0f, 1500.0f, clamp(damping, 0.0f, 1.0f)), rate_);
    }
    void process(const float *in_l, const float *in_r, float *out_l, float *out_r, int n, float mix) {
        for (int i = 0; i < n; i++) {
            float x = 0.25f * (in_l[i] + in_r[i]);
            for (int d = 0; d < 4; d++) {
                float delayed = diffusers_[d].tap(diffuse_length_[d] - 1);
                float v = x + 0.6f * delayed;
                diffusers_[d].write(v);
                x = delayed - 0.6f * v;
            }
            float s[kLines];
            for (int j = 0; j < kLines; j++) s[j] = damp_[j].step(lines_[j].tap(length_[j] - 1), damping_) * gain_[j];
            hadamard(s);
            for (int j = 0; j < kLines; j++) lines_[j].write(s[j] + x);
            float l = s[0] + s[2] + s[4] + s[6], r = s[1] + s[3] + s[5] + s[7];
            out_l[i] += mix * l;
            out_r[i] += mix * r;
        }
    }

private:
    static void hadamard(float *s) {
        for (int span = 1; span < kLines; span <<= 1)
            for (int j = 0; j < kLines; j += span << 1)
                for (int k = j; k < j + span; k++) {
                    float a = s[k], b = s[k + span];
                    s[k] = a + b;
                    s[k + span] = a - b;
                }
        for (int j = 0; j < kLines; j++) s[j] *= 0.35355339f;
    }
    static constexpr uint32_t kLengths[kLines] = {1123, 1361, 1597, 1823, 2083, 2357, 2689, 3037};
    static constexpr uint32_t kDiffuse[4] = {149, 211, 293, 397};
    DelayLine lines_[kLines], diffusers_[4];
    OnePole damp_[kLines];
    uint32_t length_[kLines] = {}, diffuse_length_[4] = {};
    float gain_[kLines] = {}, damping_ = 1.0f, rate_ = 48000.0f;
};
}  // namespace ref

// The block delay and reverb against the references, bit for bit, over uneven block sizes and setting changes.
static void effects_against_reference() {
    const int sizes[] = {64, 37, 1, 16, 100, 64, 13, 200};
    Random noise(7);
    std::vector<float> in_l(48000), in_r(48000);
    for (int i = 0; i < 48000; i++) { in_l[i] = noise.bipolar(); in_r[i] = 0.5f * noise.bipolar(); }
    for (int ping = 0; ping < 2; ping++) {
        StereoDelay a;
        ref::StereoDelay b;
        a.init(0.5f, kRate);
        b.init(0.5f, kRate);
        std::vector<float> al(48000, 0.1f), ar(48000), bl = al, br = ar;
        // The time glides (a time constant of 2000 samples): 30000 samples at the shortest delay (16 samples, as
        // long as a chunk) bring it all the way down; then longer and shorter again, with feedback and tone.
        const int change_at[] = {0, 30000, 36000, 42000, 48000};
        const float times[] = {0.0001f, 0.1f, 0.0004f, 0.25f};
        int at = 0, k = 0, change = 0;
        while (at < 48000) {
            if (at >= change_at[change]) {
                a.set(times[change], 0.7f, 0.3f * float(change), ping != 0);
                b.set(times[change], 0.7f, 0.3f * float(change), ping != 0);
                change++;
            }
            int n = std::min({sizes[k++ % 8], 48000 - at, change_at[change] - at});
            a.process(&in_l[at], &in_r[at], &al[at], &ar[at], n, 0.8f);
            b.process(&in_l[at], &in_r[at], &bl[at], &br[at], n, 0.8f);
            at += n;
        }
        same(ping ? "ping-pong delay left = per sample" : "stereo delay left = per sample", al, bl);
        same(ping ? "ping-pong delay right = per sample" : "stereo delay right = per sample", ar, br);
    }
    for (float rate : {48000.0f, 22050.0f}) {
        Reverb a;
        ref::Reverb b;
        a.init(rate);
        b.init(rate);
        std::vector<float> al(48000, -0.2f), ar(48000), bl = al, br = ar;
        int at = 0, k = 0;
        while (at < 48000) {
            if (at == 12000) { a.set(5.0f, 0.1f); b.set(5.0f, 0.1f); }
            int n = std::min(sizes[k++ % 8], 48000 - at);
            a.process(&in_l[at], &in_r[at], &al[at], &ar[at], n, 0.6f);
            b.process(&in_l[at], &in_r[at], &bl[at], &br[at], n, 0.6f);
            at += n;
        }
        same("reverb left = per sample", al, bl);
        same("reverb right = per sample", ar, br);
    }
}

static void blocks() {
    // Envelope: render in blocks of 16 against step, through attack (ending mid-block), decay into sustain, a
    // sustain change, release to idle, a retrigger during release and idle.
    {
        auto shape = EnvelopeShape::make(0.003f, 0.05f, 0.4f, 0.03f, kRate);
        Envelope a, b;
        std::vector<float> ya, yb;
        auto run = [&](int samples) {
            for (int i = 0; i < samples; i += 16) {
                float buf[16];
                a.render(buf, 16, shape);
                ya.insert(ya.end(), buf, buf + 16);
                for (int j = 0; j < 16; j++) yb.push_back(b.step(shape));
                check(a.stage() == b.stage(), "envelope stage after a block", a.stage(), b.stage());
            }
        };
        a.gate_on(); b.gate_on();
        run(9600);
        shape.sustain = 0.7f;
        run(4800);
        a.gate_off(); b.gate_off();
        run(800);
        a.gate_on(); b.gate_on();
        run(4800);
        a.gate_off(); b.gate_off();
        run(9600);
        check(!a.active(), "envelope idle after release", a.stage(), 0);
        same("envelope render = step", ya, yb);
    }
    // SVF: process against step for each output, in place and not.
    {
        auto c = SvfCoefficients::make(800, 0.7f, kRate);
        Random noise;
        std::vector<float> in(4096);
        for (float &x : in) x = noise.bipolar();
        for (int output = 0; output < 3; output++) {
            Svf a, b;
            std::vector<float> ya(in.size()), yb(in.size());
            for (size_t i = 0; i < in.size(); i += 64) a.process(&in[i], &ya[i], 64, c, Svf::Output(output));
            for (size_t i = 0; i < in.size(); i++) {
                auto o = b.step(in[i], c);
                yb[i] = output == 0 ? o.low : output == 1 ? o.band : o.high;
            }
            same("svf process = step", ya, yb);
            Svf d;
            std::vector<float> yd = in;
            for (size_t i = 0; i < yd.size(); i += 16) d.process(&yd[i], &yd[i], 16, c, Svf::Output(output));
            same("svf process in place", yd, yb);
            auto c2 = SvfCoefficients::make(2500, 0.3f, kRate);
            Svf e1, e2, f1, f2;
            std::vector<float> ye(in.size()), yf = in;
            Svf::cascade(e1, c, e2, c2, in.data(), ye.data(), int(in.size()), Svf::Output(output));
            for (size_t i = 0; i < yf.size(); i += 16) {
                f1.process(&yf[i], &yf[i], 16, c, Svf::Output(output));
                f2.process(&yf[i], &yf[i], 16, c2, Svf::Output(output));
            }
            same("svf cascade = two processes", ye, yf);
        }
        OnePole p, q;
        std::vector<float> pa(in.size()), pb(in.size());
        p.process(in.data(), pa.data(), int(in.size()), 0.01f);
        for (size_t i = 0; i < in.size(); i++) pb[i] = q.step(in[i], 0.01f);
        same("one-pole process = step", pa, pb);
    }
    // Oscillators: one block of 64 against four of 16, every wave; phase() follows.
    for (int wave = 0; wave < 4; wave++) {
        Oscillator a, b;
        std::vector<float> ya(256), yb(256);
        float dt = 0.0371f;
        for (int i = 0; i < 256; i += 64) a.render_add(&ya[i], 64, Wave(wave), dt, 1 / dt, 0.5f, 0.3f);
        for (int i = 0; i < 256; i += 16) b.render_add(&yb[i], 16, Wave(wave), dt, 1 / dt, 0.5f, 0.3f);
        same("oscillator in blocks of 64 or 16", ya, yb);
        near("oscillator phase", a.phase(), std::fmod(256 * double(dt), 1.0), 1e-5);
    }
    // Noise: the same values as Random::bipolar.
    {
        Random a, b;
        std::vector<float> ya(100), yb(100);
        a.add_noise(ya.data(), 100, 0.5f);
        for (float &y : yb) y += 0.5f * b.bipolar();
        same("add_noise = bipolar", ya, yb);
    }
    // Math on buffers and SVF coefficients for many filters against the scalar functions.
    {
        std::vector<float> x(77), e, r, t, ge(77), k(77);
        Random noise;
        for (float &v : x) v = noise.bipolar();
        e = r = t = x;
        for (float &v : e) v *= 40.0f;
        for (float &v : t) v = std::fabs(v) * 0.5f;
        std::vector<float> e2 = e, r2 = r, t2 = t;
        enaga_exp2_buffer(e.data(), 77);
        enaga_reciprocal_buffer(r.data(), 77);
        enaga_tan_pi_buffer(t.data(), 77);
        for (float &v : e2) v = exp2(v);
        for (float &v : r2) v = reciprocal(v);
        for (float &v : t2) v = tan_pi(v);
        same("exp2_buffer = exp2", e, e2);
        same("reciprocal_buffer = reciprocal", r, r2);
        same("tan_pi_buffer = tan_pi", t, t2);
        std::vector<float> q = x, q2, h = x, h2;
        for (float &v : q) v = std::fabs(v) * 1000.0f;
        q2 = q;
        h2 = h;
        enaga_sqrt_buffer(q.data(), 77);
        enaga_tanh_buffer(h.data(), 77, 3.5f);
        for (float &v : q2) v = sqrt(v);
        for (float &v : h2) v = tanh(3.5f * v);
        same("sqrt_buffer = sqrt", q, q2);
        same("tanh_buffer = tanh(drive x)", h, h2);
        for (int i = 0; i < 77; i++) { ge[i] = t[i]; k[i] = 2.0f - 1.98f * noise.unipolar(); }
        std::vector<SvfCoefficients> c(77);
        enaga_svf_coefficients_buffer(ge.data(), k.data(), c.data(), 77);
        bool all = true;
        for (int i = 0; i < 77; i++) {
            auto f = SvfCoefficients::from_g(ge[i], k[i]);
            all = all && c[i].a1 == f.a1 && c[i].a2 == f.a2 && c[i].a3 == f.a3 && c[i].k == f.k;
        }
        check(all, "svf_coefficients_buffer = from_g", all, 1);
    }
    // Buffer arithmetic against plain loops.
    {
        std::vector<float> x(37), g(37), l1(37, 0.1f), r1(37, -0.2f), l2 = l1, r2 = r1, y1(37), y2(37);
        Random noise;
        for (int i = 0; i < 37; i++) { x[i] = noise.bipolar(); g[i] = noise.unipolar(); }
        float gl = 0.3f, gr = 0.6f, gl2 = gl, gr2 = gr;
        enaga_ramp_stereo_add(x.data(), l1.data(), r1.data(), 37, &gl, &gr, 0.01f, -0.005f);
        for (int i = 0; i < 37; i++) { gl2 += 0.01f; gr2 -= 0.005f; l2[i] += x[i] * gl2; r2[i] += x[i] * gr2; }
        same("ramp_stereo_add left", l1, l2);
        same("ramp_stereo_add right", r1, r2);
        check(gl == gl2 && gr == gr2, "ramp_stereo_add final gains", gl, gl2);
        y1 = x; y2 = x;
        enaga_multiply(y1.data(), g.data(), 37);
        enaga_scale(y1.data(), 37, 1.7f);
        enaga_soft_clip_buffer(y1.data(), 37, 3.0f);
        enaga_mix(y1.data(), x.data(), 37, 0.25f);
        enaga_add(y1.data(), g.data(), 37);
        for (int i = 0; i < 37; i++) {
            y2[i] *= g[i];
            y2[i] *= 1.7f;
            y2[i] = soft_clip(3.0f * y2[i]);
            y2[i] += 0.25f * x[i];
            y2[i] += g[i];
        }
        same("buffer arithmetic", y1, y2);
        enaga_clear(y1.data(), 37);
        check(y1 == std::vector<float>(37), "clear", y1[0], 0);
    }
}

// |H| of a biquad at `frequency`, in double.
static double biquad_gain(const BiquadCoefficients &c, double frequency) {
    double w = 2 * M_PI * frequency / kRate;
    double nr = c.b0 + c.b1 * std::cos(w) + c.b2 * std::cos(2 * w), ni = -c.b1 * std::sin(w) - c.b2 * std::sin(2 * w);
    double dr = 1 + c.a1 * std::cos(w) + c.a2 * std::cos(2 * w), di = -c.a1 * std::sin(w) - c.a2 * std::sin(2 * w);
    return std::sqrt((nr * nr + ni * ni) / (dr * dr + di * di));
}

// Biquads, FIR, wavetables and modulated delay reads: responses, and blocks against per-sample loops.
static void building_blocks() {
    using B = BiquadCoefficients;
    const double six = std::pow(10.0, 6.0 / 20);
    const float butterworth = 0.70710678f;
    near("biquad low-pass at DC", biquad_gain(B::make(B::low_pass, 1000, butterworth, kRate), 0), 1, 1e-4);
    near("biquad low-pass at cutoff", biquad_gain(B::make(B::low_pass, 1000, butterworth, kRate), 1000), M_SQRT1_2, 1e-4);
    near("biquad low-pass at 30 Hz", biquad_gain(B::make(B::low_pass, 30, butterworth, kRate), 30), M_SQRT1_2, 1e-4);
    near("biquad high-pass at 30 Hz", biquad_gain(B::make(B::high_pass, 30, butterworth, kRate), 30), M_SQRT1_2, 1e-4);
    near("biquad high-pass at Nyquist", biquad_gain(B::make(B::high_pass, 1000, butterworth, kRate), 24000), 1, 1e-4);
    near("biquad high-pass at cutoff", biquad_gain(B::make(B::high_pass, 1000, butterworth, kRate), 1000), M_SQRT1_2, 1e-4);
    near("biquad band-pass at centre", biquad_gain(B::make(B::band_pass, 3000, 4, kRate), 3000), 1, 1e-4);
    near("biquad notch at centre", biquad_gain(B::make(B::notch, 3000, 4, kRate), 3000), 0, 1e-4);
    for (double f : {50.0, 3000.0, 15000.0})
        near("biquad all-pass", biquad_gain(B::make(B::all_pass, 3000, 2, kRate), f), 1, 1e-4);
    near("biquad peak +6 dB at centre", biquad_gain(B::make(B::peak, 2000, 1, kRate, 6), 2000), six, 1e-3);
    near("biquad peak away from centre", biquad_gain(B::make(B::peak, 2000, 1, kRate, 6), 20), 1, 1e-3);
    near("biquad low shelf +6 dB at DC", biquad_gain(B::make(B::low_shelf, 300, butterworth, kRate, 6), 0), six, 1e-3);
    near("biquad low shelf at 20 kHz", biquad_gain(B::make(B::low_shelf, 300, butterworth, kRate, 6), 20000), 1, 1e-3);
    near("biquad high shelf -6 dB at Nyquist", biquad_gain(B::make(B::high_shelf, 5000, butterworth, kRate, -6), 24000),
         1 / six, 1e-3);
    near("biquad high shelf at DC", biquad_gain(B::make(B::high_shelf, 5000, butterworth, kRate, -6), 0), 1, 1e-3);

    Random noise;
    std::vector<float> in(4096);
    for (float &x : in) x = noise.bipolar();
    const int sizes[] = {64, 37, 1, 16, 100, 64, 13, 200};
    // Calls f(offset, n) over the input in uneven blocks.
    auto in_blocks = [&](auto &&f) {
        for (size_t at = 0, b = 0; at < in.size(); b++) {
            int n = int(std::min<size_t>(sizes[b % 8], in.size() - at));
            f(at, n);
            at += n;
        }
    };
    // Biquads: process (in place too) and cascade against step.
    {
        auto c1 = B::make(B::peak, 700, 3, kRate, 9), c2 = B::make(B::low_pass, 4000, 0.9f, kRate);
        Biquad a, b, d, e1, e2, f1, f2;
        std::vector<float> ya(in.size()), yb(in.size()), yd = in, ye(in.size()), yf(in.size());
        in_blocks([&](size_t at, int n) { a.process(&in[at], &ya[at], n, c1); });
        for (size_t i = 0; i < in.size(); i++) yb[i] = b.step(in[i], c1);
        same("biquad process = step", ya, yb);
        in_blocks([&](size_t at, int n) { d.process(&yd[at], &yd[at], n, c1); });
        same("biquad process in place", yd, yb);
        in_blocks([&](size_t at, int n) { Biquad::cascade(e1, c1, e2, c2, &in[at], &ye[at], n); });
        for (size_t i = 0; i < in.size(); i++) yf[i] = f2.step(f1.step(in[i], c1), c2);
        same("biquad cascade = two steps", ye, yf);
    }
    // FIR: lengths below, at and past a chunk, against the plain sum; in place too. No taps: silence.
    for (int length : {0, 1, 3, 32, 64, 65, 150}) {
        std::vector<float> taps(length);
        for (float &t : taps) t = noise.bipolar() / float(length);
        Fir a, d;
        a.init(taps.data(), length);
        d.init(taps.data(), length);
        std::vector<float> ya(in.size()), yb(in.size()), yd = in;
        in_blocks([&](size_t at, int n) { a.process(&in[at], &ya[at], n); });
        for (size_t i = 0; i < in.size(); i++) {
            float sum = 0.0f;
            for (int k = 0; k < length; k++) sum += taps[k] * (i >= size_t(k) ? in[i - k] : 0.0f);
            yb[i] = sum;
        }
        same("fir process = plain sum", ya, yb);
        in_blocks([&](size_t at, int n) { d.process(&yd[at], &yd[at], n); });
        same("fir process in place", yd, yb);
    }
    // Wavetables: a 2048-sample sine against sin of the same integer phase; blocks of 64 against 16; a negative dt;
    // morph against the two tables' lerp.
    {
        const int bits = 11, size = 1 << bits;
        std::vector<float> sine(size + 1), saw(size + 1);
        for (int i = 0; i <= size; i++) {
            sine[i] = float(std::sin(2 * M_PI * i / size));
            saw[i] = i == size ? -1.0f : 2.0f * i / size - 1.0f;
        }
        const float dt = 440.0f / kRate;
        const uint32_t dp = uint32_t(int32_t(dt * 4294967296.0f));
        Oscillator a, b;
        std::vector<float> ya(4096), yb(4096);
        for (int i = 0; i < 4096; i += 64) a.table_add(&ya[i], 64, sine.data(), bits, dt, 1.0f);
        for (int i = 0; i < 4096; i += 16) b.table_add(&yb[i], 16, sine.data(), bits, dt, 1.0f);
        same("wavetable in blocks of 64 or 16", ya, yb);
        double worst = 0;
        for (int i = 0; i < 4096; i++)
            worst = std::fmax(worst, std::fabs(ya[i] - std::sin(2 * M_PI * double(uint32_t(i * dp)) / 4294967296.0)));
        near("wavetable sine error", worst, 0, 2e-6);
        Oscillator r;
        std::vector<float> yr(256);
        r.table_add(yr.data(), 256, sine.data(), bits, -dt, 1.0f);
        worst = 0;
        for (int i = 0; i < 256; i++) worst = std::fmax(worst, std::fabs(yr[i] + ya[i]));
        near("wavetable backwards = sine reversed", worst, 0, 2e-6);
        Oscillator m, p1, p2;
        std::vector<float> ym(1000), y1(1000), y2(1000), yl(1000);
        m.morph_add(ym.data(), 1000, sine.data(), saw.data(), bits, 0.3f, 0.013f, 0.5f);
        p1.table_add(y1.data(), 1000, sine.data(), bits, 0.013f, 1.0f);
        p2.table_add(y2.data(), 1000, saw.data(), bits, 0.013f, 1.0f);
        for (int i = 0; i < 1000; i++) yl[i] = 0.5f * (y1[i] + (y2[i] - y1[i]) * 0.3f);
        same("wavetable morph = lerp of the tables", ym, yl);
    }
    // Modulated delay reads: read_linear then a block write, against tap_linear and write per sample, with a delay
    // swept by an LFO, never under the block length (200).
    {
        DelayLine a(2000), b(2000);
        std::vector<float> ya(in.size()), yb(in.size()), delay(in.size());
        for (size_t i = 0; i < in.size(); i++) delay[i] = 600.0f + 390.0f * sin_turns(float(i) * 0.0007f) + 0.37f;
        in_blocks([&](size_t at, int n) {
            a.read_linear(&delay[at], &ya[at], n);
            a.write(&in[at], n);
        });
        for (size_t i = 0; i < in.size(); i++) {
            yb[i] = b.tap_linear(delay[i]);
            b.write(in[i]);
        }
        same("delay read_linear = tap_linear", ya, yb);
    }
}

// Chorus, hard sync and the product mix.
static float c_base(float rate) { return std::max(0.008f * rate, 66.0f); }   // effects.c's chorus_base
static void synth_blocks() {
    Random noise;
    std::vector<float> in(4096);
    for (float &x : in) x = noise.bipolar();
    const int sizes[] = {64, 37, 1, 16, 100, 64, 13, 200};
    // Chorus against a per-sample reference on DelayLine (tap_linear, then write), also in place, at three rates.
    for (float rate : {48000.0f, 22050.0f, 8000.0f}) {
        Chorus a, b;
        a.init(rate);
        b.init(rate);
        a.set(1.3f, 0.8f);
        b.set(1.3f, 0.8f);
        DelayLine rl(uint32_t(2 * c_base(rate)) + 4), rr(uint32_t(2 * c_base(rate)) + 4);
        const enaga_chorus &c = a.state_;
        std::vector<float> al(in.size()), ar(in.size()), bl = in, br = in, wl(in.size()), wr(in.size());
        std::vector<float> in_r(in.rbegin(), in.rend());
        std::vector<float> br2 = in_r;
        for (size_t at = 0, k = 0; at < in.size(); k++) {
            int n = int(std::min<size_t>(sizes[k % 8], in.size() - at));
            a.process(&in[at], &in_r[at], &al[at], &ar[at], n, 0.7f);
            b.process(&bl[at], &br2[at], &bl[at], &br2[at], n, 0.7f);
            at += n;
        }
        for (size_t i = 0; i < in.size(); i++) {
            const float t = enaga_unipolar(uint32_t(i) * c.step);
            wl[i] = 0.7f * rl.tap_linear(c.base + c.depth * sin_turns(t));
            wr[i] = 0.7f * rr.tap_linear(c.base + c.depth * sin_turns(t + 0.25f));
            rl.write(in[i]);
            rr.write(in_r[i]);
        }
        same("chorus left = per sample", al, wl);
        same("chorus right = per sample", ar, wr);
        for (size_t i = 0; i < in.size(); i++) { wl[i] += in[i]; wr[i] += in_r[i]; }
        same("chorus in place left", bl, wl);
        same("chorus in place right", br2, wr);
        check(c.base - c.depth >= 66, "chorus shortest delay", c.base - c.depth, 66);
    }
    // Hard sync with a master that never wraps: the wave's own jumps alone, as render_add band-limits them.
    for (Wave wave : {Wave::saw, Wave::pulse}) {
        Oscillator master, a, b;
        float carry = 0.0f;
        const float dt = 0.0173f;
        std::vector<float> ya(4096), yb(4096);
        a.sync_add(ya.data(), 4096, master, 1e-6f, 1e6f, carry, wave, dt, 1 / dt, 1.0f, 0.3f);
        b.render_add(yb.data(), 4096, wave, dt, 1 / dt, 1.0f, 0.3f);
        double worst = 0;
        for (int i = 1; i < 4096; i++) worst = std::fmax(worst, std::fabs(ya[i] - yb[i]));
        near(wave == Wave::saw ? "sync saw without restarts = render_add" : "sync pulse without restarts = render_add",
             worst, 0, 2e-5);
    }
    // Hard sync: blocks of 64 against uneven blocks; periodic at the master's period (128 samples); smoother than
    // the naive restart (second differences: the energy above the band).
    for (int wave = 0; wave < 4; wave++) {
        const float dt_m = 1.0f / 128, dt = 0.0291f;
        auto run = [&](int block, std::vector<float> &y) {
            Oscillator master, o;
            float carry = 0.0f;
            for (size_t at = 0, k = 0; at < y.size(); k++) {
                int n = block ? block : int(std::min<size_t>(sizes[k % 8], y.size() - at));
                n = int(std::min<size_t>(size_t(n), y.size() - at));
                o.sync_add(&y[at], n, master, dt_m, 1 / dt_m, carry, Wave(wave), dt, 1 / dt, 1.0f, 0.4f);
                master.advance(n, dt_m);
                at += n;
            }
        };
        std::vector<float> ya(4096), yb(4096);
        run(64, ya);
        run(0, yb);
        same("sync in blocks of 64 or uneven", ya, yb);
        double worst = 0;
        for (int i = 128; i + 128 < 4096; i++) worst = std::fmax(worst, std::fabs(ya[i] - ya[i + 128]));
        near("sync period = the master's", worst, 0, 1e-6);
        if (wave <= int(Wave::pulse)) {
            double smooth = 0, rough = 0;
            for (int i = 2; i < 4096; i++) {
                auto naive = [&](int j) {
                    double x = std::fmod(std::fmod(j * double(dt_m), 1.0) / dt_m * dt, 1.0);
                    return wave == int(Wave::saw) ? 2 * x - 1 : (x < 0.4 ? 1.0 : -1.0);
                };
                double d2 = ya[i] - 2 * ya[i - 1] + ya[i - 2], n2 = naive(i) - 2 * naive(i - 1) + naive(i - 2);
                smooth += d2 * d2;
                rough += n2 * n2;
            }
            check(smooth < 0.5 * rough, "sync band-limited: less energy at the top", smooth / rough, 0.5);
        }
    }
    // Hard sync with restarts between samples (a master period of 100.37 samples): a sine away from its restarts
    // follows the exact synced phase, and a pulse whose falling edge often shares a sample with a restart keeps the
    // continuous wave's mean (each jump's correction in the right place).
    {
        const float dt_m = 1.0f / 100.37f, dt = dt_m * 1.45f;
        const uint32_t dpm = uint32_t(int32_t(dt_m * 4294967296.0f)), dp = uint32_t(int32_t(dt * 4294967296.0f));
        Oscillator m, o;
        float carry = 0.0f;
        std::vector<float> y(64 * 312);
        for (size_t at = 0; at < y.size(); at += 64) {
            o.sync_add(&y[at], 64, m, dt_m, 1 / dt_m, carry, Wave::sine, dt, 1 / dt, 1.0f);
            m.advance(64, dt_m);
        }
        double worst = 0;
        for (size_t j = 0; j < y.size(); j++) {
            double since = double(uint32_t(j * dpm)) / dpm;      // samples since the master's last wrap
            // A restart's correction touches the samples either side of it: skip the first after and the last before.
            if (since < 1 || since + 1 >= 1 / double(dt_m) || j < 200) continue;
            worst = std::fmax(worst, std::fabs(y[j] - std::sin(2 * M_PI * std::fmod(since * dp / 4294967296.0, 1.0))));
        }
        near("sync sine follows the synced phase", worst, 0, 1e-4);

        // Ratios 1.45 and 1.441: the edge 0.7 and 0.07 of a sample before the restart.
        for (float ratio : {1.45f, 1.441f}) {
            const float width = 0.44f, dts = dt_m * ratio;
            Oscillator m2, q;
            carry = 0.0f;
            std::vector<float> z(200000);
            for (size_t at = 0; at < z.size(); at += 64) {
                q.sync_add(&z[at], 64, m2, dt_m, 1 / dt_m, carry, Wave::pulse, dts, 1 / dts, 1.0f, width);
                m2.advance(64, dt_m);
            }
            double mean = 0;
            for (float v : z) mean += v;
            mean /= double(z.size());
            // Over a master period the slave runs `ratio` cycles, high for 2 x 0.44 of them; DC removal 1 - 2 width.
            const double continuous = (2 * width - (ratio - 2 * width)) / ratio + (1 - 2 * width);
            near("sync pulse mean = the continuous wave's", mean, continuous, 3e-4);
        }
    }
    // Product mix.
    {
        std::vector<float> a(37), b(37), y1(37, 0.25f), y2 = y1;
        for (int i = 0; i < 37; i++) { a[i] = noise.bipolar(); b[i] = noise.bipolar(); }
        enaga_mix_product(y1.data(), a.data(), b.data(), 37, 0.6f);
        for (int i = 0; i < 37; i++) y2[i] += 0.6f * a[i] * b[i];
        same("mix_product", y1, y2);
    }
}

// Converts all of `in` (then the zeros that flush it), in blocks of `block` inputs into at most `room` outputs.
static std::vector<float> resample_all(const std::vector<float> &in, uint32_t from, uint32_t to, int block, int room) {
    std::vector<float> table(enaga_resample_table_size()), out;
    enaga_resample_table(table.data(), from, to);
    enaga_resampler r;
    enaga_resampler_init(&r, table.data(), from, to);
    std::vector<float> feed(in);
    feed.resize(in.size() + ENAGA_RESAMPLE_TAPS / 2, 0.0f);
    std::vector<float> chunk(room);
    for (size_t at = 0;;) {                        // until every input is used and out was not filled
        int n = int(std::min<size_t>(block, feed.size() - at)), used = 0;
        int made = enaga_resample(&r, feed.data() + at, n, chunk.data(), room, &used);
        out.insert(out.end(), chunk.begin(), chunk.begin() + made);
        at += size_t(used);
        if (at == feed.size() && made < room) break;
    }
    return out;
}

// The largest error against a sine at `frequency` sampled at `to`, over the outputs from `skip` to `end - skip`.
static double sine_error(const std::vector<float> &out, double frequency, uint32_t to, size_t skip) {
    double worst = 0;
    for (size_t i = skip; i + skip < out.size(); i++)
        worst = std::fmax(worst, std::fabs(out[i] - 0.5 * std::sin(2 * M_PI * frequency * double(i) / to)));
    return worst;
}

static void samples() {
    // Formats, read at an odd address, every second sample (the right channel of a stereo file).
    const uint8_t s16[] = {0, 0x00, 0x80, 0xFF, 0x7F, 0x00, 0x40, 0x34, 0x12, 0x01, 0x00};
    float f[2];
    int16_t i16[2];
    enaga_pcm_to_float(s16 + 3, ENAGA_PCM_S16, 2, f, 2);           // 0x7FFF and 0x1234 (0x4000 skipped)
    near("s16 to float", f[0], 32767.0 / 32768, 1e-9);
    near("s16 to float, second", f[1], 0x1234 / 32768.0, 1e-9);
    const uint8_t s24[] = {9, 0x00, 0x00, 0x80, 0xFF, 0xFF, 0x7F, 0x80, 0x00, 0x00};
    enaga_pcm_to_float(s24 + 1, ENAGA_PCM_S24, 1, f, 2);
    near("s24 to float", f[0], -1, 0);
    near("s24 to float, top", f[1], 8388607.0 / 8388608, 1e-9);
    enaga_pcm_to_int16(s24 + 1, ENAGA_PCM_S24, 1, i16, 2);
    check(i16[0] == -32768 && i16[1] == 32767, "s24 to int16 saturates", i16[1], 32767);
    const uint8_t s24_round[] = {0xC0, 0, 0, 0x7F, 0, 0, 0x7F, 0xFF, 0xFF};   // 192, 127, -129: /256 to nearest
    int16_t r3[3];
    enaga_pcm_to_int16(s24_round, ENAGA_PCM_S24, 1, r3, 3);
    check(r3[0] == 1 && r3[1] == 0 && r3[2] == -1, "s24 to int16 rounds", r3[0] * 100 + r3[1] * 10 + r3[2], 99);
    const uint8_t u8[] = {0, 128, 255};
    enaga_pcm_to_float(u8, ENAGA_PCM_U8, 1, f, 2);
    near("u8 to float", f[0], -1, 0);
    near("u8 to float, middle", f[1], 0, 0);
    enaga_pcm_to_int16(u8 + 1, ENAGA_PCM_U8, 1, i16, 2);
    check(i16[0] == 0 && i16[1] == 127 * 256, "u8 to int16", i16[1], 127 * 256);
    const uint8_t s32[] = {1, 0x00, 0x80, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x7F};
    enaga_pcm_to_int16(s32 + 1, ENAGA_PCM_S32, 1, i16, 2);
    check(i16[0] == 1 && i16[1] == 32767, "s32 to int16 rounds and saturates", i16[0], 1);
    float halves[2] = {-0.25f, 2.0f};
    uint8_t fl[9];
    std::memcpy(fl + 1, halves, 8);
    enaga_pcm_to_float(fl + 1, ENAGA_PCM_FLOAT, 1, f, 2);
    check(f[0] == -0.25f && f[1] == 2.0f, "float copied", f[0], -0.25);
    enaga_pcm_to_int16(fl + 1, ENAGA_PCM_FLOAT, 1, i16, 2);
    check(i16[0] == -8192 && i16[1] == 32767, "float to int16", i16[1], 32767);
    const uint32_t odd[3] = {0x7F800000u, 0xFF800000u, 0x7FC00000u};    // +inf, -inf, NaN
    float odd_f[3];
    enaga_pcm_to_float(odd, ENAGA_PCM_FLOAT, 1, odd_f, 3);
    enaga_pcm_to_int16(odd, ENAGA_PCM_FLOAT, 1, r3, 3);
    check(odd_f[0] == 0 && odd_f[1] == 0 && odd_f[2] == 0 && r3[0] == 0 && r3[1] == 0 && r3[2] == 0,
          "infinity and NaN in a float WAV read as silence", odd_f[2], 0);
    const float back[4] = {0.5f / 32768, -1.5f, 1.0f, -0.6f / 32768};
    int16_t out16[4];
    enaga_float_to_int16(back, out16, 4);
    check(out16[0] == 1 && out16[1] == -32768 && out16[2] == 32767 && out16[3] == -1, "float_to_int16", out16[0], 1);
    {
        // wav::parse: a chunk size that puts the next chunk at 2^32 - 8 must not wrap back into the buffer.
        uint8_t file[20] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'J', 'U', 'N', 'K'};
        enaga::wav::detail::put32(file + 16, 0xFFFFFFE4u);
        enaga::wav::Format format;
        check(!enaga::wav::parse(file, sizeof file, format), "wav parse: a chunk size that wraps", 0, 0);
    }
    const int16_t pcm[2] = {-32768, 16384};
    enaga_int16_to_float(pcm, f, 2, 0.5f);
    check(f[0] == -0.5f && f[1] == 0.25f, "int16_to_float with gain", f[1], 0.25);
    check(enaga_pcm_format(1, 24) == ENAGA_PCM_S24 && enaga_pcm_format(3, 32) == ENAGA_PCM_FLOAT &&
          enaga_pcm_format(3, 64) == -1 && enaga_pcm_format(1, 12) == -1, "pcm_format", 0, 0);

    // Rate conversion: a sine converted matches the same sine sampled at the new rate.
    const uint32_t from = 44100, to = 48000;
    std::vector<float> sine(44100);
    for (size_t i = 0; i < sine.size(); i++) sine[i] = float(0.5 * std::sin(2 * M_PI * 1000 * double(i) / from));
    std::vector<float> out = resample_all(sine, from, to, 4096, 8192);
    near("44.1 to 48 kHz length", double(out.size()), 48000, 1);    // one output per 1/48000 s of input, and the end
    near("44.1 to 48 kHz, 1 kHz error", sine_error(out, 1000, to, 64), 0, 3e-5);
    for (size_t i = 0; i < sine.size(); i++) sine[i] = float(0.5 * std::sin(2 * M_PI * 18000 * double(i) / from));
    near("44.1 to 48 kHz, 18 kHz error", sine_error(resample_all(sine, from, to, 4096, 8192), 18000, to, 64), 0, 3e-4);
    // Any blocking gives the same samples: inputs a few at a time, outputs a few at a time.
    std::vector<float> small = resample_all(sine, from, to, 7, 3), large = resample_all(sine, from, to, 100000, 100000);
    check(small == large, "the same in any blocks", double(small.size()), double(large.size()));
    // Down: 96 to 48 kHz keeps 10 kHz and removes 30 kHz, which would otherwise alias to 18 kHz.
    std::vector<float> high(96000);
    for (size_t i = 0; i < high.size(); i++) high[i] = float(0.5 * std::sin(2 * M_PI * 10000 * double(i) / 96000));
    out = resample_all(high, 96000, 48000, 512, 512);
    near("96 to 48 kHz length", double(out.size()), 48000, 0);
    near("96 to 48 kHz, 10 kHz error", sine_error(out, 10000, 48000, 64), 0, 3e-5);
    for (size_t i = 0; i < high.size(); i++) high[i] = float(0.5 * std::sin(2 * M_PI * 30000 * double(i) / 96000));
    out = resample_all(high, 96000, 48000, 512, 512);
    double peak = 0;
    for (size_t i = 64; i + 64 < out.size(); i++) peak = std::fmax(peak, std::fabs(out[i]));
    check(20 * std::log10(peak / 0.5) < -85, "96 to 48 kHz, 30 kHz removed", 20 * std::log10(peak / 0.5), -85);
    // The C++ class: a second channel sharing the first's table gives the same samples as its own.
    Resampler left, right;
    left.init(44100, 48000);
    right.init(left);
    std::vector<float> a(256), b(256);
    int used_a, used_b;
    int made_a = left.process(sine.data(), 200, a.data(), 256, used_a), made_b = right.process(sine.data(), 200, b.data(), 256, used_b);
    check(made_a == made_b && used_a == 200 && a == b, "Resampler sharing a table", made_b, made_a);
    left.reset();
    std::vector<float> c(256);
    int made_c = left.process(sine.data(), 200, c.data(), 256, used_a);
    check(made_c == made_a && c == a, "Resampler reset", made_c, made_a);
}

// libm (runtime/libm.c) against the host's, in units in the last place of the float result.
static double ulps(float got, double want) {
    if (std::isnan(want)) return std::isnan(got) ? 0 : 1e9;
    if (std::isinf(want) || std::fabs(want) > 3.4028234e38) return got == float(want) ? 0 : 1e9;
    float near_want = float(want);
    double ulp = std::fabs(double(std::nextafter(std::fabs(near_want), INFINITY)) - std::fabs(near_want));
    if (std::fabs(want) < 1.1754944e-38) ulp = 1.4012985e-45;           // subnormal: the smallest step
    return std::fabs(double(got) - want) / ulp;
}

static uint32_t libm_seed = 12345;
static float any_float() {                                              // every bit pattern, NaN and inf included
    libm_seed ^= libm_seed << 13; libm_seed ^= libm_seed >> 17; libm_seed ^= libm_seed << 5;
    float f;
    std::memcpy(&f, &libm_seed, 4);
    return f;
}

template <class Mine, class Host> static void unary(const char *what, Mine mine, Host host, double limit,
                                                    float lo, float hi, int n = 200000) {
    double worst = 0;
    float at = 0;
    auto one = [&](float x) {
        double e = ulps(mine(x), host(double(x)));
        if (e > worst) { worst = e; at = x; }
    };
    for (int i = 0; i < n; i++) {                                       // random bit patterns in [lo, hi]
        float x = any_float();
        if (x >= lo && x <= hi) one(x);
    }
    for (int i = 0; i <= n / 4; i++) one(lo + (hi - lo) * float(i) / float(n / 4));   // and an even sweep
    for (float x : {0.0f, -0.0f, INFINITY, -INFINITY, NAN, 1.0f, -1.0f, 1e-45f, -1e-45f, 3.4e38f, -3.4e38f}) one(x);
    if (worst > limit) std::printf("FAIL %s: %.3g ulps at %.9g\n", what, worst, at), failures++;
}

static void libm() {
    unary("sinf", enaga_host_sinf, [](double x) { return std::sin(x); }, 1, -1.6e6f, 1.6e6f);
    unary("cosf", enaga_host_cosf, [](double x) { return std::cos(x); }, 1, -1.6e6f, 1.6e6f);
    unary("tanf", enaga_host_tanf, [](double x) { return std::tan(x); }, 1, -1.6e6f, 1.6e6f);
    unary("sinf, any", enaga_host_sinf, [](double x) { return std::sin(x); }, 1, -INFINITY, INFINITY);
    unary("cosf, any", enaga_host_cosf, [](double x) { return std::cos(x); }, 1, -INFINITY, INFINITY);
    unary("tanf, any", enaga_host_tanf, [](double x) { return std::tan(x); }, 1, -INFINITY, INFINITY);
    unary("sinf, small", enaga_host_sinf, [](double x) { return std::sin(x); }, 1, -4, 4);
    unary("tanf, small", enaga_host_tanf, [](double x) { return std::tan(x); }, 1, -4, 4);
    unary("asinf", enaga_host_asinf, [](double x) { return std::asin(x); }, 1, -1, 1);
    unary("acosf", enaga_host_acosf, [](double x) { return std::acos(x); }, 1, -1, 1);
    unary("atanf", enaga_host_atanf, [](double x) { return std::atan(x); }, 1, -INFINITY, INFINITY);
    unary("sinhf", enaga_host_sinhf, [](double x) { return std::sinh(x); }, 1, -100, 100);
    unary("coshf", enaga_host_coshf, [](double x) { return std::cosh(x); }, 1, -100, 100);
    unary("tanhf", enaga_host_tanhf, [](double x) { return std::tanh(x); }, 1, -INFINITY, INFINITY);
    unary("expf", enaga_host_expf, [](double x) { return std::exp(x); }, 1, -87, 89);
    unary("exp2f", enaga_host_exp2f, [](double x) { return std::exp2(x); }, 1, -126, 129);
    unary("expm1f", enaga_host_expm1f, [](double x) { return std::expm1(x); }, 1, -INFINITY, 89);
    unary("logf", enaga_host_logf, [](double x) { return std::log(x); }, 1, -INFINITY, INFINITY);
    unary("log2f", enaga_host_log2f, [](double x) { return std::log2(x); }, 1, -INFINITY, INFINITY);
    unary("log10f", enaga_host_log10f, [](double x) { return std::log10(x); }, 1, -INFINITY, INFINITY);
    unary("log1pf", enaga_host_log1pf, [](double x) { return std::log1p(x); }, 1, -INFINITY, INFINITY);
    unary("sqrtf", enaga_host_sqrtf, [](double x) { return std::sqrt(x); }, 0.5, -INFINITY, INFINITY);
    unary("cbrtf", enaga_host_cbrtf, [](double x) { return std::cbrt(x); }, 1, -INFINITY, INFINITY);
    struct Exact { const char *name; float (*mine)(float); float (*host)(float); };
    for (const Exact &f : {Exact{"truncf", enaga_host_truncf, truncf}, Exact{"floorf", enaga_host_floorf, floorf},
                           Exact{"ceilf", enaga_host_ceilf, ceilf}, Exact{"roundf", enaga_host_roundf, roundf},
                           Exact{"rintf", enaga_host_rintf, rintf}, Exact{"nearbyintf", enaga_host_nearbyintf, nearbyintf},
                           Exact{"fabsf", enaga_host_fabsf, fabsf}})
        unary(f.name, f.mine, [&f](double x) { return double(f.host(float(x))); }, 0, -INFINITY, INFINITY);

    double worst[6] = {};
    for (int i = 0; i < 400000; i++) {
        float x = any_float(), y = any_float();
        worst[0] = std::fmax(worst[0], ulps(enaga_host_atan2f(y, x), std::atan2(double(y), double(x))));
        worst[1] = std::fmax(worst[1], ulps(enaga_host_hypotf(x, y), std::hypot(double(x), double(y))));
        float r = enaga_host_fmodf(x, y), want = fmodf(x, y);
        if (!(r == want && std::signbit(r) == std::signbit(want)) && !(std::isnan(r) && std::isnan(want))) worst[2] = 1e9;
        float m = enaga_host_fminf(x, y), M = enaga_host_fmaxf(x, y);
        auto signalling = [](float v) { uint32_t u; std::memcpy(&u, &v, 4); return std::isnan(v) && !(u & 0x400000u); };
        if (!signalling(x) && !signalling(y) && (!(m == fminf(x, y) || (std::isnan(m) && std::isnan(fminf(x, y)))) ||
            !(M == fmaxf(x, y) || (std::isnan(M) && std::isnan(fmaxf(x, y)))))) worst[3] = 1e9;
        float px = std::fabs(x) < 1e4f && std::fabs(x) > 1e-4f ? x : 1.5f, py = std::fmod(y, 40.0f);
        if (std::isnan(py)) py = 2;
        worst[4] = std::fmax(worst[4], ulps(enaga_host_powf(px, py), std::pow(double(px), double(py))));
        worst[5] = std::fmax(worst[5], ulps(enaga_host_powf(x, y), std::pow(double(x), double(y))));
    }
    near("atan2f ulps", worst[0], 0, 1);
    near("hypotf ulps", worst[1], 0, 1);
    check(worst[2] == 0, "fmodf exact", worst[2], 0);
    check(worst[3] == 0, "fminf and fmaxf", worst[3], 0);
    near("powf ulps, ordinary", worst[4], 0, 1);
    near("powf ulps, any", worst[5], 0, 1);
    for (float x : {0.0f, -0.0f, 1.0f, -1.0f, 2.0f, -2.0f, 0.5f, INFINITY, -INFINITY, NAN, 3.0f, -3.0f})
        for (float y : {0.0f, -0.0f, 1.0f, -1.0f, 2.0f, 3.0f, -3.0f, 0.5f, -0.5f, INFINITY, -INFINITY, NAN}) {
            float got = enaga_host_powf(x, y), want = powf(x, y);
            bool same = (got == want && std::signbit(got) == std::signbit(want)) || (std::isnan(got) && std::isnan(want));
            if (!same) std::printf("FAIL powf(%g, %g) = %g, want %g\n", x, y, got, want), failures++;
        }
    double sc = 0;
    for (float x = -20; x <= 20; x += 0.0137f) {
        float s, c;
        enaga_host_sincosf(x, &s, &c);
        sc = std::fmax(sc, double(s != enaga_host_sinf(x) || c != enaga_host_cosf(x)));
    }
    check(sc == 0, "sincosf = sinf, cosf", sc, 0);
    check(enaga_host_copysignf(2, -0.0f) == -2 && enaga_host_fdimf(5, 3) == 2 && enaga_host_fdimf(3, 5) == 0,
          "copysignf, fdimf", enaga_host_copysignf(2, -0.0f), -2);
    check(enaga_host_lroundf(2.5f) == 3 && enaga_host_lroundf(-2.5f) == -3 && enaga_host_lrintf(2.5f) == 2 &&
          enaga_host_lrintf(3.5f) == 4, "lroundf, lrintf", enaga_host_lrintf(2.5f), 2);
    for (int i = 0; i < 100000; i++) {
        float x = any_float(), w1, w2;
        int e1, e2, n = int(libm_seed % 600) - 300;
        float f1 = enaga_host_frexpf(x, &e1), f2 = frexpf(x, &e2);
        float m1 = enaga_host_modff(x, &w1), m2 = modff(x, &w2);
        float l1 = enaga_host_ldexpf(x, n), l2 = ldexpf(x, n);
        auto same = [](float a, float b) { return (a == b && std::signbit(a) == std::signbit(b)) || (std::isnan(a) && std::isnan(b)); };
        if (!same(f1, f2) || (e1 != e2 && std::isfinite(x)) || !same(m1, m2) || !same(w1, w2) || !same(l1, l2)) {
            std::printf("FAIL frexpf, modff or ldexpf at %.9g (n %d)\n", x, n);
            failures++;
            break;
        }
    }
}

// The queue, voice allocation and the note stack.
static void polyphony() {
    enaga::Queue<int, 4> q;
    int x = 0;
    check(q.empty() && !q.pop(x), "queue starts empty", q.size(), 0);
    for (int i = 0; i < 4; i++) check(q.push(i * 10), "queue push", i, 1);
    check(!q.push(99) && q.size() == 4, "queue full", q.size(), 4);
    for (int round = 0; round < 10; round++) {          // wraps its indices many times over
        check(q.pop(x) && x == round * 10, "queue order", x, round * 10);
        check(q.push((round + 4) * 10), "queue push after pop", round, 1);
    }
    while (q.pop(x)) {}
    check(q.empty(), "queue drained", q.size(), 0);

    VoiceAllocator<3> a;
    bool sounding[3] = {};
    auto pick = [&] { return a.allocate([&](int v) { return sounding[v]; }); };
    int v0 = pick(); a.start(v0, 10); sounding[v0] = true;
    int v1 = pick(); a.start(v1, 11); sounding[v1] = true;
    int v2 = pick(); a.start(v2, 12); sounding[v2] = true;
    check(v0 == 0 && v1 == 1 && v2 == 2, "silent voices first", v2, 2);
    check(pick() == 0, "all held: the oldest is stolen", pick(), 0);
    int released = 0;
    a.release(11, [&](int v) { released += v + 1; });
    check(released == 2 && !a.held(1) && a.held(0), "release by key", released, 2);
    check(pick() == 1, "the released voice before held ones", pick(), 1);
    a.release(10, [](int) {});
    check(pick() == 0, "the oldest released", pick(), 0);
    sounding[2] = false;
    check(pick() == 2, "a silent voice before released ones", pick(), 2);
    a.start(1, 20);
    a.start(2, 20);
    int count = 0;
    a.each_held(20, [&](int) { count++; });
    check(count == 2 && a.key(1) == 20, "one key holding two voices (unison)", count, 2);
    uint32_t age = a.age(1);
    a.start(1, 21, false);
    check(a.age(1) == age && a.key(1) == 21, "legato keeps the age", a.age(1), age);
    check(a.release_all([](int) {}) == 2, "release_all", 0, 2);

    NoteStack<3> s;
    s.push(1, 60, 100); s.push(2, 62, 100); s.push(3, 64, 100);
    check(s.top().note == 64 && s.size() == 3, "stack top", s.top().note, 64);
    s.push(4, 65, 100);
    check(s.size() == 3 && s[0].key == 2, "full: the oldest goes", s[0].key, 2);
    check(s.remove(4) && s.top().note == 64, "release the top: back to the one below", s.top().note, 64);
    check(!s.remove(9), "a key not held", 0, 0);
    s.push(2, 62, 90);
    check(s.size() == 2 && s.top().key == 2 && s[0].key == 3, "a key played again moves to the top", s[0].key, 3);
}

// The sample player against what it should play.
static void sample_player() {
    Sample mono;
    check(mono.allocate(100, 1, 48000), "sample allocate", 0, 1);
    for (uint32_t i = 0; i < 100; i++) mono.data()[i] = float(i);   // a ramp: each frame's value is its index
    SamplePlayer p;
    std::vector<float> l(256), r(256);
    p.start(mono);
    int made = p.render(l.data(), nullptr, 256, 1.0f);
    bool exact = made == 100 && !p.active();
    for (int i = 0; i < 100; i++) exact = exact && l[i] == float(i);
    for (int i = 100; i < 256; i++) exact = exact && l[i] == 0;
    check(exact, "rate 1 plays every frame exactly, then zeros", made, 100);

    p.start(mono);
    made = p.render(l.data(), nullptr, 256, 0.25f);
    double worst = 0;
    for (int i = 4; i < 380 / 4 * 4 && i < 256; i++) worst = std::fmax(worst, std::fabs(l[i] - 0.25 * i));
    near("a ramp interpolated at a quarter rate", worst, 0, 1e-5);
    check(made == 256 && p.active(), "a quarter rate is still playing", made, 256);

    p.start(mono, 10, 20, true);
    made = p.render(l.data(), nullptr, 16, 1.0f);
    exact = made == 10;
    for (int i = 0; i < 10; i++) exact = exact && l[i] == float(19 - i);
    check(exact, "reverse from frame 19 down to 10", made, 10);

    p.start(mono);
    p.set_loop(SamplePlayer::forward, 40, 50);
    made = p.render(l.data(), nullptr, 256, 1.0f);
    exact = made == 256 && p.active();
    for (int i = 0; i < 256; i++) exact = exact && l[i] == float(i < 50 ? i : 40 + (i - 50) % 10);
    check(exact, "a forward loop", l[60], 40);
    p.set_loop(SamplePlayer::off);
    made = p.render(l.data(), nullptr, 256, 1.0f);
    check(made < 100 && !p.active() && l[made - 1] == 99, "loop off at release: plays out to the end", made, 0);

    p.start(mono, 30, 0);
    p.set_loop(SamplePlayer::ping_pong, 40, 45);
    p.render(l.data(), nullptr, 40, 1.0f);
    const float bounce[] = {30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 43, 42, 41, 40, 41, 42, 43, 44, 43};
    exact = true;
    for (int i = 0; i < 24; i++) exact = exact && l[i] == bounce[i];
    check(exact, "ping-pong between 40 and 44", l[16], 42);

    for (float rate : {3.0f, 11.0f}) {                  // steps that cross the loop's ends once or more
        p.start(mono, 40, 0);
        p.set_loop(SamplePlayer::ping_pong, 40, 45);    // frames 40-44: a fold every 4, a period of 8
        p.render(l.data(), nullptr, 64, rate);
        exact = true;
        for (int i = 0; i < 64; i++) {
            int u = int(rate) * i % 8;
            exact = exact && l[i] == float(40 + (u <= 4 ? u : 8 - u));
        }
        check(exact, "ping-pong with a step longer than the loop", rate, 0);
    }
    p.start(mono, 40, 0);
    p.set_loop(SamplePlayer::ping_pong, 40, 42);        // two frames at twice the rate: always the first
    p.render(l.data(), nullptr, 16, 2.0f);
    check(std::count(l.begin(), l.begin() + 16, 40.0f) == 16, "ping-pong over two frames at rate 2", l[1], 40);
    p.start(mono, 0, 100, true);
    p.set_loop(SamplePlayer::forward, 40, 50);
    p.render(l.data(), nullptr, 8, 23.0f);
    const float wrapped[] = {99, 76, 53, 40, 47, 44, 41, 48};
    exact = true;
    for (int i = 0; i < 8; i++) exact = exact && l[i] == wrapped[i];
    check(exact, "a reversed forward loop with a step longer than the loop", l[4], 47);

    Sample cycle;                                       // one cycle of a sine, looped: seamless at any rate
    cycle.allocate(64, 1, 48000);
    for (int i = 0; i < 64; i++) cycle.data()[i] = float(std::sin(2 * M_PI * i / 64));
    p.start(cycle);
    p.set_loop(SamplePlayer::forward, 0, 64);
    worst = 0;
    std::vector<float> long_block(2000);
    p.render(long_block.data(), nullptr, 2000, 0.37f);
    for (int i = 0; i < 2000; i++) worst = std::fmax(worst, std::fabs(long_block[i] - std::sin(2 * M_PI * 0.37 * i / 64)));
    near("a looped cycle at rate 0.37: no glitch at the join", worst, 0, 1e-4);

    Sample stereo;
    stereo.allocate(8, 2, 44100);
    for (int i = 0; i < 8; i++) { stereo.data()[2 * i] = float(i); stereo.data()[2 * i + 1] = float(-i); }
    p.start(stereo);
    made = p.render(l.data(), r.data(), 8, 1.0f);
    check(made == 8 && l[5] == 5 && r[5] == -5, "stereo frames", r[5], -5);
    near("rate_for: 44.1 kHz an octave up at 48 kHz", stereo.rate_for(12), 2 * 44100.0 / 48000.0, 1e-5);

    std::vector<float> out_l(64, 0.0f), out_r(64, 0.0f);
    Sample ones;
    ones.allocate(1000, 1, 48000);
    for (uint32_t i = 0; i < 1000; i++) ones.data()[i] = 1.0f;
    p.start(ones);
    p.render_add(out_l.data(), out_r.data(), 64, 1.0f, 0.5f, 0.25f);
    check(out_l[0] == 0.5f && out_r[63] == 0.25f, "render_add: the first block's gains at once", out_l[0], 0.5);
    std::fill(out_l.begin(), out_l.end(), 0.0f);
    std::fill(out_r.begin(), out_r.end(), 0.0f);
    p.render_add(out_l.data(), out_r.data(), 64, 1.0f, 1.0f, 0.25f);
    near("render_add ramps: the first sample moved one step", out_l[0], 0.5 + 0.5 / 64, 1e-6);
    near("render_add ramps: the last sample at the new gain", out_l[63], 1.0, 1e-5);
    near("render_add: an unchanged gain stays", out_r[30], 0.25, 1e-7);

    Sample none;
    check(none.empty() && !none.allocate(0x7FFFFFFF, 2, 48000) && none.empty(), "allocate fails cleanly", 0, 0);
}

// Note names, scales, tempo; the step clock, note-offs, conditions; parameter smoothing.
static void sequencing() {
    using namespace enaga;
    char name[5];
    struct { int note; bool flats; int middle; const char *want; } names[] = {
        {60, false, 4, "C4"}, {61, false, 4, "C#4"}, {61, true, 4, "Db4"}, {0, false, 4, "C-1"}, {127, false, 4, "G9"},
        {69, false, 4, "A4"}, {60, false, 3, "C3"}, {70, true, 4, "Bb4"}, {1, true, 4, "Db-1"}};
    for (auto &n : names) {
        music::note_name(n.note, name, n.flats, n.middle);
        check(std::strcmp(name, n.want) == 0, n.want, n.note, 0);
    }
    check(music::scale_size(music::major) == 7 && music::scale_size(music::minor_pentatonic) == 5 &&
          music::scale_size(music::chromatic) == 12, "scale sizes", music::scale_size(music::major), 7);
    for (const auto &sc : music::scales)
        check(sc.mask & 1, "every scale has its root", sc.mask, 1);
    check(music::in_scale(64, 60, music::major) && !music::in_scale(63, 60, music::major) &&
          music::in_scale(51, 60, music::minor), "in_scale (any octave of the root)", 0, 1);
    check(music::quantize(61, 60, music::major) == 60, "quantize: C# to C (a tie: the lower)", music::quantize(61, 60, music::major), 60);
    check(music::quantize(66, 48, music::major) == 65, "quantize: F# to F", music::quantize(66, 48, music::major), 65);
    check(music::quantize(63, 0, music::major_pentatonic) == 62, "quantize: Eb to D in the pentatonic (tie: lower)",
          music::quantize(63, 0, music::major_pentatonic), 62);
    check(music::quantize(70, 60, music::major) == 69, "quantize: Bb to A (tie)", music::quantize(70, 60, music::major), 69);
    check(music::quantize(70, 60, 0) == 70, "quantize: an empty mask", 0, 70);
    const int c_major[] = {48, 50, 52, 53, 55, 57, 59, 60, 62};
    for (int i = 0; i < 9; i++) check(music::degree(48, music::major, i) == c_major[i], "degree up", music::degree(48, music::major, i), c_major[i]);
    check(music::degree(48, music::major, -1) == 47 && music::degree(48, music::major, -7) == 36 &&
          music::degree(48, music::major, -8) == 35, "degree down", music::degree(48, music::major, -8), 35);
    near("note_hz(69)", music::note_hz(69), 440, 1e-3);
    near("note_hz(60)", music::note_hz(60), 261.6256, 1e-3);
    near("seconds(120, quarter)", music::seconds(120, music::quarter), 0.5, 1e-6);
    near("milliseconds(90, dotted eighth)", music::milliseconds(90, music::dotted(music::eighth)), 500, 1e-3);
    near("frames(120, sixteenth)", music::frames(120, music::sixteenth), 6000, 1e-2);
    near("hz(120, quarter)", music::hz(120, music::quarter), 2, 1e-6);
    near("triplet(eighth)", music::triplet(music::eighth), 1.0 / 3, 1e-7);

    // The step clock: 16ths (24 ticks), 4 steps, straight, then swung.
    seq::StepClock clock;
    clock.set(24, 4, 0.5f);
    std::vector<clock::Tick> ticks;
    for (uint32_t t = 0; t < 200; t++) ticks.push_back({t, t % 64});
    seq::Step steps[32];
    uint32_t n = clock.steps(ticks.data(), uint32_t(ticks.size()), steps, 32);
    bool ok = n == 9;
    for (uint32_t i = 0; i < n && ok; i++)
        ok = steps[i].tick == 24 * i && steps[i].count == i && steps[i].index == i % 4 && steps[i].pass == i / 4 &&
             steps[i].frame == (24 * i) % 64;
    check(ok, "straight 16ths: a step every 24 ticks", n, 9);
    clock.set(24, 16, 0.66f);
    n = clock.steps(ticks.data(), uint32_t(ticks.size()), steps, 32);
    check(n == 9 && steps[1].tick == 32 && steps[2].tick == 48 && steps[3].tick == 80, "66 % swing: odd steps at 32 of 48",
          steps[1].tick, 32);
    check(clock.step_at(31) == 0 && clock.step_at(32) == 1 && clock.step_at(47) == 1 && clock.step_at(48) == 2,
          "step_at with swing", clock.step_at(32), 1);
    check(clock.tick_of(3) == 80 && clock.tick_of(4) == 96, "tick_of", clock.tick_of(3), 80);
    clock::Tick late[] = {{1000, 0}, {1008, 3}, {1009, 5}};     // a locate, swung step at 1000 + 8? 1008 = 21*48
    n = clock.steps(late, 3, steps, 32);
    check(n == 1 && steps[0].tick == 1008 && steps[0].count == 42 && steps[0].index == 10 && steps[0].frame == 3,
          "after a locate", n, 1);

    // Note-offs.
    seq::NoteOffs<3> offs;
    std::vector<int> ended;
    auto off = [&](int ch, int note) { ended.push_back(ch * 1000 + note); };
    offs.add(0, 60, 100, 10, off);
    offs.add(1, 64, 100, 20, off);
    offs.due(109, off);
    check(ended.empty(), "nothing due before its length", int(ended.size()), 0);
    offs.due(110, off);
    check(ended.size() == 1 && ended[0] == 60 && offs.waiting() == 1, "due at its length", ended.size(), 1);
    offs.add(1, 64, 115, 20, off);
    check(ended.size() == 2 && ended[1] == 1064 && offs.waiting() == 1, "the same note again ends the old first", ended.size(), 2);
    offs.due(50, off);
    check(ended.size() == 3 && offs.waiting() == 0, "a locate back ends it", ended.size(), 3);
    offs.add(0, 1, 0, 30, off); offs.add(0, 2, 0, 10, off); offs.add(0, 3, 0, 20, off);
    ended.clear();
    offs.add(0, 4, 0, 40, off);
    check(ended.size() == 1 && ended[0] == 2 && offs.waiting() == 3, "full: the soonest due ends", ended.size(), 1);
    offs.all(off);
    check(ended.size() == 4 && offs.waiting() == 0, "all ends every one", ended.size(), 4);
    offs.add(0, 9, 0xFFFFFFF0u, 0x20, off);
    ended.clear();
    offs.due(0x0F, off);
    check(ended.empty(), "tick numbers wrapping: not yet due", int(ended.size()), 0);
    offs.due(0x10, off);
    check(ended.size() == 1, "tick numbers wrapping: due", int(ended.size()), 1);

    check(seq::every(1, 2, 0) && !seq::every(1, 2, 1) && seq::every(2, 2, 1) && seq::every(4, 4, 7) &&
          !seq::every(4, 4, 4) && !seq::every(1, 0, 0), "every", 0, 1);
    Random random(7);
    int hits = 0;
    for (int i = 0; i < 10000; i++) hits += seq::chance(0.3f, random);
    check(hits > 2800 && hits < 3200 && !seq::chance(0, random), "chance(0.3)", hits, 3000);

    // Smoothing.
    Smoothed s;
    s.init(0.01f, 48000.0f, 1.0f);
    s.set(0.0f);
    int i = 0;
    while (s.value() > 0.01f) { s.next(); i++; }
    check(std::abs(i - 480) <= 2, "smoothed: 1 % of the way left after the time set", i, 480);
    Smoothed a, b;
    a.init(0.005f, 48000.0f, 0.0f);
    b.init(0.005f, 48000.0f, 0.0f);
    a.set(1.0f);
    b.set(1.0f);
    std::vector<float> block(64);
    a.render(block.data(), 64);
    for (int k = 0; k < 64; k++) b.next();
    check(block[63] == b.value() && a.value() == b.value(), "render = next steps", block[63], b.value());
    Smoothed c;
    c.init(0.005f, 48000.0f, 0.0f, 64);
    c.set(1.0f);
    near("advance = a block of steps", c.advance(), b.value(), 1e-5);
    c.jump(0.25f);
    check(c.value() == 0.25f && c.settled(), "jump", c.value(), 0.25);
    Smoothed d, e;                                      // long glides stall short of the target without a snap
    d.init(0.05f, 48000.0f, 0.0f);
    e.init(0.05f, 750.0f, 0.0f, 64);
    d.set(1.0f);
    e.set(1.0f);
    for (int k = 0; k < 48000 && !d.settled(); k++) d.next();
    for (int k = 0; k < 750 && !e.settled(); k++) e.advance();
    check(d.settled() && e.settled(), "smoothed: a glide settles", d.value(), 1);
    for (int k = 0; k < 48000; k++) d.next();
    check(d.value() == 1.0f, "smoothed: and lands on the target", d.value(), 1);
    d.set(0.3f);
    std::vector<float> glide(48000);
    d.render(glide.data(), 48000);
    check(d.value() == 0.3f, "smoothed: render lands on the target too", d.value(), 0.3);
}

static void dynamics() {
    // DC blocker: an offset dies away, the audio band passes, -3 dB at its cutoff; blocks = steps.
    {
        DcBlocker dc(10.0f, kRate);
        float y = 0;
        for (int i = 0; i < 48000; i++) y = dc.step(0.5f);
        near("dc blocker: an offset is gone after 1 s", y, 0, 1e-4);
        DcBlocker a(10.0f, kRate), b(10.0f, kRate), c(10.0f, kRate);
        near("dc blocker: 1 kHz passes (gain)", sine_gain(1000, [&](float x) { return a.step(x); }), 1, 2e-3);
        near("dc blocker: 10 Hz is down 3 dB (gain)", sine_gain(10, [&](float x) { return b.step(x); }), 0.7071,
             0.01);
        std::vector<float> in(1000), block(1000), steps(1000);
        Random random;
        for (float &x : in) x = random.bipolar() + 0.3f;
        c.process(in.data(), block.data(), 1000);
        DcBlocker d(10.0f, kRate);
        for (int i = 0; i < 1000; i++) steps[i] = d.step(in[i]);
        same("dc blocker: block = steps", block, steps);
    }

    // Bitcrusher: on the grid of 2^bits levels, within half a step; held at the lower rate; 24 bits untouched.
    {
        Bitcrusher crusher;
        crusher.set(4, kRate, kRate);
        std::vector<float> in(4800), l(4800), r(4800), mono(4800);
        Random random;
        for (float &x : in) x = 0.9f * random.bipolar();
        l = in; r = in; mono = in;
        crusher.process(l.data(), r.data(), 4800);
        double worst = 0, off_grid = 0;
        for (int i = 0; i < 4800; i++) {
            worst = std::max(worst, double(std::fabs(l[i] - in[i])));
            off_grid += l[i] * 8 != std::floor(l[i] * 8) || l[i] != r[i];
        }
        near("crusher 4 bits: within half a step", worst, 0, 0.0625 + 1e-6);
        near("crusher 4 bits: every value on the 1/8 grid", off_grid, 0, 0);
        Bitcrusher m;
        m.set(4, kRate, kRate);
        m.process(mono.data(), nullptr, 4800);
        same("crusher: mono = stereo's left", mono, l);

        Bitcrusher held;
        held.set(24, 12000, kRate);
        l = in;
        held.process(l.data(), nullptr, 4800);
        int changes = 0, wrong = 0;
        for (int i = 0; i < 4800; i++) {
            if (i && l[i] != l[i - 1]) changes++;
            if ((i + 1) % 4 == 0 && std::fabs(l[i] - in[i]) > 1e-6f) wrong++;   // taken every fourth sample
        }
        near("crusher at 12 kHz: 1200 samples taken", changes, 1199, 1);
        near("crusher at 12 kHz: each the input then", wrong, 0, 0);
    }

    // Compressor, on a steady level (a constant: peak detection sees it whole).
    {
        auto settle = [](Compressor &c, float level, int n = 48000) {
            std::vector<float> l(n, level), r(n, level);
            for (int i = 0; i < n; i += 64) c.process(&l[i], &r[i], std::min(64, n - i));
            return l.back();
        };
        Compressor c(kRate);
        c.set(-20, 4, 0.001f, 0.05f, 0);
        const double over = 20 + 20 * std::log10(0.5);                        // 0.5 is 13.98 dB over
        near("compressor: at 4:1, a quarter as far over (dB)", enaga_gain_to_db(settle(c, 0.5f)), -20 + over / 4,
             0.01);
        near("compressor: its reduction (dB)", c.reduction_db(), over * 0.75, 0.01);
        Compressor below(kRate);
        below.set(-20, 4, 0.001f, 0.05f, 0);
        check(settle(below, 0.05f) == 0.05f, "compressor: below the threshold untouched", settle(below, 0.05f), 0.05);
        Compressor knee(kRate);
        knee.set(-20, 4, 0.001f, 0.05f, 6);
        near("compressor: at the threshold, half way into a 6 dB knee (reduction dB)",
             (settle(knee, 0.1f), knee.reduction_db()), 0.75 * 9 / 12, 0.01);
        Compressor makeup(kRate);
        makeup.set(-20, 4, 0.001f, 0.05f, 0, 6);
        near("compressor: makeup", enaga_gain_to_db(settle(makeup, 0.05f)), enaga_gain_to_db(0.05f) + 6, 0.01);

        // Attack and release: the reduction goes 63 % of the way to its target in the time constant.
        Compressor timed(kRate);
        timed.set(-20, 4, 0.01f, 0.2f, 0);
        settle(timed, 0.01f);
        int i = 0;
        for (; timed.reduction_db() < 0.632f * over * 0.75 && i < 48000; i++) {
            float l = 0.5f, r = 0.5f;
            timed.process(&l, &r, 1);
        }
        near("compressor: attack time constant (s)", i / kRate, 0.01, 0.0003);
        settle(timed, 0.5f);
        for (i = 0; timed.reduction_db() > 0.368f * over * 0.75 && i < 48000; i++) {
            float l = 0.01f, r = 0.01f;
            timed.process(&l, &r, 1);
        }
        near("compressor: release time constant (s)", i / kRate, 0.2, 0.002);

        // A side chain: a quiet pad ducks under a loud key, and is left alone when the key is silent.
        Compressor duck(kRate);
        duck.set(-30, 10, 0.001f, 0.05f, 0);
        std::vector<float> pad(4800, 0.01f), key(4800, 0.5f), quiet(4800, 0.0f);
        for (int k = 0; k < 4800; k += 64) duck.process(&pad[k], nullptr, 64, &key[k]);
        check(pad.back() < 0.01f * 0.2f, "compressor: the side chain ducks", pad.back(), 0.002);
        std::fill(pad.begin(), pad.end(), 0.01f);
        Compressor open(kRate);
        open.set(-30, 10, 0.001f, 0.05f, 0);
        for (int k = 0; k < 4800; k += 64) open.process(&pad[k], nullptr, 64, quiet.data() + k);
        check(pad.back() == 0.01f, "compressor: a silent key leaves it alone", pad.back(), 0.01);

        // Chunking does not change a sample.
        std::vector<float> in(5000), a(5000), b(5000), ar(5000), br(5000);
        Random random;
        for (int k = 0; k < 5000; k++) in[k] = random.bipolar() * (k % 1000 < 500 ? 1.0f : 0.05f);
        a = in; b = in; ar = in; br = in;
        Compressor ca(kRate), cb(kRate);
        ca.set(-20, 4, 0.002f, 0.05f);
        cb.set(-20, 4, 0.002f, 0.05f);
        for (int k = 0; k < 5000; k += 7) ca.process(&a[k], &ar[k], std::min(7, 5000 - k));
        for (int k = 0; k < 5000; k += 100) cb.process(&b[k], &br[k], 100);
        same("compressor: blocks of 7 = blocks of 100", a, b);
    }

    // Limiter: nothing above the ceiling; below it, the signal exactly, latency() late; smooth gain.
    {
        Limiter limiter;
        limiter.init(0.002f, kRate);
        limiter.set(-0.3f, 0.05f);
        near("limiter: latency (samples)", limiter.latency(), 95, 0);
        const float ceiling = enaga_db_to_gain(-0.3f);
        std::vector<float> l(48000), r(48000), inl(48000), inr(48000);
        Random random;
        for (int i = 0; i < 48000; i++) {   // noise with bursts and single spikes, up to 12 dB over
            float burst = (i / 2400) % 2 ? 4.0f : 0.3f;
            inl[i] = burst * random.bipolar() * (i % 997 == 0 ? 3.0f : 1.0f);
            inr[i] = burst * random.bipolar();
        }
        l = inl; r = inr;
        for (int i = 0; i < 48000; i += 64) limiter.process(&l[i], &r[i], 64);
        double peak = 0;
        for (int i = 0; i < 48000; i++) peak = std::max({peak, double(std::fabs(l[i])), double(std::fabs(r[i]))});
        check(peak <= ceiling, "limiter: no sample above the ceiling", peak, ceiling);
        check(peak > 0.99 * ceiling, "limiter: the loud parts reach it", peak, ceiling);
        // The gain each sample got: the output over the input that left then (where the input is not tiny).
        double jump = 0;
        float previous = 1;
        for (int i = 96; i < 48000; i++) {
            const float x = inl[i - 95];
            if (std::fabs(x) < 0.05f) continue;
            const float gain = l[i] / x;
            jump = std::max(jump, double(std::fabs(gain - previous)));
            previous = gain;
        }
        check(jump < 0.05, "limiter: the gain moves smoothly", jump, 0.05);

        Limiter quiet;
        quiet.init(0.002f, kRate);
        quiet.set(-0.3f, 0.05f);
        std::vector<float> q(4800), in(4800);
        for (float &x : in) x = 0.5f * random.bipolar();
        q = in;
        for (int i = 0; i < 4800; i += 64) quiet.process(&q[i], nullptr, 64);
        int differ = 0;
        for (int i = 95; i < 4800; i++) differ += q[i] != in[i - 95];
        check(differ == 0 && quiet.reduction_db() == 0, "limiter: below the ceiling, the input exactly, delayed",
              differ, 0);

        // A sine 6 dB over: limited to the ceiling, and recovering after it within the release.
        Limiter sine;
        sine.init(0.002f, kRate);
        sine.set(0.0f, 0.02f);
        std::vector<float> s(48000);
        for (int i = 0; i < 48000; i++) s[i] = (i < 24000 ? 2.0f : 0.5f) * std::sin(2 * M_PI * 100 * i / kRate);
        std::vector<float> in_s = s;
        for (int i = 0; i < 48000; i += 64) sine.process(&s[i], nullptr, 64);
        double held = 0, after = 0;
        for (int i = 12000; i < 24000; i++) held = std::max(held, double(std::fabs(s[i])));
        for (int i = 36000; i < 48000; i++) after = std::max(after, double(std::fabs(s[i])));
        near("limiter: a sine 6 dB over comes out at the ceiling", held, 1.0, 0.01);
        near("limiter: back to unity after the release", after, 0.5, 0.005);
        int changed = 0;
        for (int i = 40000; i < 48000; i++) changed += s[i] != in_s[i - int(sine.latency())];
        check(changed == 0, "limiter: after the release, the input exactly again", changed, 0);

        // A lone spike 20 dB over after silence, at look-aheads down to the least: at the ceiling, never over.
        for (float lookahead : {0.0f, 0.0005f, 0.004f, 0.1f}) {
            Limiter spike;
            spike.init(lookahead, kRate);
            spike.set(-1.0f, 0.1f);
            std::vector<float> x(6144);
            x[1000] = 10.0f;
            x[1001] = -3.0f;
            for (int i = 0; i < 6144; i += 64) spike.process(&x[i], nullptr, 64);
            const float c = enaga_db_to_gain(-1.0f), out = x[1000 + spike.latency()];
            check(out <= c && out > 0.999f * c, "limiter: a spike leaves at the ceiling", out, c);
        }

        // Blocks of any size give the same samples; mono = stereo's left when the right is silent.
        Limiter la, lb, lm;
        la.init(0.0015f, kRate);
        lb.init(0.0015f, kRate);
        lm.init(0.0015f, kRate);
        std::vector<float> a = inl, b = inl, m = inl, zero_a(48000), zero_b(48000);
        for (int i = 0; i < 48000; i += 5) la.process(&a[i], &zero_a[i], std::min(5, 48000 - i));
        for (int i = 0; i < 48000; i += 128) lb.process(&b[i], &zero_b[i], 128);
        for (int i = 0; i < 48000; i += 64) lm.process(&m[i], nullptr, 64);
        same("limiter: blocks of 5 = blocks of 128", a, b);
        same("limiter: mono = stereo's left", m, b);
    }
}

int main() {
    dynamics();
    sequencing();
    sample_player();
    polyphony();
    libm();
    samples();
    math();
    oscillators();
    filters();
    modulation();
    effects();
    blocks();
    effects_against_reference();
    building_blocks();
    synth_blocks();
    if (!failures) std::printf("ok\n");
    return failures ? 1 : 0;
}
