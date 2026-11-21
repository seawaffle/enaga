// MIDI out, and the user's MIDI channel. MIDI in arrives at App::on_midi and App::on_midi_sysex (<enaga/app.hpp>).
//
// Each call queues one whole message for the ports MIDI OUT names (the jack, USB), or nothing if a queue is full
// (false): a message is never cut. A computer that has stopped reading USB is skipped rather than waited for. Real-time messages (clock, start, stop, continue) go ahead of anything
// queued. Main or audio context. Channels are 0-15; data bytes are masked to 7 bits.
#pragma once

#include <enaga/app.hpp>

#include <cstdint>

namespace enaga::midi {

// The user's MIDI CH setting (Enaga menu, SETTINGS), 0-15: the channel an app listens and sends on by default.
inline int channel() { return int(host().midi_channel()); }

// Raw bytes: one or more complete messages, each with its status byte. `ports` 0 = the user's MIDI OUT routing.
inline bool send(const uint8_t *data, uint32_t bytes, uint32_t ports = 0) {
    return host().midi_send(ports, data, bytes) == 0;
}

namespace detail {
inline bool message(int status, int a, int b, uint32_t bytes) {
    const uint8_t m[3] = {uint8_t(status), uint8_t(a & 0x7F), uint8_t(b & 0x7F)};
    return send(m, bytes);
}
}  // namespace detail

inline bool note_on(int ch, int note, int velocity) { return detail::message(0x90 | (ch & 15), note, velocity, 3); }
inline bool note_off(int ch, int note, int velocity = 0) { return detail::message(0x80 | (ch & 15), note, velocity, 3); }
inline bool poly_pressure(int ch, int note, int pressure) { return detail::message(0xA0 | (ch & 15), note, pressure, 3); }
inline bool control_change(int ch, int controller, int value) {
    return detail::message(0xB0 | (ch & 15), controller, value, 3);
}
inline bool program_change(int ch, int program) { return detail::message(0xC0 | (ch & 15), program, 0, 2); }
inline bool channel_pressure(int ch, int pressure) { return detail::message(0xD0 | (ch & 15), pressure, 0, 2); }
inline bool pitch_bend(int ch, int bend) {   // -8192..8191
    int v = bend + 8192;
    v = v < 0 ? 0 : v > 16383 ? 16383 : v;
    return detail::message(0xE0 | (ch & 15), v & 0x7F, v >> 7, 3);
}
inline bool song_position(int beats) { return detail::message(0xF2, beats & 0x7F, beats >> 7, 3); }   // in 16ths
inline bool song_select(int song) { return detail::message(0xF3, song, 0, 2); }
// Real-time: 0xF8 clock, 0xFA start, 0xFB continue, 0xFC stop, 0xFF reset.
inline bool realtime(uint8_t byte) { return send(&byte, 1); }
inline bool clock() { return realtime(0xF8); }
inline bool start() { return realtime(0xFA); }
inline bool continue_() { return realtime(0xFB); }
inline bool stop() { return realtime(0xFC); }
// System Exclusive: `data` is the whole message, F0 ... F7.
inline bool sysex(const uint8_t *data, uint32_t bytes) { return send(data, bytes); }

}  // namespace enaga::midi
