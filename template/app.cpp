// My app: a starting point for an Enaga app (docs/getting-started.md). A one-voice synth: the pads and MIDI notes
// play it, and the four knobs set its wave, attack, release and level on a parameter page (<enaga/ui.hpp>).
//
// Two contexts run this code (docs/api.md, "The two contexts"): the main context gets the events and draws; the
// audio context runs audio() every 64 frames. They share a few words below, each written by one side only.
#include <enaga/app.hpp>
#include <enaga/dsp.hpp>
#include <enaga/ui.hpp>

namespace {

using namespace enaga::dsp;

const char *const kWaves[] = {"SAW", "PULSE", "TRI", "SINE"};
const enaga::ui::Param kParams[] = {
    {"WAVE", 0, 3, 0, kWaves},
    {"ATTACK", 0, 2000, 5, nullptr, nullptr, "MS"},
    {"RELEASE", 0, 4000, 400, nullptr, nullptr, "MS"},
    {"LEVEL", 0, 100, 70, nullptr, nullptr, "%"},
};
enum { kWave, kAttack, kRelease, kLevel, kParamCount };

constexpr int kFirstNote = 48;                  // pad 1 plays C3, then up in semitones

}  // namespace

class MyApp : public enaga::App {
public:
    bool init() override {
        for (int i = 0; i < kParamCount; i++) values_[i] = kParams[i].initial;
        page_.set("MY APP", kParams, kParamCount, values_);
        return true;
    }

    // ---- main context: events and drawing ----

    void on_pad_press(int pad, int velocity) override { note_on(kFirstNote + pad, velocity); }
    void on_pad_release(int pad) override { note_off(kFirstNote + pad); }

    void on_midi(const enaga::MidiMessage &m) override {
        if (m.type() == enaga::MidiMessage::note_on) note_on(m.note(), m.velocity());
        else if (m.type() == enaga::MidiMessage::note_off) note_off(m.note());
    }

    void on_key(int key, bool down) override {
        page_.key(key, down);
    }
    void on_encoder(int encoder, int delta) override {
        page_.encoder(encoder, delta);
    }

    void draw(enaga::Canvas &canvas) override {
        canvas.clear();
        page_.draw(canvas, 0);
        canvas.text(0, 120, note_ >= 0 ? "PLAYING" : "PLAY A PAD", note_ >= 0 ? 15 : 5);
    }

    // ---- audio context ----

    void audio(const float *, float *out, uint32_t frames) override {
        const int attack = values_[kAttack], release = values_[kRelease];
        if (attack != shape_attack_ || release != shape_release_) {   // a knob moved: new envelope times
            shape_attack_ = attack;
            shape_release_ = release;
            shape_ = EnvelopeShape::make(float(attack) * 0.001f + 0.001f, 0.1f, 1.0f, float(release) * 0.001f + 0.001f,
                                         float(ENAGA_SAMPLE_RATE));
        }
        if (presses_ != heard_presses_) { heard_presses_ = presses_; envelope_.gate_on(); }
        if (!gate_ && envelope_.stage() != Envelope::release && envelope_.active()) envelope_.gate_off();
        float env[ENAGA_BLOCK_FRAMES], voice[ENAGA_BLOCK_FRAMES] = {};
        envelope_.render(env, frames, shape_);
        oscillator_.render_add(voice, frames, Wave(values_[kWave]), dt_, reciprocal(dt_), level_);
        for (uint32_t i = 0; i < frames; i++) out[2 * i] = out[2 * i + 1] = voice[i] * env[i];
    }

private:
    void note_on(int note, int velocity) {
        note_ = note;
        dt_ = 261.6256f * semitones(float(note - 60)) * (1.0f / float(ENAGA_SAMPLE_RATE));
        level_ = float(values_[kLevel]) * 0.01f * float(velocity) * (1.0f / 127.0f);
        gate_ = true;
        presses_ = presses_ + 1;               // a new note: the audio context restarts the envelope
    }
    void note_off(int note) {
        if (note != note_) return;              // an older note let go: the newer one keeps sounding
        note_ = -1;
        gate_ = false;
    }

    enaga::ui::ParamPage page_;
    int note_ = -1;

    // Shared between the contexts: written by the main context, read by audio(). Each is one word, so audio()
    // never sees half of a change; anything larger (the envelope's shape) audio() builds from these itself.
    int values_[kParamCount] = {};             // the parameter page writes them
    volatile float dt_ = 0.01f, level_ = 0.0f;
    volatile bool gate_ = false;
    volatile uint32_t presses_ = 0;
    // audio() only:
    EnvelopeShape shape_{};
    int shape_attack_ = -1, shape_release_ = -1;
    uint32_t heard_presses_ = 0;
    Oscillator oscillator_;
    Envelope envelope_;
};

ENAGA_APP(MyApp)
