// Enaga: step-sequencer helpers, for the audio context: the clock's ticks into steps (with swing), note lengths
// whose note-offs send themselves (no stuck notes, through stops and locates), and step probability and
// conditions. The pattern itself is the app's: these do the timing.
//
//     enaga::seq::StepClock steps_;                                  // 16ths, 16 steps, 60 % swing
//     enaga::seq::NoteOffs<32> offs_;
//     steps_.set(enaga::clock::sixteenth, 16, 0.6f);
//     // in audio():
//     enaga::clock::Tick ticks[32];
//     uint32_t n = enaga::clock::ticks(ticks, 32);
//     for (uint32_t i = 0; i < n; i++) offs_.due_midi(ticks[i].tick); // note-offs whose time has come
//     enaga::seq::Step due[32];
//     for (uint32_t i = 0, k = steps_.steps(ticks, n, due, 32); i < k; i++) {
//         const Pattern::Step &s = pattern_[due[i].index];
//         if (s.on && enaga::seq::chance(s.probability, random_)) {
//             offs_.add_midi(ch, s.note, due[i].tick, s.length);    // before the note on: ends the same note
//             enaga::midi::note_on(ch, s.note, s.velocity);
//         }
//     }
//     // on_transport(ENAGA_TRANSPORT_STOP): offs_.all_midi();
#pragma once

#include <cstdint>

#include <enaga/clock.hpp>
#include <enaga/dsp/math.hpp>
#include <enaga/midi.hpp>

namespace enaga::seq {

// A step that starts in this block: `index` in the pattern (0 to length - 1), `count` the steps since tick 0,
// `pass` the times the pattern has gone round (count / length), `tick` and `frame` where it falls.
struct Step {
    uint32_t index, count, pass, tick, frame;
};

// Turns the clock's ticks into steps of `ticks_per_step` (clock::sixteenth for 16ths) in a pattern of `length`
// steps, with swing: each odd step (counting from 0) starts `swing` of the way through its pair instead of half
// way, 0.5 straight to 0.75 (the classic 66 % is 0.66). Stateless: the steps follow from the tick numbers, so
// locates and catch-ups come out right. Swing moves a step to the nearest tick (96 to the quarter note).
class StepClock {
public:
    void set(uint32_t ticks_per_step = clock::sixteenth, uint32_t length = 16, float swing = 0.5f) {
        per_step_ = ticks_per_step ? ticks_per_step : 1;
        length_ = length ? length : 1;
        swing = enaga_clamp(swing, 0.5f, 0.75f);
        offset_ = uint32_t(swing * float(2 * per_step_) + 0.5f);
        if (offset_ < 1) offset_ = 1;
        if (offset_ >= 2 * per_step_) offset_ = 2 * per_step_ - 1;
    }

    // The steps starting at any of these ticks (from clock::ticks), into out; how many (at most max).
    uint32_t steps(const clock::Tick *ticks, uint32_t n, Step *out, uint32_t max) const {
        uint32_t made = 0;
        for (uint32_t i = 0; i < n && made < max; i++) {
            uint32_t t = ticks[i].tick, pair = 2 * per_step_, in_pair = t % pair, count;
            if (in_pair == 0) count = t / pair * 2;
            else if (in_pair == offset_) count = t / pair * 2 + 1;
            else continue;
            out[made++] = {count % length_, count, count / length_, t, ticks[i].frame};
        }
        return made;
    }

    // The step sounding at tick (the last to start at or before it), for a playhead on the screen.
    uint32_t step_at(uint32_t tick) const {
        uint32_t pair = 2 * per_step_, count = tick / pair * 2 + (tick % pair >= offset_ ? 1 : 0);
        return count % length_;
    }
    // The tick step `count` starts on.
    uint32_t tick_of(uint32_t count) const { return count / 2 * 2 * per_step_ + (count & 1 ? offset_ : 0); }

    uint32_t ticks_per_step() const { return per_step_; }
    uint32_t length() const { return length_; }

private:
    uint32_t per_step_ = clock::sixteenth, length_ = 16, offset_ = clock::sixteenth;
};

// Notes waiting for their note-off, up to N: add one when its note starts, and due() at each tick ends those whose
// length has run, calling off(channel, note). A note is also ended if time goes back before its start (a locate),
// and all() ends every one (call it when the transport stops). The _midi forms send MIDI note-offs.
template <int N> class NoteOffs {
public:
    // Note `note` on `channel` started at tick `now` and lasts `length` ticks (at least 1). Call it before sending
    // the note on: if the same note is still waiting, it is ended first (so the new one is not cut by the old
    // one's note-off). When all N are waiting, the one due soonest is ended to make room.
    template <class F> void add(int channel, int note, uint32_t now, uint32_t length, F off) {
        int slot = -1;
        for (int i = 0; i < count_; i++)
            if (notes_[i].channel == channel && notes_[i].note == note) {
                off(channel, note);
                slot = i;
            }
        if (slot < 0 && count_ == N) {                     // full: end the soonest due
            slot = 0;
            for (int i = 1; i < N; i++)
                if (int32_t(notes_[i].off - notes_[slot].off) < 0) slot = i;
            off(notes_[slot].channel, notes_[slot].note);
        }
        if (slot < 0) slot = count_++;
        notes_[slot] = {uint8_t(channel), uint8_t(note), now, now + (length ? length : 1)};
    }
    // Ends the notes due at `tick` (their length has run, or tick is before their start).
    template <class F> void due(uint32_t tick, F off) {
        for (int i = 0; i < count_;) {
            const Waiting &w = notes_[i];
            if (int32_t(tick - w.off) >= 0 || int32_t(tick - w.on) < 0) {
                off(w.channel, w.note);
                notes_[i] = notes_[--count_];
            } else {
                i++;
            }
        }
    }
    // Ends every note waiting.
    template <class F> void all(F off) {
        for (int i = 0; i < count_; i++) off(notes_[i].channel, notes_[i].note);
        count_ = 0;
    }
    int waiting() const { return count_; }

    void add_midi(int channel, int note, uint32_t now, uint32_t length) { add(channel, note, now, length, send_off); }
    void due_midi(uint32_t tick) { due(tick, send_off); }
    void all_midi() { all(send_off); }

private:
    struct Waiting {
        uint8_t channel, note;
        uint32_t on, off;
    };
    static void send_off(int channel, int note) { midi::note_off(channel, note); }
    Waiting notes_[N] = {};
    int count_ = 0;
};

// True with the given probability (0 never, 1 always), for a step's chance of playing.
inline bool chance(float probability, dsp::Random &random) { return random.unipolar() < probability; }

// A step condition: true on the a-th of every b passes of the pattern (a 1 to b; Step::pass counts from 0), so
// every(1, 2, pass) plays on the first, third, fifth ... pass and every(4, 4, pass) on every fourth.
constexpr bool every(uint32_t a, uint32_t b, uint32_t pass) { return b && a >= 1 && pass % b == (a - 1) % b; }

}  // namespace enaga::seq
