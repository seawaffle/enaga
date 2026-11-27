// Enaga: WAV files. parse() finds the format and the sample data in the first bytes of a file; header() makes
// the 44 bytes that start a PCM or float file, for a recording (Stream::create reserves them, Stream::close
// writes them with the final size); Writer records a file from audio() with both done for it; Loader reads a
// whole file into a dsp::Sample in the background.
//
//     uint8_t head[512];                                   // read from the file's start (File::read, read_all)
//     enaga::wav::Format wav;
//     if (enaga::wav::parse(head, got, wav) && wav.encoding == enaga::wav::pcm && wav.bits == 16) ...
//
// The fields are read byte by byte: WAV is packed, and the C674x silently rounds a misaligned word access down.
#pragma once

#include <cstdint>

#include <enaga/dsp/sample.hpp>
#include <enaga/files.hpp>

namespace enaga::wav {

enum Encoding : uint16_t { pcm = 1, floating = 3 };

struct Format {
    uint16_t encoding = pcm;       // pcm (8-bit unsigned, 16/24/32-bit signed) or floating (32-bit)
    uint16_t channels = 2;
    uint32_t rate = 48000;
    uint16_t bits = 16;
    uint32_t data_offset = 0;      // where the samples start in the file
    uint32_t data_bytes = 0;       // how many bytes of them (a header still being written may say 0)
    uint32_t frame_bytes() const { return uint32_t(channels) * (bits / 8u); }
};

namespace detail {
inline uint32_t u16(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
inline uint32_t u32(const uint8_t *p) { return u16(p) | u16(p + 2) << 16; }
inline void put16(uint8_t *p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
inline bool tag(const uint8_t *p, const char *t) { return p[0] == t[0] && p[1] == t[1] && p[2] == t[2] && p[3] == t[3]; }
}  // namespace detail

// True for a RIFF WAVE whose "fmt " chunk and the start of its "data" chunk lie in the `bytes` given, in a
// format listed above (WAVE_FORMAT_EXTENSIBLE included, by its sub-format).
inline bool parse(const void *start, uint32_t bytes, Format &out) {
    using namespace detail;
    const uint8_t *p = static_cast<const uint8_t *>(start);
    if (bytes < 12 || !tag(p, "RIFF") || !tag(p + 8, "WAVE")) return false;
    bool have_format = false;
    for (uint32_t at = 12; at < bytes && bytes - at >= 8;) {
        uint32_t size = u32(p + at + 4), body = at + 8;
        if (tag(p + at, "fmt ")) {
            if (size < 16 || body + 16 > bytes) return false;
            out.encoding = uint16_t(u16(p + body));
            if (out.encoding == 0xFFFE && size >= 40 && body + 26 <= bytes) out.encoding = uint16_t(u16(p + body + 24));
            out.channels = uint16_t(u16(p + body + 2));
            out.rate = u32(p + body + 4);
            out.bits = uint16_t(u16(p + body + 14));
            have_format = true;
        } else if (tag(p + at, "data")) {
            if (!have_format) return false;
            out.data_offset = body;
            out.data_bytes = size;
            bool known = out.encoding == pcm ? out.bits == 8 || out.bits == 16 || out.bits == 24 || out.bits == 32
                                             : out.encoding == floating && out.bits == 32;
            return known && out.channels > 0;
        }
        if (size > 0xFFFFFFFFu - body - 1) return false;
        at = body + size + (size & 1);            // chunks are padded to even sizes
    }
    return false;
}

// The 44-byte header of a file holding `data_bytes` of samples in `format` (data_offset is ignored: 44).
inline void header(uint8_t out[44], const Format &format, uint32_t data_bytes) {
    using namespace detail;
    const char *riff = "RIFF", *wave = "WAVEfmt ", *data = "data";
    for (int i = 0; i < 4; i++) out[i] = uint8_t(riff[i]);
    put32(out + 4, 36 + data_bytes);
    for (int i = 0; i < 8; i++) out[8 + i] = uint8_t(wave[i]);
    put32(out + 16, 16);
    put16(out + 20, format.encoding);
    put16(out + 22, format.channels);
    put32(out + 24, format.rate);
    put32(out + 28, format.rate * format.frame_bytes());
    put16(out + 32, format.frame_bytes());
    put16(out + 34, format.bits);
    for (int i = 0; i < 4; i++) out[36 + i] = uint8_t(data[i]);
    put32(out + 40, data_bytes);
}

// Loader's and Writer's errors, besides the file service's ENAGA_FILE_* codes.
enum LoadError : int32_t {
    not_wav = -64,                  // not a RIFF WAVE in a format parse() knows, no sample data, or a rate
                                    // outside Loader::min_rate - max_rate
    no_memory = -65,                // the heap has no room for the sample
};

// Records a WAV file through a stream (files::Stream): create() and close() in the main context, write() in
// audio() (or the main context, for a file rendered faster than real time; one context at a time). write() takes
// the app's float samples, interleaved, and converts them to the file's format (enaga_float_to_pcm: any of
// parse()'s, so a file read back with Loader gives the same samples). close() writes the header with the final
// size, so call it in stop() too: a recording the app never closes is left with an empty header.
//
//     enaga::wav::Writer take_;
//     enaga::wav::Format format;                              // 16-bit stereo at 48 kHz by default
//     format.bits = 24;
//     take_.create("/RECORD/TAKE1.WAV", format, ring_, 1 << 18);   // in the main context; ring_ from the heap
//     take_.write(in, frames);                                 // in audio(): the input, as it comes
//     take_.close();                                           // in the main context: the header, then closed
//
// The ring (a power of two, at least ENAGA_STREAM_RING_MIN) holds the samples until Enaga writes them to the card
// in the main loop; size it for the card's slowest moments, a few tenths of a second (256 KiB holds 0.9 s of
// 24-bit stereo). When it is full the frames that do not fit are dropped whole, and counted (frames_lost()).
class Writer {
public:
    static constexpr uint32_t header_bytes = 44;
    static constexpr uint32_t max_frame_bytes = 32;    // 8 channels of 32 bits

    Writer() = default;
    Writer(const Writer &) = delete;
    Writer &operator=(const Writer &) = delete;

    // A new file at path (an existing one is replaced), recording in format (its data_offset and data_bytes are
    // ignored). ENAGA_FILE_OK, an ENAGA_FILE_* error, or not_wav for a format parse() would not read back or wider
    // than max_frame_bytes.
    int32_t create(const char *path, const Format &format, void *ring, uint32_t ring_bytes) {
        close();
        pcm_ = enaga_pcm_format(format.encoding, format.bits);
        if (pcm_ < 0 || format.channels == 0 || format.frame_bytes() > max_frame_bytes) return not_wav;
        format_ = format;
        format_.data_offset = header_bytes;
        format_.data_bytes = 0;
        frames_ = 0;
        lost_ = 0;
        tail_bytes_ = 0;
        return stream_.create(path, header_bytes, ring, ring_bytes);
    }

    // frames frames of format().channels interleaved floats (±1 full scale): those taken, the rest lost.
    uint32_t write(const float *in, uint32_t frames) {
        const uint32_t channels = format_.channels, frame = format_.frame_bytes();
        const uint32_t per_chunk = sizeof chunk_ / frame;
        uint32_t taken = 0;
        if (!stream_.is_open() || !finish_tail()) { lost_ += frames; return 0; }
        while (taken < frames) {
            uint32_t n = frames - taken < per_chunk ? frames - taken : per_chunk;
            enaga_float_to_pcm(in + taken * channels, chunk_, pcm_, int(n * channels));
            uint32_t moved = put(n * frame);
            taken += moved;
            if (moved < n || tail_bytes_) break;     // the ring is full
        }
        frames_ += taken;
        lost_ += frames - taken;
        return taken;
    }

    // Bytes already in the file's format, whole frames: the frames taken.
    uint32_t write_bytes(const void *data, uint32_t bytes) {
        const uint32_t frame = format_.frame_bytes(), frames = stream_.is_open() ? bytes / frame : 0;
        const uint8_t *p = static_cast<const uint8_t *>(data);
        uint32_t taken = 0;
        if (!frames || !finish_tail()) { lost_ += frames; return 0; }
        while (taken < frames) {
            uint32_t n = frames - taken < sizeof chunk_ / frame ? frames - taken : sizeof chunk_ / frame;
            for (uint32_t i = 0; i < n * frame; i++) chunk_[i] = p[taken * frame + i];
            uint32_t moved = put(n * frame);
            taken += moved;
            if (moved < n || tail_bytes_) break;     // the ring is full
        }
        frames_ += taken;
        lost_ += frames - taken;
        return taken;
    }

    // Writes out what is left, the header with the final size over the file's start, and closes. Main context;
    // waits for the card. ENAGA_FILE_OK (also when nothing is open) or an error.
    int32_t close() {
        if (!stream_.is_open()) return ENAGA_FILE_OK;
        finish_tail();
        uint8_t head[header_bytes];
        uint32_t bytes = stream_.status().position;
        header(head, format_, bytes - bytes % format_.frame_bytes());   // a frame cut short is not counted
        return stream_.close(head, header_bytes);
    }

    bool is_open() const { return stream_.is_open(); }
    const Format &format() const { return format_; }
    uint32_t frames() const { return frames_; }            // frames written so far
    uint32_t frames_lost() const { return lost_; }         // frames dropped: the ring was full, or nothing open
    // The stream's state (main context): state (a negative error once the card failed or filled), position
    // (bytes of samples so far), buffered (bytes in the ring), underruns.
    files::Stream::Status status() const { return stream_.status(); }

private:
    // n bytes of chunk_ to the stream: the whole frames moved. A frame the ring took only part of is counted as
    // moved; its other bytes wait in tail_ and go first next time, so the file stays whole frames.
    uint32_t put(uint32_t n) {
        const uint32_t frame = format_.frame_bytes();
        uint32_t got = stream_.write(chunk_, n), part = got % frame;
        if (part) {
            tail_bytes_ = frame - part;
            for (uint32_t i = 0; i < tail_bytes_; i++) tail_[i] = chunk_[got + i];
        }
        return got / frame + (part ? 1u : 0u);
    }
    bool finish_tail() {                                   // true once no frame is cut short
        if (!tail_bytes_) return true;
        uint32_t got = stream_.write(tail_, tail_bytes_);
        for (uint32_t i = got; i < tail_bytes_; i++) tail_[i - got] = tail_[i];
        tail_bytes_ -= got;
        return tail_bytes_ == 0;
    }

    files::Stream stream_;
    Format format_;
    int pcm_ = -1;
    volatile uint32_t frames_ = 0, lost_ = 0;
    uint32_t tail_bytes_ = 0;
    uint8_t tail_[max_frame_bytes] = {};
    uint8_t chunk_[768] = {};                              // 64 frames of 24-bit stereo, 24 of 8 x 32-bit
};


// Reads a WAV file into a dsp::Sample in the background, a chunk at a time between the app's callbacks, so the
// screen and controls stay live: any format parse() knows, as float, mono or stereo (a file with more channels
// gives its first two), converted to the output rate on the way (enaga::dsp's resampler) unless asked not to.
// Main context. The Loader must outlive its load, as a member of the app does (requests pending when stop()
// returns are dropped without their callbacks).
//
//     enaga::wav::Loader loader_;
//     enaga::dsp::Sample kick_;
//     loader_.load<&App::loaded>("/SAMPLES/Kick.wav", kick_, this);
//     void loaded(int32_t result) { if (result > 0) ...kick_ holds `result` frames }
class Loader {
public:
    static constexpr uint32_t chunk_bytes = 16384;  // read and converted per step (a few ms of the main loop)
    static constexpr uint32_t min_rate = 8000, max_rate = 192000;   // the sample rates it takes (others: not_wav)

    Loader() = default;
    Loader(const Loader &) = delete;
    Loader &operator=(const Loader &) = delete;
    ~Loader() { free_buffers(); }

    // Starts loading path into sample (its old frames released at once). done(int32_t result) runs later in the
    // main context: the frames loaded, or a negative error (ENAGA_FILE_*, not_wav, no_memory), the sample then
    // empty. Returns ENAGA_FILE_OK, or an error at once (ENAGA_FILE_BUSY while a load is under way), with no
    // callback. to_output_rate false keeps the file's rate (play it with Sample::rate_for).
    template <auto Done, class T> int32_t load(const char *path, dsp::Sample &sample, T *object, bool to_output_rate = true) {
        return start(path, sample, &files::detail::done<T, Done>, object, to_output_rate);
    }
    int32_t load(const char *path, dsp::Sample &sample, enaga_file_done_t done, void *user, bool to_output_rate = true) {
        return start(path, sample, done, user, to_output_rate);
    }

    // Stops a load: no callback. The Loader stays busy until the file service hands back its buffer (the next
    // main-loop pass or so).
    void cancel() {
        if (state_ == reading || state_ == reading_header) {
            file_.close();                              // the pending read ends ENAGA_FILE_CANCELLED
            state_ = cancelling;
            if (sample_) sample_->release();
        }
    }
    bool busy() const { return state_ != idle; }
    // How far: 0 to 1, by the bytes read.
    float progress() const {
        return data_total_ ? float(data_total_ - data_left_) * enaga_reciprocal(float(data_total_)) : 0.0f;
    }

private:
    enum State : uint8_t { idle, reading_header, reading, cancelling };
    static constexpr uint32_t header_max = 1u << 20;   // how far into a file the sample data may start
    static constexpr int block = 256;                  // frames converted at a time

    int32_t start(const char *path, dsp::Sample &sample, enaga_file_done_t done, void *user, bool to_output_rate) {
        if (state_ != idle) return ENAGA_FILE_BUSY;
        sample.release();
        int32_t r = file_.open(path, ENAGA_FILE_READ);
        if (r < 0) return r;
        int32_t size = file_.size();
        if (size < 0) {
            file_.close();
            return size;
        }
        buffer_bytes_ = uint32_t(size) < chunk_bytes ? uint32_t(size) : chunk_bytes;
        buffer_ = new (std::nothrow) uint8_t[buffer_bytes_ ? buffer_bytes_ : 1];
        if (!buffer_) {
            file_.close();
            return no_memory;
        }
        sample_ = &sample;
        done_ = done;
        user_ = user;
        to_output_rate_ = to_output_rate;
        file_size_ = uint32_t(size);
        data_total_ = data_left_ = 0;
        written_ = carry_ = 0;
        state_ = reading_header;
        r = file_.read<&Loader::header_read>(buffer_, buffer_bytes_, this);
        if (r < 0) {
            file_.close();
            free_buffers();
            state_ = idle;
            return r;
        }
        return ENAGA_FILE_OK;
    }

    void header_read(int32_t result) {
        if (state_ == cancelling) return finish_cancel();
        if (result < 0) return finish(result);
        uint32_t got = uint32_t(result);
        format_ = Format();
        bool parsed = parse(buffer_, got, format_);
        if (!parsed && got == buffer_bytes_ && file_size_ > got && buffer_bytes_ < header_max && got >= 12 &&
            detail::tag(buffer_, "RIFF") && detail::tag(buffer_ + 8, "WAVE")) {
            // The data starts further in (large chunks before it): read up to header_max from the start again.
            delete[] buffer_;
            buffer_bytes_ = file_size_ < header_max ? file_size_ : header_max;
            buffer_ = new (std::nothrow) uint8_t[buffer_bytes_];
            if (!buffer_) return finish(no_memory);
            if (file_.seek(0) < 0) return finish(ENAGA_FILE_CARD_ERROR);
            int32_t r = file_.read<&Loader::header_read>(buffer_, buffer_bytes_, this);
            if (r < 0) return finish(r);
            return;
        }
        if (!parsed || !format_.frame_bytes() || format_.frame_bytes() > sizeof carry_bytes_ ||
            format_.rate < min_rate || format_.rate > max_rate || format_.data_offset > got)
            return finish(not_wav);
        uint32_t available = file_size_ - format_.data_offset;
        data_total_ = format_.data_bytes && format_.data_bytes < available ? format_.data_bytes : available;
        data_total_ -= data_total_ % format_.frame_bytes();
        data_left_ = data_total_;
        uint32_t frames = data_total_ / format_.frame_bytes();
        if (!frames) return finish(not_wav);
        channels_ = format_.channels < 2 ? 1 : 2;
        pcm_ = enaga_pcm_format(format_.encoding, format_.bits);
        resample_ = to_output_rate_ && format_.rate != uint32_t(ENAGA_SAMPLE_RATE);
        uint32_t rate = resample_ ? uint32_t(ENAGA_SAMPLE_RATE) : format_.rate;
        uint32_t room = resample_ ? uint32_t(uint64_t(frames) * ENAGA_SAMPLE_RATE / format_.rate) + 2 : frames;
        if (resample_) {
            table_ = new (std::nothrow) float[enaga_resample_table_size()];
            if (!table_) return finish(no_memory);
            enaga_resample_table(table_, format_.rate, ENAGA_SAMPLE_RATE);
            for (uint32_t c = 0; c < channels_; c++) enaga_resampler_init(&resampler_[c], table_, format_.rate, ENAGA_SAMPLE_RATE);
        }
        if (!sample_->allocate(room, channels_, rate)) return finish(no_memory);
        state_ = reading;
        consume(buffer_ + format_.data_offset, got - format_.data_offset);
        next();
    }

    void chunk_read(int32_t result) {
        if (state_ == cancelling) return finish_cancel();
        if (result < 0) return finish(result);
        consume(buffer_, uint32_t(result));
        if (result == 0 && data_left_) data_left_ = 0;     // the file ended early: keep what came
        next();
    }

    // Reads the next chunk, or ends the load when the data is all in.
    void next() {
        if (!data_left_) return complete();
        if (buffer_bytes_ > chunk_bytes) {                 // after a long header: back to the chunk size
            delete[] buffer_;
            buffer_bytes_ = chunk_bytes;
            buffer_ = new (std::nothrow) uint8_t[buffer_bytes_];
            if (!buffer_) return finish(no_memory);
        }
        uint32_t want = data_left_ < buffer_bytes_ ? data_left_ : buffer_bytes_;
        int32_t r = file_.read<&Loader::chunk_read>(buffer_, want, this);
        if (r < 0) finish(r);
    }

    // Converts the bytes of sample data at p (frames split across chunks wait in carry_bytes_).
    void consume(const uint8_t *p, uint32_t bytes) {
        uint32_t fb = format_.frame_bytes();
        bytes = bytes < data_left_ ? bytes : data_left_;
        data_left_ -= bytes;
        if (carry_) {                                      // complete the frame split by the last chunk
            uint32_t need = fb - carry_, take = need < bytes ? need : bytes;
            for (uint32_t i = 0; i < take; i++) carry_bytes_[carry_ + i] = p[i];
            carry_ += take;
            p += take;
            bytes -= take;
            if (carry_ < fb) return;
            convert(carry_bytes_, 1);
            carry_ = 0;
        }
        uint32_t frames = bytes / fb;
        for (uint32_t done = 0; done < frames;) {
            uint32_t n = frames - done < uint32_t(block) ? frames - done : uint32_t(block);
            convert(p + done * fb, n);
            done += n;
        }
        for (uint32_t i = frames * fb; i < bytes; i++) carry_bytes_[carry_++] = p[i];
    }

    // n whole frames at p, to float, through the resampler if converting, into the sample.
    void convert(const uint8_t *p, uint32_t n) {
        float in[block];
        uint32_t step = format_.bits / 8u;
        for (uint32_t c = 0; c < channels_; c++) {
            enaga_pcm_to_float(p + c * step, pcm_, format_.channels, in, int(n));
            write(c, in, int(n), c + 1 == channels_);
        }
    }

    // Channel c's n inputs to the sample (last: the final channel of these inputs, which moves the write position).
    void write(uint32_t c, const float *in, int n, bool last) {
        float *out = sample_->data();
        uint32_t room = sample_->frames(), at = written_;
        if (!resample_) {
            for (int i = 0; i < n && at < room; i++, at++) out[at * channels_ + c] = in[i];
        } else {
            float made_out[2 * block];
            for (int used = 0;;) {                         // until a call leaves room: all inputs taken
                int taken = 0, made = enaga_resample(&resampler_[c], in + used, n - used, made_out, 2 * block, &taken);
                for (int i = 0; i < made && at < room; i++, at++) out[at * channels_ + c] = made_out[i];
                used += taken;
                if (made < 2 * block) break;
            }
        }
        if (last) written_ = at;
    }

    void complete() {
        if (resample_) {                                   // the last outputs need TAPS / 2 inputs after them
            float zeros[ENAGA_RESAMPLE_TAPS / 2] = {};
            uint32_t start = written_;
            for (uint32_t c = 0; c < channels_; c++) {
                written_ = start;
                write(c, zeros, ENAGA_RESAMPLE_TAPS / 2, true);
            }
        }
        sample_->set_frames(written_);
        finish(int32_t(written_));
    }

    void finish(int32_t result) {
        file_.close();
        free_buffers();
        if (result < 0 && sample_) sample_->release();
        state_ = idle;
        enaga_file_done_t done = done_;
        void *user = user_;
        sample_ = nullptr;
        if (done) done(user, result);                      // may start another load
    }
    void finish_cancel() {
        free_buffers();
        sample_ = nullptr;
        state_ = idle;
    }
    void free_buffers() {
        delete[] buffer_;
        delete[] table_;
        buffer_ = nullptr;
        table_ = nullptr;
    }

    files::File file_;
    State state_ = idle;
    dsp::Sample *sample_ = nullptr;
    enaga_file_done_t done_ = nullptr;
    void *user_ = nullptr;
    bool to_output_rate_ = true, resample_ = false;
    uint8_t *buffer_ = nullptr;
    uint32_t buffer_bytes_ = 0, file_size_ = 0;
    Format format_;
    int pcm_ = 0;
    uint32_t channels_ = 1, data_total_ = 0, data_left_ = 0, written_ = 0, carry_ = 0;
    uint8_t carry_bytes_[64] = {};                         // a frame split across chunks (64 bytes at most)
    float *table_ = nullptr;
    enaga_resampler resampler_[2] = {};
};

}  // namespace enaga::wav
