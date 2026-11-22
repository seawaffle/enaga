// Battery test app: STEP 1 logs enaga::system::battery() as "B<level>"; stop() saves STOP.TXT in the app's data
// folder, so a test can see that a low-battery switch-off stopped the app (and closed its files) first.
#include <enaga/app.hpp>
#include <enaga/files.hpp>

class Battery : public enaga::App {
public:
    void on_key(int key, bool down) override {
        if (!down || key != ENAGA_KEY_STEP1) return;
        int level = enaga::system::battery();
        char line[4] = {'B', '-', '1', 0};
        if (level >= 0) { line[1] = char('0' + level); line[2] = 0; }
        enaga::log(line);
    }
    void stop() override {
        char path[64];
        const char *folder = enaga::files::data_folder();
        int n = 0;
        for (; folder[n] && n < 50; n++) path[n] = folder[n];
        for (const char *p = "/STOP.TXT"; *p; p++) path[n++] = *p;
        path[n] = 0;
        enaga::files::save(path, "STOPPED", 7);
    }
};

ENAGA_APP(Battery)
