// Clock test app. STEP 1 start, 2 stop, 3 continue, 4 locate to bar 2 (tick 384), 5 tempo 150, logging each call's
// result as "C<step> <0 | -1>". Transport events are logged as "T<what> <position>". Its audio callback takes the
// block's ticks and counts them, with "gaps" for any tick that is not the one after the last (a transport event
// starts a new run). STEP 6 logs "N<ticks> G<gaps> L<last tick>", STEP 7 "B<tempo x 10> R<running>".
#include <enaga/app.hpp>
#include <enaga/clock.hpp>

namespace {
void number(char *t, int &n, uint32_t v) {
    char digits[12];
    int k = 0;
    do { digits[k++] = char('0' + v % 10); v /= 10; } while (v);
    while (k) t[n++] = digits[--k];
}
}  // namespace

class Clock : public enaga::App {
public:
    void on_key(int key, bool down) override {
        int step = key - ENAGA_KEY_STEP1 + 1;
        if (!down || step < 1 || step > 7) return;
        char t[40];
        int n = 0;
        if (step <= 5) {
            bool ok = step == 1 ? enaga::clock::start() : step == 2 ? enaga::clock::stop()
                    : step == 3 ? enaga::clock::continue_() : step == 4 ? enaga::clock::locate(384)
                    : enaga::clock::set_tempo(150.0f);
            t[n++] = 'C'; t[n++] = char('0' + step); t[n++] = ' ';
            if (!ok) t[n++] = '-';
            t[n++] = ok ? '0' : '1';
        } else if (step == 6) {
            t[n++] = 'N'; number(t, n, count_); t[n++] = ' ';
            t[n++] = 'G'; number(t, n, gaps_); t[n++] = ' ';
            t[n++] = 'L'; number(t, n, last_);
        } else {
            enaga::clock::State s = enaga::clock::state();
            t[n++] = 'B'; number(t, n, uint32_t(s.tempo * 10.0f + 0.5f)); t[n++] = ' ';
            t[n++] = 'R'; number(t, n, s.running);
        }
        t[n] = 0;
        enaga::log(t);
    }
    void on_transport(int what, uint32_t position) override {
        fresh_ = true;
        char t[24] = {'T', char('0' + what), ' '};
        int n = 3;
        number(t, n, position);
        t[n] = 0;
        enaga::log(t);
    }
    void audio(const float *in, float *out, uint32_t frames) override {
        (void)in;
        enaga::clock::Tick ticks[8];
        uint32_t n = enaga::clock::ticks(ticks, 8);
        for (uint32_t i = 0; i < n; i++) {
            if (ticks[i].frame >= frames || (i && ticks[i].frame < ticks[i - 1].frame)) gaps_ = gaps_ + 1;
            if (have_last_ && !fresh_ && ticks[i].tick != last_ + 1) gaps_ = gaps_ + 1;
            fresh_ = false;
            have_last_ = true;
            last_ = ticks[i].tick;
            count_ = count_ + 1;
        }
        for (uint32_t i = 0; i < 2 * frames; i++) out[i] = 0.0f;
    }

private:
    volatile uint32_t count_ = 0, gaps_ = 0, last_ = 0;
    volatile bool have_last_ = false, fresh_ = false;
};

ENAGA_APP(Clock)
