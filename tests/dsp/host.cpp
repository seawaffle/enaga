// The DSP library (include/enaga/dsp) on the host: accuracy of the math, oscillator pitch, filter response,
// envelope times, delay time and reverb decay, and the block functions (runtime/dsp) giving exactly what the
// per-sample steps give. Run by tests/test_dsp.py; prints one line per failed check and
// exits non-zero if any failed.
#include <enaga/dsp.hpp>

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <vector>

using namespace enaga::dsp;

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
    for (float t = -3; t <= 3; t += 0.00071f) worst = std::fmax(worst, std::fabs(sin_turns(t) - std::sin(2 * M_PI * t)));
    near("sin_turns absolute error", worst, 0, 4e-6);
    worst = 0;
    for (float x = 0; x <= 0.49f; x += 0.0007f) worst = std::fmax(worst, std::fabs(tan_pi(x) / std::tan(M_PI * x) - 1) * (x > 0));
    near("tan_pi relative error", worst, 0, 1e-4);
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
        current_ = target_ = 0.25f * sample_rate;
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
    // FIR: lengths below, at and past a chunk, against the plain sum; in place too.
    for (int length : {1, 3, 32, 64, 65, 150}) {
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

int main() {
    math();
    oscillators();
    filters();
    modulation();
    effects();
    blocks();
    effects_against_reference();
    building_blocks();
    if (!failures) std::printf("ok\n");
    return failures ? 1 : 0;
}
