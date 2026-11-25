#include "engine.hpp"

namespace sampler {

void Engine::init() {
    limiter_.init(0.002f, float(ENAGA_SAMPLE_RATE));        // 2 ms look-ahead
    limiter_.set(-0.3f, 0.15f);
}

bool Engine::play(int pad, float velocity, const Settings &settings) {
    if (pad < 0 || pad >= pads) return false;
    return events_.push({Event::play, uint8_t(pad), velocity, settings, 0});
}

bool Engine::silence(int pad) {
    if (pad < 0 || pad >= pads) return false;
    if (!events_.push({Event::silence, uint8_t(pad), 0.0f, {}, requested_ + 1})) return false;
    requested_++;
    return true;
}

void Engine::start(const Event &e) {
    const enaga::dsp::Sample &sample = samples_[e.pad];
    uint32_t start = e.settings.start < sample.frames() ? e.settings.start : sample.frames();
    uint32_t end = e.settings.end < sample.frames() ? e.settings.end : sample.frames();
    if (start == end) return;                               // nothing to play (an empty sample, say)
    // A voice: a silent one, else the one that started longest ago (pads are one-shots: none is ever "held").
    int v = allocator_.allocate([&](int i) { return voices_[i].player.active(); });
    allocator_.start(v, e.pad);
    allocator_.release(e.pad, [](int) {});
    bool reverse = end < start;
    if (reverse) {
        uint32_t t = start;
        start = end;
        end = t;
    }
    Voice &voice = voices_[v];
    voice.player.start(sample, start, end, reverse);
    voice.pad = e.pad;
    voice.rate = sample.rate_for(e.settings.pitch, float(ENAGA_SAMPLE_RATE));
    voice.gain = e.settings.level * e.velocity;
}

void Engine::render(float *left, float *right, int n) {
    Event e;
    while (events_.pop(e)) {
        if (e.type == Event::play) {
            start(e);
        } else {
            for (Voice &voice : voices_)
                if (voice.pad == e.pad) voice.player.stop();
            done_ = e.ticket;
        }
    }
    for (int i = 0; i < n; i++) left[i] = right[i] = 0.0f;
    for (Voice &voice : voices_)
        if (voice.player.active()) voice.player.render_add(left, right, n, voice.rate, voice.gain, voice.gain);
    limiter_.process(left, right, n);
}

}  // namespace sampler
