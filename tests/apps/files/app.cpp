// File service test app (tests/test_files.py). Runs the file API through its cases and writes one line per
// check, "<check> <result>", to RESULTS.TXT in its own folder; the test reads it from the card image afterwards.
// The card the test gives it holds /SAMPLES/BIG.RAW (1 MiB, byte i = i * 7 + (i >> 9)) and
// /SAMPLES/ONLYTMP.BIN.TMP (a save cut short: only the new data, under the temporary name).
//
// With /SAMPLES/CRASH on the card the app calls fatal() right after queuing its load instead.
//
// Phases, one or more main-loop passes each: immediate calls; a background load counted in update() passes;
// background writes queued four at a time; a folder listing; a close that cancels queued reads; then, when the
// card file /SAMPLES/PULL exists, a removal and reinsertion driven by the test on the card-detect pin. The log
// shows "FILES PULL" when the app is ready for the card to go and "FILES DONE" once RESULTS.TXT is saved.
#include <enaga/files.hpp>

#include <cstdint>

namespace {
namespace files = enaga::files;

uint8_t pattern(uint32_t i) { return uint8_t(i * 7 + (i >> 9)); }

constexpr uint32_t kBig = 1u << 20;
constexpr uint32_t kWrite = 64 * 1024;          // four of these to /RECORD/OUT.BIN
}  // namespace

class Files : public enaga::App {
public:
    bool init() override {
        big_ = new uint8_t[kBig];
        return big_ != nullptr;
    }

    void audio(const float *, float *out, uint32_t frames) override {
        if (!audio_checked_) {
            files::Info info;
            audio_result_ = files::info("/SAMPLES/BIG.RAW", info);
            audio_checked_ = true;
        }
        for (uint32_t i = 0; i < 2 * frames; i++) out[i] = 0.0f;
    }

    void update(uint32_t) override {
        passes_++;
        switch (phase_) {
        case 0: immediate(); phase_ = 1; break;
        case 1: {
            files::Info info;
            start_load();
            if (files::info("/SAMPLES/CRASH", info) == ENAGA_FILE_OK) enaga::fatal("files crash");   // load pending
            phase_ = 2;
            break;
        }
        case 2: break;                                       // loaded() moves on
        case 3: start_writes(); phase_ = 4; break;
        case 4: break;                                       // written() moves on
        case 5: start_list(); phase_ = 6; break;
        case 6: break;                                       // listed() moves on
        case 7: cancel_on_close(); phase_ = 8; break;
        case 8: break;                                       // cancelled() moves on
        case 9: {
            files::Info info;
            if (files::info("/SAMPLES/PULL", info) == ENAGA_FILE_OK) {
                held_.open("/SAMPLES/BIG.RAW", ENAGA_FILE_READ);
                enaga::log("FILES PULL");
                phase_ = 10;                                 // on_card() moves on
            } else {
                finish();
            }
            break;
        }
        default: break;
        }
    }

    void on_card(bool usable) override {
        if (phase_ == 10 && !usable) {
            check("card_removed", 0);
            check("removed_read_all", files::read_all("/SAMPLES/BIG.RAW", big_, kBig));
            check("removed_size", held_.size());
            check("removed_close", held_.close());
            check("removed_open", files::File().open("/SAMPLES/BIG.RAW", ENAGA_FILE_READ));
            phase_ = 11;
        } else if (phase_ == 11 && usable) {
            check("card_back", 1);
            int32_t got = files::read_all("/SAMPLES/BIG.RAW", big_, kBig);
            check("back_read_all", got == int32_t(kBig) && intact(big_, kBig) ? 1 : got);
            finish();
        }
    }

private:
    void check(const char *name, int32_t value) {
        uint32_t i = 0;
        while (name[i] && used_ < sizeof results_ - 16) results_[used_++] = name[i++];
        results_[used_++] = ' ';
        char digits[12];
        int n = 0;
        uint32_t v = value < 0 ? uint32_t(-value) : uint32_t(value);
        do digits[n++] = char('0' + v % 10); while (v /= 10);
        if (value < 0) results_[used_++] = '-';
        while (n) results_[used_++] = digits[--n];
        results_[used_++] = '\n';
    }

