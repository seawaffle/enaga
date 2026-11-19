// Menu test app (tests/test_menu.py): counts its starts in its own settings (enaga::Settings), plays a constant
// level (so a switch's fade shows in the output), STEP 1 opens the Enaga menu itself, STEP 2 makes stop() never
// finish (saving to the card all the while, which feeds the watchdog), STEP 3 says it is busy (AUTO OFF waits).
#include <enaga/app.hpp>
#include <enaga/settings.hpp>

class MenuTest : public enaga::App {
public:
    bool init() override {
        settings_.load();
        settings_.set("STARTS", settings_.get("STARTS", 0) + 1);
        settings_.set("NOTE", "SAVED BY MENU-TEST");
        return settings_.save() == ENAGA_FILE_OK;
    }
    void on_key(int key, bool down) override {
        if (!down) return;
        if (key == ENAGA_KEY_STEP1) enaga::system::open_menu();
        if (key == ENAGA_KEY_STEP1 + 1) slow_stop_ = true;
        if (key == ENAGA_KEY_STEP1 + 2) enaga::system::set_busy(true);
    }
    void stop() override {
        while (slow_stop_) enaga::files::save("/ENAGA/DATA/MENU-TEST/SPIN.TXT", "x", 1);
    }
    void audio(const float *, float *out, uint32_t frames) override {
        for (uint32_t i = 0; i < 2 * frames; i++) out[i] = 0.25f;
    }

private:
    enaga::Settings settings_;
    volatile bool slow_stop_ = false;
};

ENAGA_APP(MenuTest)
