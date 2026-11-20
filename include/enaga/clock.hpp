// The tempo clock and transport: one for the whole unit, in ticks of ENAGA_CLOCK_PPQN (96) to the quarter note.
//
// The user's CLOCK SRC setting says who drives it: the app (INTERNAL), MIDI clock at MIDI IN, or SYNC IN pulses.
// With INTERNAL, start / stop / continue / locate / set_tempo control it; with an outside source they do nothing
// and return false, and on_transport tells the app what the outside clock did. Either way the app follows it in
// its audio callback: ticks() lists the ticks that fall in the block being computed and the frame each falls on,
// so steps land on the right sample. With CLOCK OUT on, Enaga sends MIDI clock and transport for it; with SYNC
// OUT on, pulses at SYNC OUT.
#pragma once

#include <enaga/app.hpp>

#include <cstdint>

namespace enaga::clock {

using Tick = enaga_clock_tick_t;              // .tick (its number), .frame (in the block)
using State = enaga_clock_state_t;            // .running, .source, .tempo (BPM), .position (ticks), .fraction

constexpr uint32_t ppqn = ENAGA_CLOCK_PPQN;
constexpr uint32_t sixteenth = ENAGA_CLOCK_PPQN / 4;

// Main context.
inline bool start() { return host().clock_control(ENAGA_CLOCK_START, 0) == 0; }
inline bool stop() { return host().clock_control(ENAGA_CLOCK_STOP, 0) == 0; }
inline bool continue_() { return host().clock_control(ENAGA_CLOCK_CONTINUE, 0) == 0; }
inline bool locate(uint32_t ticks) { return host().clock_control(ENAGA_CLOCK_LOCATE, int32_t(ticks)) == 0; }
inline bool set_tempo(float bpm) {            // 20-300
    return host().clock_control(ENAGA_CLOCK_TEMPO, int32_t(bpm * 100.0f + 0.5f)) == 0;
}

// Main or audio context (in audio, as of the start of the current block).
inline State state() { State s; host().clock_state(&s); return s; }
inline bool running() { return state().running != 0; }
inline float tempo() { return state().tempo; }
inline uint32_t position() { return state().position; }
inline int source() { return int(state().source); }   // ENAGA_CLOCK_INTERNAL, _MIDI, _SYNC

// Audio context only: this block's ticks, in order; returns how many (a block holds at most one or two at
// ordinary tempos, more when catching up with an outside clock).
inline uint32_t ticks(Tick *out, uint32_t max) { return host().clock_ticks(out, max); }

}  // namespace enaga::clock