    static bool crashed_marker() {
        files::Info info;
        return files::info("/SAMPLES/CRASH", info) == ENAGA_FILE_OK;
    }
    static bool intact(const uint8_t *data, uint32_t bytes) {
        for (uint32_t i = 0; i < bytes; i++) if (data[i] != pattern(i)) return false;
        return true;
    }

    void immediate() {
        char path[96];
        join(path, files::data_folder(), "/A long patch name.txt");
        check("data_folder_is_files", text_equal(files::data_folder(), "/ENAGA/DATA/FILES"));
        check("save_first", files::save(path, "hello patch", 11));
        char back[32] = {};
        int32_t got = files::read_all(path, back, sizeof back);
        check("read_first", got == 11 && text_equal(back, "hello patch") ? 1 : got);
        check("save_second", files::save(path, "second", 6));
        for (auto &c : back) c = 0;
        got = files::read_all(path, back, sizeof back);
        check("read_second", got == 6 && text_equal(back, "second") ? 1 : got);
        files::Info info;
        check("info_size", files::info(path, info) == ENAGA_FILE_OK ? int32_t(info.size) : -100);
        check("info_folder", files::info(files::data_folder(), info) == ENAGA_FILE_OK ? int32_t(info.flags) : -100);
        check("case_insensitive", files::read_all("/enaga/data/files/a LONG patch NAME.TXT", back, sizeof back));

        check("deny_apps", files::save("/ENAGA/APPS/X.TXT", "x", 1));
        check("deny_other_app", files::save("/ENAGA/DATA/OTHER/X.TXT", "x", 1));
        check("deny_prefix_lookalike", files::save("/ENAGA/DATA/FILESX/X.TXT", "x", 1));
        check("deny_root", files::save("/X.TXT", "x", 1));
        check("deny_area_itself", files::remove("/SAMPLES"));
        check("bad_dotdot", files::save("/SAMPLES/../ENAGA/X.TXT", "x", 1));
        check("bad_dot", files::read_all("/SAMPLES/./BIG.RAW", back, sizeof back));
        check("bad_relative", files::save("SAMPLES/X.TXT", "x", 1));
        check("bad_trailing_slash", files::save("/SAMPLES/", "x", 1));
        check("bad_empty_part", files::read_all("/SAMPLES//BIG.RAW", back, sizeof back));

        {                                     // buffers off word alignment: whole sectors bounce in the driver
            for (uint32_t i = 0; i < sizeof odd_; i++) odd_[i] = pattern(i);
            int32_t saved = files::save("/RECORD/odd.bin", odd_ + 1, 2048);
            got = files::read_all("/RECORD/odd.bin", big_ + 3, 4096);
            bool same = got == 2048;
            for (uint32_t i = 0; same && i < 2048; i++) same = big_[3 + i] == pattern(i + 1);
            check("unaligned", saved < 0 ? saved : same ? 1 : got);
        }
        check("save_shared", files::save("/SAMPLES/new folder/s.raw", "abcd", 4));
        check("read_shared", files::read_all("/SAMPLES/new folder/s.raw", back, sizeof back));
        check("tmp_fallback", files::read_all("/SAMPLES/ONLYTMP.BIN", back, sizeof back));
        check("too_big", files::read_all("/SAMPLES/BIG.RAW", back, sizeof back));
        check("not_found", files::read_all("/SAMPLES/NOPE.BIN", back, sizeof back));
        check("folder_make", files::make_folder("/RECORD/takes/one"));
        check("folder_again", files::make_folder("/RECORD/takes/one"));
        check("folder_remove", files::remove("/RECORD/takes/one"));
        check("remove_missing", files::remove("/RECORD/takes/one"));

        {
            files::File open[ENAGA_FILES_OPEN];
            int32_t worst = 0;
            for (auto &f : open) { int32_t r = f.open("/SAMPLES/BIG.RAW", ENAGA_FILE_READ); if (r < worst) worst = r; }
            check("open_eight", worst);
            files::File ninth;
            check("open_ninth", ninth.open("/SAMPLES/BIG.RAW", ENAGA_FILE_READ));
            check("size", open[0].size());
        }
        {
            files::File w;
            check("open_write", w.open("/RECORD/locked.bin", ENAGA_FILE_WRITE));
            check("save_over_open", files::save("/RECORD/locked.bin", "x", 1));
            check("write_on_read_handle", files::File("/SAMPLES/BIG.RAW", ENAGA_FILE_READ).write("x", 1, nullptr, nullptr));
        }
        check("bad_handle", files::File().size());
    }

