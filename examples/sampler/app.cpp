// Sampler: a WAV file on each pad, trimmed, tuned and played. A small example of the parts a sample app is made
// of: loading WAV files in the background (wav::Loader), choosing them (ui::FileBrowser), playing them on a few
// voices (dsp::SamplePlayer, dsp::VoiceAllocator), a limiter on the mix (dsp::Limiter), and showing a sample
// (ui::Waveform). The sound is in engine.cpp, apart from the screen and controls, and is tested on a PC by
// tests/engine.cpp (python3 -B tools/host_test.py examples/sampler).
//
// Playing: each pad plays its sample to the end (a one-shot), its velocity setting the level, up to eight at once.
// Pressing a pad also selects it for editing.
//
// Editing the selected pad: knobs A-D are START and END (where it plays from and to; END before START plays it
// backwards), PITCH (semitones) and LEVEL; pushing a knob puts it back. VALUE zooms the waveform in and out
// around START. F1 loads a WAV from /SAMPLES onto the pad: OK on a file loads it, CLR goes back. Nothing is saved:
// the pads start empty each time.
#include <enaga/app.hpp>
#include <enaga/ui.hpp>
#include <enaga/wav.hpp>

#include "engine.hpp"

namespace {

enum { start_param, end_param, pitch_param, level_param, param_count };

// Values short enough for a column (five characters): START and END in percent of the sample ("12.5"), PITCH
// with its sign.
void percent(int value, char *out) {           // tenths of a percent: 125 is "12.5"
    enaga::ui::Line line;
    line.fixed(value, 1);
    line.copy_to(out, 12);
}
void semitones(int value, char *out) {
    enaga::ui::Line line;
    line.add(value > 0 ? "+" : "").number(value);
    line.copy_to(out, 12);
}

const enaga::ui::Param params[param_count] = {
    {.name = "START", .min = 0, .max = 1000, .initial = 0, .format = percent},
    {.name = "END", .min = 0, .max = 1000, .initial = 1000, .format = percent},
    {.name = "PITCH", .min = -24, .max = 24, .initial = 0, .format = semitones},
    {.name = "LEVEL", .min = 0, .max = 100, .initial = 80},
};

}  // namespace

class Sampler : public enaga::App {
public:
    bool init() override {
        engine_.init();
        for (auto &values : values_)
            for (int p = 0; p < param_count; p++) values[p] = params[p].initial;
        page_.set("PAD 1", params, param_count, values_[0]);
        page_.set_columns(true);
        show_pad_leds();
        return true;
    }

    void update(uint32_t) override {
        // A file chosen: once the engine has stopped the pad's voices, its sample can be loaded into.
        if (waiting_ >= 0 && engine_.silenced()) {
            int pad = waiting_;
            waiting_ = -1;
            if (loader_.load<&Sampler::loaded>(browser_.path(), engine_.sample(pad), this) == ENAGA_FILE_OK)
                loading_ = pad;
            else
                notice_.show("CANNOT LOAD");
        }
    }

    void on_pad_press(int pad, int velocity) override {
        select(pad);
        if (pad != loading_ && pad != waiting_) engine_.play(pad, float(velocity) / 127.0f, settings(pad));
        enaga::pad_led(pad, 0, 4 + unsigned(velocity) * 6 / 127, 0);
    }
    void on_pad_release(int pad) override { show_pad_led(pad); }

    void on_key(int key, bool down) override {
        if (browsing_) {
            enaga::ui::Action a = browser_.key(key, down);
            if (a == enaga::ui::Action::chosen) choose();
            else if (a == enaga::ui::Action::back) browsing_ = false;
            return;
        }
        if (key == ENAGA_KEY_F1 && down && loading_ < 0 && waiting_ < 0) {
            browser_.open("/SAMPLES", ".WAV");
            browsing_ = true;
            return;
        }
        page_.key(key, down);
    }

    void on_encoder(int encoder, int delta) override {
        if (browsing_) {
            browser_.encoder(encoder, delta);
        } else if (encoder == 4) {                  // VALUE: zoom around START
            wave_.zoom(delta, frame(selected_, start_param));
        } else {
            page_.encoder(encoder, delta);
        }
    }

