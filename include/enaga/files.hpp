// Enaga: files on the SD card for apps. The C interface is the file part of <enaga/abi.h>; docs/design.md,
// "Files", has the design.
//
//     // Small files, at once (settings, patches): a few milliseconds of the main loop, never of audio.
//     enaga::files::save("/ENAGA/DATA/SYNTH/patch 1.syn", &patch, sizeof patch);
//     int32_t got = enaga::files::read_all("/ENAGA/DATA/SYNTH/patch 1.syn", &patch, sizeof patch);
//
//     // Large ones in the background: done() runs in the main context when the load has finished.
//     enaga::files::load("/SAMPLES/Kick.raw", buffer, bytes, this, &MyApp::loaded);   // void loaded(int32_t)
//
//     // Longer than memory, or recorded: a Stream (below), read or written in audio(). <enaga/wav.hpp> reads and
//     // writes WAV headers.
//
// Results are byte counts, handles or ENAGA_FILE_OK, or a negative enaga_file_error (enaga::files::error_name
// gives a short name for one). Paths are absolute, UTF-8. An app reads anywhere and writes in
// enaga::files::data_folder() (/ENAGA/DATA/<package name>), /SAMPLES and /RECORD. Main context only. A
// buffer given to a background call must stay valid until its callback.
#pragma once

#include <enaga/app.hpp>

namespace enaga::files {

using Info = enaga_file_info_t;

inline const char *data_folder() { return host().file_data_folder(); }   // "/ENAGA/DATA/<package name>"
inline int32_t read_all(const char *path, void *buffer, uint32_t max) { return host().file_read_all(path, buffer, max); }
inline int32_t save(const char *path, const void *data, uint32_t bytes) { return host().file_save(path, data, bytes); }
inline int32_t info(const char *path, Info &out) { return host().file_info(path, &out); }
inline int32_t remove(const char *path) { return host().file_remove(path); }
inline int32_t make_folder(const char *path) { return host().file_make_folder(path); }

// Callbacks as a plain function and its argument...
inline int32_t load(const char *path, void *buffer, uint32_t max, enaga_file_done_t done, void *user) {
    return host().file_load(path, buffer, max, done, user);
}
inline int32_t list(const char *path, enaga_file_entry_t entry, enaga_file_done_t done, void *user) {
    return host().file_list(path, entry, done, user);
}

// ...or as member functions of an object: void T::done(int32_t result), void T::entry(const Info &).
namespace detail {
template <class T, void (T::*F)(int32_t)> void done(void *object, int32_t result) { (static_cast<T *>(object)->*F)(result); }
template <class T, void (T::*F)(const Info &)> void entry(void *object, const enaga_file_info_t *e) {
    (static_cast<T *>(object)->*F)(*e);
}
}  // namespace detail
template <auto Done, class T> int32_t load(const char *path, void *buffer, uint32_t max, T *object) {
    return load(path, buffer, max, &detail::done<T, Done>, object);
}
template <auto Entry, auto Done, class T> int32_t list(const char *path, T *object) {
    return list(path, &detail::entry<T, Entry>, &detail::done<T, Done>, object);
}

// An open file, closed when the object goes. Background reads and writes on it run in order.
class File {
public:
    File() = default;
    File(const char *path, uint32_t mode) { open(path, mode); }
    ~File() { close(); }
    File(const File &) = delete;
    File &operator=(const File &) = delete;

