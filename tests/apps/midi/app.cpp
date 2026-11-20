// MIDI test app. Logs every message as "M<status> <data1> <data2>" in hex (unused data bytes 00) and every SysEx
// chunk as "S<bytes> <flags>" (1 start, 2 end, 4 cut: the raw enaga_sysex_flags rebuilt from start / end and the
// last byte). Sends from the STEP keys:
//   STEP 1  note on, key 60, velocity 100, on the user's MIDI channel
//   STEP 2  a 300-byte SysEx (F0, 298 data bytes 0..127 repeating, F7), then a clock byte at once
//   STEP 3  every helper once: note off, poly pressure, CC, program, channel pressure, pitch bend, SPP, song select
#include <enaga/app.hpp>
#include <enaga/midi.hpp>

namespace {
char hex(int v) { return "0123456789ABCDEF"[v & 15]; }
}

class Midi : public enaga::App {
public:
    void on_midi(const enaga::MidiMessage &m) override {
        char t[12] = {'M', hex(m.status >> 4), hex(m.status), ' ', hex(m.data1 >> 4), hex(m.data1), ' ',
                      hex(m.data2 >> 4), hex(m.data2), 0};
        enaga::log(t);
    }
    void on_midi_sysex(int port, const uint8_t *data, uint32_t bytes, bool start, bool end) override {
        (void)port;
        int flags = (start ? 1 : 0) | (end ? 2 : 0) | (end && data[bytes - 1] != 0xF7 ? 4 : 0);
        char t[12] = {'S'};
        int n = 1;
        if (bytes >= 100) t[n++] = char('0' + bytes / 100);
        if (bytes >= 10) t[n++] = char('0' + bytes / 10 % 10);
        t[n++] = char('0' + bytes % 10);
        t[n++] = ' ';
        t[n++] = char('0' + flags);
        t[n] = 0;
        enaga::log(t);
    }
    void on_key(int key, bool down) override {
        if (!down) return;
        int ch = enaga::midi::channel();
        if (key == ENAGA_KEY_STEP1) enaga::midi::note_on(ch, 60, 100);
        if (key == ENAGA_KEY_STEP1 + 1) {
            static uint8_t sysex[300];
            sysex[0] = 0xF0;
            for (int i = 1; i < 299; i++) sysex[i] = uint8_t((i - 1) & 0x7F);
            sysex[299] = 0xF7;
            enaga::midi::sysex(sysex, sizeof sysex);
            enaga::midi::clock();
        }
        if (key == ENAGA_KEY_STEP1 + 2) {
            enaga::midi::note_off(ch, 60);
            enaga::midi::poly_pressure(ch, 60, 33);
            enaga::midi::control_change(ch, 7, 100);
            enaga::midi::program_change(ch, 5);
            enaga::midi::channel_pressure(ch, 44);
            enaga::midi::pitch_bend(ch, -8192);
            enaga::midi::song_position(300);
            enaga::midi::song_select(3);
        }
    }
};

ENAGA_APP(Midi)
