// Enaga DSP: samples in memory and their player, and rate conversion (the C functions in <enaga/dsp.h>). The
// sample format functions (enaga_pcm_to_float, enaga_pcm_to_int16, enaga_int16_to_float, enaga_float_to_int16)
// are called as they are. <enaga/wav.hpp>'s Loader fills a Sample from a WAV file.
//
//     enaga::dsp::Sample kick_;                             // loaded in init(), or by wav::Loader
//     enaga::dsp::SamplePlayer voice_;
//     voice_.start(kick_);                                  // on a pad
//     voice_.render_add(left, right, frames, kick_.rate_for(0.0f), 0.8f, 0.8f);   // in audio()
//
//     enaga::dsp::Resampler left, right;                    // in init(): the table comes from the app heap
//     left.init(wav.rate, ENAGA_SAMPLE_RATE);
//     right.init(left);                                     // the same rates: shares left's table
//     int used, made = left.process(in, n, out, room, used);
#pragma once

#include <enaga/dsp.h>
#include <enaga/dsp/math.hpp>

#include <new>

namespace enaga::dsp {

// A sample in memory: frames of float, mono or stereo interleaved, at a rate. Owns its frames; not copyable.
class Sample {
public:
    Sample() = default;
    Sample(const Sample &) = delete;
    Sample &operator=(const Sample &) = delete;
    ~Sample() { release(); }

    // Room for `frames` frames of `channels` (1 or 2) at `rate` Hz, zeroed; false (and empty) if the heap has no
    // room, rather than stopping the app as new would. Main context.
    bool allocate(uint32_t frames, uint32_t channels, uint32_t rate) {
        release();
        if (channels < 1 || channels > 2 || frames > 0x0FFFFFFFu / channels) return false;   // over 1 GiB
        float *data = new (std::nothrow) float[std::size_t(frames) * channels];
        if (!data) return false;
        for (uint32_t i = 0; i < frames * channels; i++) data[i] = 0.0f;
        state_ = {data, frames, channels, rate};
        return true;
    }
    void release() {
        delete[] state_.data;
        state_ = {nullptr, 0, 1, 48000};
    }

    float *data() { return state_.data; }                 // frame i: data()[i], or [2 i] and [2 i + 1]
    const float *data() const { return state_.data; }
    uint32_t frames() const { return state_.frames; }
    uint32_t channels() const { return state_.channels; }
    uint32_t rate() const { return state_.rate; }
    bool empty() const { return state_.frames == 0; }
    void set_frames(uint32_t frames) { state_.frames = frames < state_.frames ? frames : state_.frames; }   // shortens
    // The player's rate for a pitch offset in semitones, at output rate `out_rate` (0 semitones: the sample as
    // recorded).
    float rate_for(float semitones, float out_rate = 48000.0f) const {
        return enaga_semitones(semitones) * float(state_.rate) * enaga_reciprocal(out_rate);
    }

    enaga_sample state_{nullptr, 0, 1, 48000};
};

// Plays a Sample: enaga_sample_player. One voice; the Sample must outlive its playing.
class SamplePlayer {
public:
    enum Loop { off = ENAGA_LOOP_OFF, forward = ENAGA_LOOP_FORWARD, ping_pong = ENAGA_LOOP_PING_PONG };

    // From the start (or the end, reversed) of the sample, or of frames [start, end); no loop.
    void start(const Sample &sample, bool reverse = false) { start(sample, 0, 0, reverse); }
    void start(const Sample &sample, uint32_t start, uint32_t end, bool reverse = false) {
        enaga_sample_player_start(&state_, &sample.state_, start, end, reverse ? 1 : 0);
    }
    // Loops between frames loop_start and loop_end once the playing reaches them; set_loop(off) at a note's release
    // lets it play out to its end.
    void set_loop(Loop mode, uint32_t loop_start, uint32_t loop_end) {
        state_.loop = mode;
        state_.loop_start = loop_start;
        state_.loop_end = loop_end;
    }
    void set_loop(Loop mode) { state_.loop = mode; }
    void stop() { state_.active = 0; }
    bool active() const { return state_.active != 0; }
    float position() const { return float(state_.position) * (1.0f / 4294967296.0f); }   // in frames

    // enaga_sample_player_render: raw frames (a mono sample into left only), zeros after the end.
    int render(float *left, float *right, int n, float rate) {
        return enaga_sample_player_render(&state_, left, right, n, rate);
    }
    // enaga_sample_player_render_add: mixed in with gains that glide from the last block's.
    int render_add(float *left, float *right, int n, float rate, float gain_l, float gain_r) {
        return enaga_sample_player_render_add(&state_, left, right, n, rate, gain_l, gain_r);
    }

    enaga_sample_player state_{};
};

class Resampler {
public:
    // A conversion from in_rate to out_rate (Hz), from the start of a sound; makes the table (16.6 KB).
    void init(uint32_t in_rate, uint32_t out_rate) {
        float *table = memory_.allocate(enaga_resample_table_size());
        enaga_resample_table(table, in_rate, out_rate);
        enaga_resampler_init(&state_, table, in_rate, out_rate);
        in_rate_ = in_rate;
        out_rate_ = out_rate;
    }
    // Another channel at the same rates, sharing other's table (other must outlive this one).
    void init(const Resampler &other) {
        enaga_resampler_init(&state_, other.state_.table, other.in_rate_, other.out_rate_);
        in_rate_ = other.in_rate_;
        out_rate_ = other.out_rate_;
    }
    // Back to the start of a sound, at the same rates.
    void reset() { enaga_resampler_init(&state_, state_.table, in_rate_, out_rate_); }
    // enaga_resample: returns the outputs written; used is set to the inputs taken.
    int process(const float *in, int n_in, float *out, int max_out, int &used) {
        return enaga_resample(&state_, in, n_in, out, max_out, &used);
    }

    enaga_resampler state_{};

private:
    Memory memory_;
    uint32_t in_rate_ = 0, out_rate_ = 0;
};

}  // namespace enaga::dsp
