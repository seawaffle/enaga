// E1 regression app: GCC (13.3, -O2) can release a function's stack frame before its last reads of a local
// array (pick() below: `addab B15, 72, B15`, then two loads from the array). An interrupt in that window
// must not write over the array: Enaga's interrupt entry skips a gap below the interrupted stack pointer
// (firmware/start.s). update() calls pick() back to back while audio interrupts arrive and logs how many
// results were wrong; tests/test_e1.py expects none.
#include <enaga/app.hpp>

#include <cstdint>

namespace {
volatile int g_in[16];

[[gnu::noinline]] int pick(int n, const volatile int *in) {
    int a[16];
    for (int i = 0; i < 16; i++) a[i] = in[i] * 3 + n;
    return a[n & 15] + a[(n >> 4) & 15];
}

void number(char *text, uint32_t value) {          // decimal, right after the text already there
    char digits[12];
    int count = 0;
    do digits[count++] = char('0' + value % 10); while (value /= 10);
    while (*text) text++;
    while (count) *text++ = digits[--count];
    *text = 0;
}
}  // namespace

class Stack : public enaga::App {
public:
    bool init() override {
        for (int i = 0; i < 16; i++) g_in[i] = i * 1000 + 7;
        return true;
    }
    void update(uint32_t) override {
        if (calls_ >= kCalls) return;
        for (int k = 0; k < 20000; k++, calls_++) {
            const int n = int(calls_ & 0xFF);
            const int want = g_in[n & 15] * 3 + n + g_in[(n >> 4) & 15] * 3 + n;
            if (pick(n, g_in) != want) wrong_++;
        }
        if (calls_ >= kCalls) {
            char text[40] = "STACK WRONG ";
            number(text, wrong_);
            enaga::log(text);
        }
    }

private:
    static constexpr uint32_t kCalls = 2000000;
    uint32_t calls_ = 0, wrong_ = 0;
};

ENAGA_APP(Stack)