    void start_load() {
        for (uint32_t i = 0; i < kBig; i++) big_[i] = 0;
        load_passes_ = passes_;
        check("load_queued", files::load<&Files::loaded>("/SAMPLES/BIG.RAW", big_, kBig, this));
    }
    void loaded(int32_t result) {
        if (crashed_marker()) enaga::log("FILES CALLBACK AFTER DROP");
        check("load_result", result);
        check("load_intact", intact(big_, kBig));
        check("load_passes", int32_t(passes_ - load_passes_));
        phase_ = 3;
    }

    void start_writes() {
        check("write_open", out_.open("/RECORD/OUT.BIN", ENAGA_FILE_WRITE));
        for (int k = 0; k < 4; k++) check("write_queued", out_.write<&Files::written>(big_ + k * kWrite, kWrite, this));
    }
    void written(int32_t result) {
        check("write_result", result);
        if (result == ENAGA_FILE_FULL) full_ = true;
        if (++writes_ == 4) {
            check("write_close", out_.close());
            if (full_) check("full_remove", files::remove("/RECORD/OUT.BIN"));   // room for RESULTS.TXT again
            phase_ = 5;
        }
    }

    void start_list() { check("list_queued", files::list<&Files::entry, &Files::listed>("/SAMPLES", this)); }
    void entry(const files::Info &e) {
        if (text_equal(e.name, "BIG.RAW")) check("list_big", int32_t(e.size));
        if (text_equal(e.name, "new folder")) check("list_folder", int32_t(e.flags));
    }
    void listed(int32_t result) {
        check("list_count", result);
        phase_ = 7;
    }

    void cancel_on_close() {
        files::File f("/SAMPLES/BIG.RAW", ENAGA_FILE_READ);
        for (int k = 0; k < 3; k++) f.read<&Files::cancelled>(big_ + k * 4096, 4096, this);
        check("seek_while_pending", f.seek(0));
    }                                                        // f closes here, with three reads queued
    void cancelled(int32_t result) {
        check("cancelled", result);
        if (++cancels_ == 3) phase_ = 9;
    }

    void finish() {
        check("audio_context", audio_result_);
        int32_t saved = files::save(path_results(), results_, used_);
        enaga::log(saved == ENAGA_FILE_OK ? "FILES DONE" : "FILES SAVE FAILED");
        phase_ = 99;
    }
    const char *path_results() {
        static char path[64];
        join(path, files::data_folder(), "/RESULTS.TXT");
        return path;
    }

    static void join(char *out, const char *a, const char *b) {
        while (*a) *out++ = *a++;
        while (*b) *out++ = *b++;
        *out = 0;
    }
    static bool text_equal(const char *a, const char *b) {
        while (*a && *a == *b) { a++; b++; }
        return *a == *b;
    }

    uint8_t *big_ = nullptr;
    uint8_t odd_[4096] = {};
    char results_[4096] = {};
    uint32_t used_ = 0;
    int phase_ = 0, writes_ = 0, cancels_ = 0;
    bool full_ = false;
    uint32_t passes_ = 0, load_passes_ = 0;
    files::File out_, held_;
    volatile bool audio_checked_ = false;
    volatile int32_t audio_result_ = 0;
};

ENAGA_APP(Files)
