// Enaga: an app's own settings, a small key/value store kept as text in its data folder
// (enaga::files::data_folder()/SETTINGS.TXT, lines of KEY=VALUE, readable and editable by hand).
//
//     enaga::Settings settings;                        // a member of the app
//     bool init() override { settings.load(); tempo = settings.get("TEMPO", 120); return true; }
//     void stop() override { settings.set("TEMPO", tempo); settings.save(); }   // writes only if something changed
//
// Keys are up to 23 bytes without '=' or a line break, text values up to 31. Up to 32 keys; set() on a 33rd
// returns false. Main context only. Enaga's own settings (volume, brightness...) are separate and not here.
#pragma once

#include <enaga/files.hpp>

namespace enaga {

class Settings {
public:
    static constexpr int max_keys = 32, key_bytes = 24, value_bytes = 32;

    explicit Settings(const char *file = "SETTINGS.TXT") : file_(file) {}

    // Reads the file (a missing one is empty settings). ENAGA_FILE_OK or a negative enaga_file_error.
    int32_t load() {
        count_ = 0;
        changed_ = false;
        char path[ENAGA_FILE_PATH_MAX + 1];
        if (!make_path(path)) return ENAGA_FILE_BAD_PATH;
        static char text[max_keys * (key_bytes + value_bytes + 1)];
        int32_t got = files::read_all(path, text, sizeof text);
        if (got == ENAGA_FILE_NOT_FOUND) return ENAGA_FILE_OK;
        if (got < 0) return got;
        for (int32_t i = 0, start = 0; i <= got; i++)
            if (i == got || text[i] == '\n' || text[i] == '\r') {
                parse(text + start, i - start);
                start = i + 1;
            }
        changed_ = false;
        return ENAGA_FILE_OK;
    }

    // Writes the file if anything changed since load() or the last save().
    int32_t save() {
        if (!changed_) return ENAGA_FILE_OK;
        char path[ENAGA_FILE_PATH_MAX + 1];
        if (!make_path(path)) return ENAGA_FILE_BAD_PATH;
        static char text[max_keys * (key_bytes + value_bytes + 1)];
        uint32_t n = 0;
        for (int k = 0; k < count_; k++) {
            for (const char *p = keys_[k]; *p; p++) text[n++] = *p;
            text[n++] = '=';
            for (const char *p = values_[k]; *p; p++) text[n++] = *p;
            text[n++] = '\n';
        }
        int32_t r = files::save(path, text, n);
        if (r >= 0) changed_ = false;
        return r < 0 ? r : int32_t(ENAGA_FILE_OK);
    }

    const char *text(const char *key, const char *fallback = "") const {
        int k = find(key);
        return k < 0 ? fallback : values_[k];
    }
    int32_t get(const char *key, int32_t fallback) const {
        int k = find(key);
        if (k < 0) return fallback;
        const char *p = values_[k];
        bool negative = *p == '-';
        if (negative) p++;
        if (*p < '0' || *p > '9') return fallback;
        int32_t v = 0;
        for (; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
        return *p ? fallback : negative ? -v : v;
    }

    bool set(const char *key, const char *value) {
        if (!valid(key, key_bytes) || !valid(value, value_bytes, true)) return false;
        int k = find(key);
        if (k < 0) {
            if (count_ == max_keys) return false;
            k = count_++;
            copy(keys_[k], key, key_bytes);
            values_[k][0] = 0;
            changed_ = true;
        }
        if (!same(values_[k], value)) {
            copy(values_[k], value, value_bytes);
            changed_ = true;
        }
        return true;
    }
    bool set(const char *key, int32_t value) {
        char digits[12];
        int n = 0;
        uint32_t v = value < 0 ? 0u - uint32_t(value) : uint32_t(value);
        do { digits[n++] = char('0' + v % 10); v /= 10; } while (v);
        char out[13];
        int o = 0;
        if (value < 0) out[o++] = '-';
        while (n) out[o++] = digits[--n];
        out[o] = 0;
        return set(key, out);
    }

    bool changed() const { return changed_; }

private:
    static bool same(const char *a, const char *b) {
        while (*a && *a == *b) { a++; b++; }
        return *a == *b;
    }
    static void copy(char *dst, const char *src, int size) {
        int i = 0;
        for (; i + 1 < size && src[i]; i++) dst[i] = src[i];
        dst[i] = 0;
    }
    static bool valid(const char *s, int size, bool value = false) {
        int i = 0;
        for (; s[i]; i++)
            if (i + 1 >= size || s[i] == '\n' || s[i] == '\r' || (!value && s[i] == '=')) return false;
        return value || i > 0;
    }
    int find(const char *key) const {
        for (int k = 0; k < count_; k++) if (same(keys_[k], key)) return k;
        return -1;
    }
    void parse(const char *line, int n) {
        int eq = 0;
        while (eq < n && line[eq] != '=') eq++;
        if (eq == n || eq == 0 || eq >= key_bytes || n - eq - 1 >= value_bytes) return;
        char key[key_bytes], value[value_bytes];
        for (int i = 0; i < eq; i++) key[i] = line[i];
        key[eq] = 0;
        for (int i = eq + 1; i < n; i++) value[i - eq - 1] = line[i];
        value[n - eq - 1] = 0;
        set(key, value);
    }
    bool make_path(char *path) const {
        uint32_t n = 0;
        for (const char *p = files::data_folder(); *p; p++) path[n++] = *p;
        path[n++] = '/';
        for (const char *p = file_; *p; p++) {
            if (n >= ENAGA_FILE_PATH_MAX) return false;
            path[n++] = *p;
        }
        path[n] = 0;
        return true;
    }

    const char *file_;
    char keys_[max_keys][key_bytes];
    char values_[max_keys][value_bytes];
    int count_ = 0;
    bool changed_ = false;
};

}  // namespace enaga
