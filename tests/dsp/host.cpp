// The DSP library (include/enaga/dsp) on the host: accuracy of the math, oscillator pitch, filter response,
// envelope times, delay time and reverb decay, and the block functions (runtime/dsp) giving exactly what the
// per-sample steps give. Run by tests/test_dsp.py; prints one line per failed check and
// exits non-zero if any failed.
#include <enaga/dsp.hpp>

#include <cmath>
#include <cstdio>
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

int main() {
    math();
    oscillators();
    filters();
    modulation();
    effects();
    blocks();
    if (!failures) std::printf("ok\n");
    return failures ? 1 : 0;
}
