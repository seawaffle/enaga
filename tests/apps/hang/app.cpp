// Watchdog test app: init() never returns, so the app is dropped during start-up.
#include <enaga/app.hpp>

class Hang : public enaga::App {
public:
    bool init() override {
        enaga::log("hang init");
        while (forever_) {}
        return true;
    }

private:
    volatile bool forever_ = true;
};

ENAGA_APP(Hang)