    int32_t open(const char *path, uint32_t mode) {       // ENAGA_FILE_READ or ENAGA_FILE_WRITE
        close();
        int32_t r = host().file_open(path, mode);
        handle_ = r >= 0 ? r : -1;
        return r < 0 ? r : int32_t(ENAGA_FILE_OK);
    }
    int32_t close() {
        int32_t r = handle_ >= 0 ? host().file_close(handle_) : int32_t(ENAGA_FILE_OK);
        handle_ = -1;
        return r;
    }
    bool is_open() const { return handle_ >= 0; }
    int32_t size() const { return host().file_size(handle_); }
    int32_t seek(uint32_t position) { return host().file_seek(handle_, position); }
    int32_t read(void *buffer, uint32_t bytes, enaga_file_done_t done, void *user) {
        return host().file_read(handle_, buffer, bytes, done, user);
    }
    int32_t write(const void *data, uint32_t bytes, enaga_file_done_t done, void *user) {
        return host().file_write(handle_, data, bytes, done, user);
    }
    template <auto Done, class T> int32_t read(void *buffer, uint32_t bytes, T *object) {
        return read(buffer, bytes, &detail::done<T, Done>, object);
    }
    template <auto Done, class T> int32_t write(const void *data, uint32_t bytes, T *object) {
        return write(data, bytes, &detail::done<T, Done>, object);
    }

private:
    int32_t handle_ = -1;
};

// A stream: a file played into audio(), or recorded from it, through a ring buffer the app gives (a power of
// two, ENAGA_STREAM_RING_MIN or more; it belongs to Enaga until close). open/create/status/close in the main
// context; read or write in audio(), where they never wait. Closed when the object goes.
//
//     ring_ = new uint8_t[1 << 16];
//     stream_.open("/SAMPLES/Loop.wav", wav.data_offset, ring_, 1 << 16);   // then wait for ready()
//     ...in audio(): uint32_t got = stream_.read(buffer, frames * 4);       // short = an underrun, or the end
class Stream {
public:
    using Status = enaga_stream_status_t;
    Stream() = default;
    ~Stream() { close(); }
    Stream(const Stream &) = delete;
    Stream &operator=(const Stream &) = delete;

    int32_t open(const char *path, uint32_t offset, void *ring, uint32_t ring_bytes) {   // playback
        close();
        return set(host().stream_open(path, offset, ring, ring_bytes));
    }
    int32_t create(const char *path, uint32_t reserve, void *ring, uint32_t ring_bytes) {   // recording
        close();
        return set(host().stream_create(path, reserve, ring, ring_bytes));
    }
    // A recording is written out first, then `header` over the file's start (the space `reserve` kept).
    int32_t close(const void *header = nullptr, uint32_t header_bytes = 0) {
        int32_t r = stream_ >= 0 ? host().stream_close(stream_, header, header_bytes) : int32_t(ENAGA_FILE_OK);
        stream_ = -1;
        return r;
    }
    bool is_open() const { return stream_ >= 0; }
    uint32_t read(void *out, uint32_t bytes) { return host().stream_read(stream_, out, bytes); }
    uint32_t write(const void *data, uint32_t bytes) { return host().stream_write(stream_, data, bytes); }
    Status status() const {
        Status s{};
        if (host().stream_status(stream_, &s) != ENAGA_FILE_OK) s.state = ENAGA_FILE_BAD_HANDLE;
        return s;
    }
    bool ready() const { return status().state >= ENAGA_STREAM_RUNNING; }   // playback: filled, start reading

private:
    int32_t set(int32_t r) {
        stream_ = r >= 0 ? r : -1;
        return r < 0 ? r : int32_t(ENAGA_FILE_OK);
    }
    int32_t stream_ = -1;
};

inline const char *error_name(int32_t result) {
    switch (result) {
    case ENAGA_FILE_NO_CARD: return "NO CARD";
    case ENAGA_FILE_NOT_FOUND: return "NOT FOUND";
    case ENAGA_FILE_EXISTS: return "EXISTS";
    case ENAGA_FILE_FULL: return "CARD FULL";
    case ENAGA_FILE_DENIED: return "DENIED";
    case ENAGA_FILE_TOO_MANY: return "TOO MANY";
    case ENAGA_FILE_CARD_ERROR: return "CARD ERROR";
    case ENAGA_FILE_CANCELLED: return "CANCELLED";
    case ENAGA_FILE_BAD_PATH: return "BAD PATH";
    case ENAGA_FILE_BAD_HANDLE: return "BAD HANDLE";
    case ENAGA_FILE_TOO_BIG: return "TOO BIG";
    case ENAGA_FILE_BUSY: return "BUSY";
    case ENAGA_FILE_WRONG_CONTEXT: return "WRONG CONTEXT";
    default: return result >= 0 ? "OK" : "ERROR";
    }
}

}  // namespace enaga::files
