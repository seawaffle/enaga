// Enaga DSP: polyphony bookkeeping, for the audio context. Unlike the rest of the library these are plain C++ (no
// C state), and they hold no sound: the app keeps its voices in its own array, and these say which to use.
//
//     enaga::dsp::VoiceAllocator<8> voices_;               // with Voice voice_[8] of the app's own
//     // note on (key: whoever played it, a pad, a MIDI note; the note off names the same key):
//     int i = voices_.allocate([&](int v) { return voice_[v].amp.active(); });
//     voices_.start(i, key);
//     voice_[i].start(note, velocity);
//     // note off:
//     voices_.release(key, [&](int v) { voice_[v].amp.gate_off(); });
//
//     enaga::dsp::NoteStack<16> held_;                     // monophonic: the keys held, newest on top
#pragma once

#include <cstdint>

namespace enaga::dsp {

// Chooses voices for notes among N: a silent voice if there is one, else the oldest released, else the oldest
// held (stealing it). Keys are any number from 0 to 254 that tells notes apart.
template <int N> class VoiceAllocator {
public:
    static constexpr int size = N;
    static constexpr uint8_t no_key = 0xFF;

    VoiceAllocator() { reset(); }

    // The voice to play the next note on: `sounding(v)` says whether voice v still makes sound (its envelope is
    // active). A silent voice first, then the oldest released, then the oldest held. Marks nothing: call start.
    template <class Sounding> int allocate(Sounding sounding) const {
        int best = -1;
        for (int v = 0; v < N; v++)
            if (!sounding(v)) return v;
        for (int v = 0; v < N; v++)
            if (!gate_[v] && (best < 0 || age_[v] < age_[best])) best = v;
        if (best >= 0) return best;
        for (int v = 0; v < N; v++)
            if (best < 0 || age_[v] < age_[best]) best = v;
        return best;
    }

    // Voice v is held by key from now. It becomes the newest unless retrigger is false (legato: the same note
    // slides on, keeping its age).
    void start(int v, uint8_t key, bool retrigger = true) {
        key_[v] = key;
        gate_[v] = true;
        if (retrigger) age_[v] = ++clock_;
    }

    // Releases every voice key holds, calling `f(v)` for each (to start its envelope's release); the count.
    template <class F> int release(uint8_t key, F f) {
        int count = 0;
        for (int v = 0; v < N; v++)
            if (gate_[v] && key_[v] == key) {
                gate_[v] = false;
                f(v);
                count++;
            }
        return count;
    }
    template <class F> int release_all(F f) {
        int count = 0;
        for (int v = 0; v < N; v++)
            if (gate_[v]) {
                gate_[v] = false;
                f(v);
                count++;
            }
        return count;
    }
    void release_voice(int v) { gate_[v] = false; }

    // Calls `f(v)` for each voice key holds (pressure, a pitch change for that note).
    template <class F> void each_held(uint8_t key, F f) const {
        for (int v = 0; v < N; v++)
            if (gate_[v] && key_[v] == key) f(v);
    }

    bool held(int v) const { return gate_[v]; }
    uint8_t key(int v) const { return key_[v]; }          // the last key to start it, held or not
    uint32_t age(int v) const { return age_[v]; }         // larger is newer
    void reset() {                                        // all released and equally old
        for (int v = 0; v < N; v++) {
            gate_[v] = false;
            key_[v] = no_key;
            age_[v] = 0;
        }
        clock_ = 0;
    }

private:
    uint8_t key_[N] = {};
    bool gate_[N] = {};
    uint32_t age_[N] = {};
    uint32_t clock_ = 0;
};

// The keys held down, newest on top, for monophonic and legato playing: on a release, go back to the note still
// held below (last-note priority). When full, a new key pushes out the oldest.
template <int N> class NoteStack {
public:
    struct Note { uint8_t key, note, velocity; };

    void push(uint8_t key, uint8_t note, uint8_t velocity) {
        remove(key);                                      // a key played again moves to the top
        if (count_ == N) {
            for (int i = 1; i < N; i++) notes_[i - 1] = notes_[i];
            count_--;
        }
        notes_[count_++] = {key, note, velocity};
    }
    // Takes key off the stack; true if it was there.
    bool remove(uint8_t key) {
        int i = 0;
        while (i < count_ && notes_[i].key != key) i++;
        if (i == count_) return false;
        for (; i + 1 < count_; i++) notes_[i] = notes_[i + 1];
        count_--;
        return true;
    }
    bool empty() const { return count_ == 0; }
    int size() const { return count_; }
    const Note &top() const { return notes_[count_ - 1]; }   // the newest; only when not empty
    const Note &operator[](int i) const { return notes_[i]; }   // 0 is the oldest
    void clear() { count_ = 0; }

private:
    Note notes_[N] = {};
    int count_ = 0;
};

}  // namespace enaga::dsp
