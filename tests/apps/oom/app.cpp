// E0 failure-path app: exhausts the heap from init(). operator new reports through the host's fatal().
#include <enaga/app.hpp>

namespace {
char *volatile g_sink;    // keeps each allocation observable, so the compiler cannot elide it
}

class Oom : public enaga::App {
public:
    bool init() override {
        enaga::log("allocating");
        for (;;) g_sink = new char[64 * 1024];
    }
};

ENAGA_APP(Oom)
