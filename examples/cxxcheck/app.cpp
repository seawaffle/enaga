// E0 check app: exercises the C++ features an Enaga app relies on, and logs one line per result so a test
// can compare the whole log. Static constructors, virtual dispatch, pure virtuals, new/delete, function-local
// statics, templates, freestanding standard headers, 64-bit and float arithmetic (libgcc's helpers, and the
// runtime's own 64-bit division), and the string functions of <enaga/string.h>.
#include <enaga/app.hpp>
#include <enaga/string.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace {

void append(char *&out, const char *text) { while (*text) *out++ = *text++; }
void append(char *&out, int64_t value) {
    char digits[24];
    int n = 0;
    uint64_t magnitude = value < 0 ? uint64_t(-value) : uint64_t(value);
    do { digits[n++] = char('0' + magnitude % 10); magnitude /= 10; } while (magnitude);
    if (value < 0) *out++ = '-';
    while (n) *out++ = digits[--n];
}
void line(const char *label, int64_t value) {
    char text[64], *out = text;
    append(out, label);
    append(out, " ");
    append(out, value);
    *out = 0;
    enaga::log(text);
}

// A global with a constructor: runs from .init_array, before the app object exists.
struct Global {
    int value;
    Global() : value(41) { value++; enaga::log("global constructed"); }
};
Global g_global;

struct Voice {
    virtual ~Voice() = default;
    virtual int render(int input) const = 0;
};
struct Gain final : Voice {
    int amount;
    explicit Gain(int a) : amount(a) {}
    int render(int input) const override { return input * amount; }
};
struct Offset final : Voice {
    int amount;
    explicit Offset(int a) : amount(a) {}
    int render(int input) const override { return input + amount; }
};

int g_counter_constructions;
struct Counter {
    int start;
    Counter() : start(100) { g_counter_constructions++; }
};
int next_count() {
    static Counter counter;      // guarded function-local static
    return counter.start++;
}

template <typename T, std::size_t N> T sum(std::span<const T, N> values) {
    T total{};
    for (const T &value : values) total += value;
    return total;
}

std::optional<int> find_index(std::span<const int> values, int wanted) {
    for (std::size_t i = 0; i < values.size(); i++) if (values[i] == wanted) return int(i);
    return std::nullopt;
}

class CxxCheck : public enaga::App {
public:
    bool init() override {
        line("global", g_global.value);

        std::array<Voice *, 3> voices{new Gain(3), new Offset(-5), new Gain(-2)};
        int chained = 7;
        for (const Voice *voice : voices) chained = voice->render(chained);
        line("virtual", chained);                       // ((7*3)-5)*-2 = -32
        for (Voice *voice : voices) delete voice;

        // Heap reuse after delete: blocks coalesce, so a larger allocation fits where the three were.
        int *block = new int[64];
        for (int i = 0; i < 64; i++) block[i] = i * i;
        line("heap", block[63]);                        // 3969
        delete[] block;

        int a = next_count(), b = next_count(), c = next_count();
        line("static", a + b + c);                      // 100+101+102 = 303
        line("static constructions", g_counter_constructions);

        std::array<int, 6> values{9, -4, 17, 0, 3, 8};
        std::sort(values.begin(), values.end());
        line("sorted first", values.front());
        line("sorted last", values.back());
        line("sum", sum(std::span<const int, 6>(values)));   // 33
        auto index = find_index(values, 17);
        line("optional", index.value_or(-1));           // 5
        line("max", *std::max_element(values.begin(), values.end()));

        volatile int64_t big = int64_t(123456789) * 1000, divisor = 977, negative = -big;   // volatile: at run time
        line("int64 div", big / divisor);               // 126363141
        line("int64 mod", big % divisor);               // 343
        line("int64 negative div", negative / divisor); // -126363141, rounded towards zero
        line("int64 negative mod", negative % divisor); // -343, the dividend's sign
        volatile uint64_t huge = 0xFEDCBA9876543210ull, by = 0x123456789ull;
        line("uint64 div", int64_t(huge / by));         // 3740085487
        line("uint64 mod", int64_t(huge % by));         // 1490061265
        const char *name = "SAMPLES/KICK.WAV";
        line("strcmp", strcmp("ABC", "ABD") < 0 && strcmp("ABC", "ABC") == 0 && strcmp("B", "A") > 0);
        line("strncmp", strncmp("KICK1", "KICK2", 4) == 0 && strncmp("A", "AB", 5) < 0);
        line("strchr", strchr(name, '/') - name);       // 7
        line("strrchr", strrchr(name, '.') - name);     // 12
        line("strchr end", strchr(name, 0) - name == 16 && !strchr(name, 'Z'));
        volatile float x = 2.0f, y = 3.0f;              // volatile: force the runtime path
        float ratio = x / y;
        line("float milli", int64_t(ratio * 1000.0f));  // 666
        volatile double p = 1.0, q = 7.0;
        line("double micro", int64_t((p / q) * 1e6));   // 142857
        return true;
    }
    void update(uint32_t ms) override {
        updates++;
        line("update", ms);
    }
    void stop() override { line("stop after updates", updates); }

private:
    int updates = 0;
};

}  // namespace

ENAGA_APP(CxxCheck)