    void draw(enaga::Canvas &canvas) override {
        canvas.clear();
        enaga::ui::Line length;                     // the selected pad's sample: its length in seconds
        const enaga::dsp::Sample &sample = engine_.sample(selected_);
        if (!sample.empty()) length.fixed(int(sample.frames() / (sample.rate() / 100u)), 2).add(" S");
        enaga::ui::status_line(canvas, "SAMPLER", length);
        if (browsing_) {
            browser_.draw(canvas, enaga::ui::body_top, enaga::ui::body_bottom);
            enaga::ui::hint_line(canvas, "OK: LOAD  CLR: BACK");
        } else {
            draw_sample(canvas);
            page_.draw(canvas, 66);
            enaga::ui::hint_line(canvas, "F1: LOAD  VALUE: ZOOM");
        }
        notice_.draw(canvas);
    }

    void audio(const float *, float *out, uint32_t frames) override {
        engine_.render(left_, right_, int(frames));
        for (uint32_t i = 0; i < frames; i++) {
            out[2 * i] = left_[i];
            out[2 * i + 1] = right_[i];
        }
    }

private:
    void select(int pad) {
        if (pad == selected_) return;
        selected_ = pad;
        wave_.set(engine_.sample(pad));
        title_.clear().add("PAD ").number(pad + 1);
        page_.set(title_, params, param_count, values_[pad]);
    }

    // The browser chose a file for the selected pad: stop the pad's voices first (update() loads it after).
    void choose() {
        browsing_ = false;
        if (!engine_.silence(selected_)) {
            notice_.show("BUSY: TRY AGAIN");
            return;
        }
        waiting_ = selected_;
    }

    void loaded(int32_t result) {
        int pad = loading_;
        loading_ = -1;
        if (result <= 0) {
            notice_.show(result == enaga::wav::not_wav ? "NOT A WAV FILE" : result == enaga::wav::no_memory
                                                                                 ? "NO MEMORY"
                                                                                 : "CANNOT LOAD");
            enaga::log("LOAD FAILED");
        } else {
            values_[pad][start_param] = params[start_param].initial;
            values_[pad][end_param] = params[end_param].initial;
            enaga::ui::Line line;
            line.add("LOADED ").number(pad + 1).add(' ').number(result);
            enaga::log(line);
        }
        if (pad == selected_) wave_.set(engine_.sample(pad));
        show_pad_led(pad);
    }

    // A percentage parameter as a frame of the pad's sample.
    uint32_t frame(int pad, int param) const {
        uint32_t frames = engine_.sample(pad).frames(), v = uint32_t(values_[pad][param]);
        return frames / 1000u * v + frames % 1000u * v / 1000u;     // 32-bit: no 64-bit division on the C674x
    }
    sampler::Settings settings(int pad) const {
        sampler::Settings s;
        s.start = frame(pad, start_param);
        s.end = frame(pad, end_param);
        s.pitch = float(values_[pad][pitch_param]);
        s.level = float(values_[pad][level_param]) / 100.0f;
        return s;
    }

    void draw_sample(enaga::Canvas &canvas) {
        const int y = enaga::ui::body_top + 2, h = 50;
        if (loading_ == selected_ || waiting_ == selected_) {
            enaga::ui::Line line;
            line.add("LOADING ").number(int(loader_.progress() * 100.0f)).add('%');
            canvas.text(0, y + 20, line, 9);
            enaga::ui::bar(canvas, 0, y + 32, enaga::ui::screen_width, int(loader_.progress() * 1000.0f), 0, 1000, 2);
            return;
        }
        if (engine_.sample(selected_).empty()) {
            canvas.text(0, y + 20, "EMPTY: F1 LOADS A WAV", 6);
            return;
        }
        wave_.draw(canvas, 0, y, enaga::ui::screen_width, h);
        wave_.mark(canvas, frame(selected_, start_param), 15);
        uint32_t end = frame(selected_, end_param), last = engine_.sample(selected_).frames() - 1;
        wave_.mark(canvas, end < last ? end : last, 12);          // at 100 %, on the last frame
    }

    void show_pad_led(int pad) { enaga::pad_led(pad, 0, 0, engine_.sample(pad).empty() ? 0 : 2); }
    void show_pad_leds() {
        for (int pad = 0; pad < sampler::pads; pad++) show_pad_led(pad);
    }

    sampler::Engine engine_;
    enaga::wav::Loader loader_;
    enaga::ui::FileBrowser browser_;
    enaga::ui::Waveform wave_;
    enaga::ui::ParamPage page_;
    enaga::ui::Notice notice_;
    enaga::ui::Line title_{"PAD 1"};
    int values_[sampler::pads][param_count] = {};
    int selected_ = 0;
    int waiting_ = -1, loading_ = -1;          // the pad waiting for its voices to stop, and the one loading
    bool browsing_ = false;
    float left_[ENAGA_BLOCK_FRAMES] = {}, right_[ENAGA_BLOCK_FRAMES] = {};
};

ENAGA_APP(Sampler)
