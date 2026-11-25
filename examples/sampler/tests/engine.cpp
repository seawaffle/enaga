// The sampler's engine on a PC: python3 -B tools/host_test.py examples/sampler
// Each check renders the engine as audio() would and measures what came out; engine.wav is there to listen to.
#include <enaga/testing.hpp>

#include "../engine.hpp"

using namespace enaga::testing;

namespace {

// A sample of `seconds` of a sine at `hz`, amplitude 0.5, mono.
void fill(enaga::dsp::Sample &sample, float hz, float seconds) {
    Buffer tone = sine(hz, frames(seconds), 0.5f);
    sample.allocate(uint32_t(tone.size()), 1, ENAGA_SAMPLE_RATE);
    for (size_t i = 0; i < tone.size(); i++) sample.data()[i] = tone[i];
}

// The last frame that is not silent, or -1.
long last_sound(const Buffer &b) {
    long i = long(b.size()) - 1;
    while (i >= 0 && b[size_t(i)] == 0.0f) i--;
    return i;
}

Buffer run(sampler::Engine &engine, double seconds) {
    return render(frames(seconds), [&](float *left, float *right, int n) { engine.render(left, right, n); });
}

}  // namespace

int main() {
    Checks t;

    sampler::Engine engine;
    engine.init();
    fill(engine.sample(0), 1000, 0.5f);
    fill(engine.sample(1), 250, 1.0f);

    // A pad plays its sample, at its pitch: an octave up is twice the frequency and half as long.
    engine.play(0, 1.0f, {});
    Buffer out = channel(run(engine, 0.7), 0);
    t.near("plays the sample at 1 kHz", frequency(slice(out, 1000, 20000)), 1000, 0.5);
    t.near("at its level (0.8 x 0.5)", peak(out), 0.4, 0.01);
    const long latency = 95;                                     // the limiter's: 2 ms, less one frame
    t.near("for its length (frames)", double(last_sound(out)), 24000 + latency, 4);
    engine.play(0, 1.0f, {.pitch = 12.0f});
    out = channel(run(engine, 0.5), 0);
    t.near("12 semitones up: 2 kHz", frequency(slice(out, 1000, 10000)), 2000, 1);
    t.near("and half as long (frames)", double(last_sound(out)), 12000 + latency, 4);

    // Velocity and level scale it; START and END trim it; END before START plays it backwards.
    engine.play(0, 0.5f, {.level = 0.5f});
    t.near("velocity x level", peak(channel(run(engine, 0.6), 0)), 0.5 * 0.5 * 0.5, 0.01);
    engine.play(0, 1.0f, {.start = 6000, .end = 12000});
    out = channel(run(engine, 0.6), 0);
    long first = first_above(out, 1e-6), last = last_sound(out);
    t.near("start to end: 6000 frames", double(last - first), 6000, 3);
    Buffer forwards = slice(out, size_t(first), size_t(last + 1));
    engine.play(0, 1.0f, {.start = 12000, .end = 6000});
    out = channel(run(engine, 0.6), 0);
    first = first_above(out, 1e-6);
    // Backwards, the first output frame is the forward run's last (frame 11999 of the sample).
    t.near("end before start: backwards", out[size_t(first)], forwards[forwards.size() - 1], 1e-3);

    // Every pad at once, loud: the limiter keeps the mix under -0.3 dB.
    for (int pad = 2; pad < sampler::pads; pad++) fill(engine.sample(pad), 100.0f + 37.0f * float(pad), 0.5f);
    for (int pad = 0; pad < sampler::pads; pad++) engine.play(pad, 1.0f, {.level = 1.0f});
    Buffer loud = run(engine, 0.5);
    t.check("eight voices sound together", rms(slice(loud, 0, 4800)) > 0.2);
    t.below("the limiter holds the mix under -0.3 dB", peak(loud), enaga_db_to_gain(-0.3f) + 1e-6);
    t.above("and is working", engine.reduction_db(), 0.0);
    write_wav(output("engine.wav"), loud);

    // Silencing a pad stops its voices in the next block, and says when.
    engine.play(1, 1.0f, {});
    run(engine, 0.1);
    t.check("silence() is queued", engine.silence(1));
    t.check("not done before a block", !engine.silenced());
    run(engine, 0.01);
    t.check("done after one", engine.silenced());
    t.near("and quiet once the limiter's delay has passed", peak(channel(run(engine, 0.1), 0)), 0, 0);

    // An empty pad plays nothing.
    engine.sample(1).release();
    engine.play(1, 1.0f, {});
    t.near("an empty pad is silent", peak(run(engine, 0.1)), 0, 0);

    return t.finish();
}
