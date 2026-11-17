// The synth's sound engine, run in the audio callback.
//
// The main context talks to it two ways: parameter values (`values`, one int16_t write each, read at the start
// of every block) and note events through a single-producer, single-consumer queue. Nothing here allocates
// after init().
#pragma once

#include <enaga/abi.h>
#include <enaga/dsp.hpp>

#include "params.hpp"

namespace synth {

struct NoteEvent {
    enum Type : uint8_t { on, off, pressure, all_off } type;
    uint8_t key;                // who played it: pad 0-14, STEP 16-31; releases and pressure match on it
    uint8_t note;               // MIDI note number
    uint8_t value;              // velocity 1-127, or pressure 0-127
};

class Engine {
public:
    static constexpr int kVoices = 8;
    static constexpr int kControl = 16;           // frames per control step: modulation at 3 kHz
    static constexpr int kScope = 512;

    void init(float sample_rate);

    // Main context.
    volatile int16_t values[param_count] = {};
    bool post(const NoteEvent &event);            // false if the queue is full

    // Audio context: `frames` interleaved stereo frames, a multiple of kControl.
    void render(float *out, uint32_t frames);

    // Written by the audio context for display.
    volatile uint32_t active_voices = 0;
    float scope[kScope] = {};
    volatile uint32_t scope_write = 0;

private:
    struct Voice {
        uint8_t key = 0xFF, note = 0;
        bool gate = false;
        uint32_t started = 0;
        float velocity = 0, pressure = 0, random = 0, pitch = 60, target = 60;
        float lfo_rate[2] = {1, 1};
        enaga::dsp::Oscillator osc1, osc2, sub;
        enaga::dsp::Svf svf1, svf2;
        enaga::dsp::Envelope amp, env2, env3;
        enaga::dsp::Lfo lfo1, lfo2;
        float gain_l = 0, gain_r = 0;
    };
    struct Control {                              // a voice's control values for one step
        float d[destination_count];               // the matrix's sums per destination
        float dt1, dt2, inv1, inv2;               // oscillator frequencies: cycles per sample, and 1 / that
        enaga::dsp::SvfCoefficients c1, c2;       // c1: LP24's first stage
        float target_l, target_r;
    };
    struct Slot { uint8_t source, destination; float amount; };
    struct Settings {
        enaga::dsp::Wave wave1, wave2;
        float width1, width2, offset1, offset2;   // offset: semitones
        float level1, level2, level_sub, level_noise;
        uint8_t filter_type;
        float cutoff, resonance, drive, filter_env, filter_key, filter_velocity;   // cutoff as a note number
        enaga::dsp::EnvelopeShape amp, env2, env3;
        enaga::dsp::LfoShape lfo_shape[2];
        float lfo_rate[2];
        bool lfo_key_sync[2];
        Slot slots[mod_slots];
        int slot_count;
        uint8_t mode;
        float glide, velocity_amp, volume;
    };

    void update_settings();
    void handle(const NoteEvent &event);
    void start(Voice &voice, const NoteEvent &event, bool retrigger);
    Voice &allocate();
    void control_step();
    void render_voice(Voice &voice, const Control &control, float *left, float *right);

    float rate_ = 48000.0f, inv_rate_ = 1.0f / 48000.0f;
    Settings s_{};
    int16_t applied_[param_count] = {};
    bool first_ = true;
    Voice voices_[kVoices];
    Control control_[kVoices];
    uint32_t clock_ = 0;
    float last_pitch_ = 60;
    enaga::dsp::Random random_;
    enaga::dsp::Lfo free_lfo_[2];                  // the phase FREE voices take at note-on

    struct Held { uint8_t key, note, velocity; };  // MONO and LEGATO: the keys held, last on top
    Held held_[16];
    int held_count_ = 0;

    NoteEvent queue_[64];
    volatile uint32_t queue_write_ = 0, queue_read_ = 0;

    enaga::dsp::StereoDelay delay_;
    enaga::dsp::Reverb reverb_;
    float delay_mix_ = 0, delay_send_ = 1, reverb_mix_ = 0;
};

}  // namespace synth
