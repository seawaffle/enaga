// The sampler's sound, apart from its screen and controls: a sample for each of the 15 pads, eight voices that
// play them, and a limiter on the mix. audio() calls render(); the main context talks to it only through a queue
// (play, silence), so neither side waits for the other. tests/engine.cpp tests it on a PC (tools/host_test.py).
#pragma once

#include <cstdint>

#include <enaga/abi.h>
#include <enaga/dsp.hpp>
#include <enaga/queue.hpp>

namespace sampler {

constexpr int pads = 15;
constexpr int voices = 8;

// How a pad plays: frames start to end of its sample, end excluded (both kept inside the sample, so the default
// is all of it; end before start plays backwards from end to start), pitch in semitones, level 0-1.
struct Settings {
    uint32_t start = 0, end = 0xFFFFFFFFu;
    float pitch = 0.0f, level = 0.8f;
};

class Engine {
public:
    // Main context, from init(): the limiter's buffers come from the heap.
    void init();

    // ---- Main context ----
    // Plays a pad (velocity 0-1) with these settings; false if the queue is full. A pad whose sample is empty
    // plays nothing.
    bool play(int pad, float velocity, const Settings &settings);
    // Stops every voice playing the pad, in the next block; false if the queue is full (try again later). Once
    // silenced(), no voice reads the pad's sample until it is played again, and it may be loaded into or
    // released.
    bool silence(int pad);
    bool silenced() const { return done_ == requested_; }   // every silence() so far carried out
    // The pad's sample. Change it (load, release) only while nothing plays it: silence() first, wait for
    // silenced(), and do not play() the pad until it is ready.
    enaga::dsp::Sample &sample(int pad) { return samples_[pad]; }
    const enaga::dsp::Sample &sample(int pad) const { return samples_[pad]; }
    // The limiter's gain reduction in the last block, dB: for a meter.
    float reduction_db() const { return limiter_.reduction_db(); }

    // ---- Audio context ----
    // A block: writes n frames into left and right (cleared first).
    void render(float *left, float *right, int n);

private:
    struct Event {
        enum Type : uint8_t { play, silence } type;
        uint8_t pad;
        float velocity;
        Settings settings;
        uint32_t ticket;                 // a silence's number
    };
    struct Voice {
        enaga::dsp::SamplePlayer player;
        int pad = -1;
        float rate = 1.0f, gain = 0.0f;
    };
    void start(const Event &e);

    enaga::dsp::Sample samples_[pads];
    enaga::Queue<Event, 64> events_;
    Voice voices_[voices];
    enaga::dsp::VoiceAllocator<voices> allocator_;
    enaga::dsp::Limiter limiter_;
    uint32_t requested_ = 0;             // main context: silences asked for
    volatile uint32_t done_ = 0;         // audio context: the number of the last carried out
};

}  // namespace sampler
