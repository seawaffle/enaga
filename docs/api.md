# Enaga API reference

What an app can use, header by header. This is the reference; `docs/getting-started.md` goes from nothing to an
app on the card, and `examples/tone` (minimal) and `examples/synth` (a whole instrument) are working code. The interface is
**provisional until 1.0** (`ENAGA_ABI_VERSION` 0): names and the binary layout may still change.
In the repository, `tests/test_api_docs.py` fails when a public name in `include/enaga/` is missing here.

| Header | What it gives |
|---|---|
| `<enaga/app.hpp>` | `enaga::App`, `ENAGA_APP`, `enaga::Canvas`, LEDs, `enaga::system`, `log`, `ms`, `fatal` |
| `<enaga/files.hpp>` | `enaga::files`: whole files, background reads and writes, folders, `File`, `Stream` |
| `<enaga/wav.hpp>` | `enaga::wav`: WAV headers |
| `<enaga/settings.hpp>` | `enaga::Settings`: an app's own key/value settings |
| `<enaga/midi.hpp>`, `<enaga/clock.hpp>` | `enaga::midi`: sending MIDI; `enaga::clock`: tempo, transport, ticks |
| `<enaga/ui.hpp>` | `enaga::ui`: list, parameter page, dialog, text entry |
| `<enaga/bytes.hpp>` | `enaga::bytes`: numbers in packed data at any address |
| `<enaga/dsp.hpp>` | `enaga::dsp`: math, oscillators, filters, envelopes, LFOs, delays, chorus, reverb |
| `<enaga/abi.h>` | The C boundary under all of it: constants, events, the host and app tables, the package header |
| `<enaga/dsp.h>`, `<enaga/dsp/math.h>` | The DSP library's C interface, for C and `*.ti.c` files |

## Contents

1. [Building and installing an app](#building-and-installing-an-app)
2. [The app](#the-app)
3. [The two contexts](#the-two-contexts)
4. [Audio](#audio)
5. [Input](#input)
6. [Display](#display)
7. [LEDs](#leds)
8. [System](#system)
9. [Files](#files)
10. [Streams](#streams)
11. [WAV headers](#wav-headers)
12. [App settings](#app-settings)
13. [MIDI](#midi)
14. [Clock and transport](#clock-and-transport)
15. [UI toolkit](#ui-toolkit)
16. [DSP library](#dsp-library)
17. [The C interface](#the-c-interface)
18. [Language and runtime](#language-and-runtime)

## Building and installing an app

Outside this repository, the SDK has what building needs (`docs/getting-started.md` goes from an empty folder to
an app on the card). Here, or in the SDK:

```sh
python3 -B tools/build_app.py examples/tone                 # -> build/apps/tone/APP.ENA
python3 -B tools/build_app.py DIR -o /media/card/ENAGA/APPS/NAME/APP.ENA --name "MY SYNTH" --app-version 3
python3 -B tools/build_app.py ~/my-app                      # outside Enaga's folder -> ~/my-app/build/APP.ENA
```

Every `*.cpp` and `*.c` file in the directory is compiled and linked with Enaga's runtime and the DSP library into
one package. `--name` is the display name in the Enaga menu (default: the directory's name; 1-23 printable ASCII
characters, shown upper case); `--app-version` is a number stored in the package. On the card the package goes at
`/ENAGA/APPS/<NAME>/APP.ENA`, `<NAME>` a folder name of up to 8 letters, digits, `_` or `-`. That folder name is the app's **package name**, which
also names its data folder (`/ENAGA/DATA/<NAME>`), so keep it when updating the app.

**The icon.** An optional `icon.txt` in the directory is the app's icon in the Enaga menu, shown beside its name:
16 lines of 16 greys, each a hex digit from `0` (black) to `F` (white), or `.` for 0. Lines starting with `#` are
comments. Grey 0 shows the menu's background, which is dim grey on the selected line. An app without one gets a
generic icon. `examples/tone/icon.txt`:

```
................
...FFF..........
..FF.FF.........
.FF...F.........
```
(the first rows of a sine wave).

Apps are compiled with `-Wall -Wextra -Werror`: warnings stop the build. Files named `*.ti.c` are built with TI's
cl6x when it is installed ("Writing inner loops for cl6x" below).

## The app

```cpp
#include <enaga/app.hpp>

class Beep : public enaga::App {
public:
    bool init() override { enaga::log("BEEP READY"); return true; }
    void on_key(int key, bool down) override { if (key == ENAGA_KEY_F5) playing_ = down; }
    void audio(const float *, float *out, uint32_t frames) override {
        for (uint32_t i = 0; i < frames; i++) {
            float s = playing_ ? ((phase_++ & 32) ? 0.2f : -0.2f) : 0.0f;   // 750 Hz square
            out[2 * i] = out[2 * i + 1] = s;
        }
    }
    void draw(enaga::Canvas &canvas) override { canvas.clear(); canvas.text(0, 0, "BEEP"); }

private:
    volatile bool playing_ = false;     // written in the main context, read in audio
    uint32_t phase_ = 0;
};

ENAGA_APP(Beep)
```

**`ENAGA_APP(Type)`** defines the app. `Type` derives from `enaga::App` and is default-constructible. Exactly one
per app.

**`enaga::App`**: override what you need; the defaults do nothing (`audio` writes silence).

| Callback | Context | When |
|---|---|---|
| `bool init()` | main | once, after the app's static constructors and the app object's constructor. Return `false` to refuse to start: Enaga shows `APP INIT FAILED` |
| `void update(uint32_t ms)` | main | every pass of the main loop, at least 750 times a second (after every audio block); `ms` is `enaga::ms()` |
| `void draw(Canvas &canvas)` | main | every 50 ms (20 frames a second), later if the previous frame is still being sent |
| `void stop()` | main | before the app is unloaded (switching apps, switching off): save state here |
| `void on_key(int key, bool down)` | main | a key went down or up ([Input](#input)) |
| `void on_encoder(int encoder, int delta)` | main | an encoder turned: signed detents |
| `void on_pad_press(int pad, int velocity)` | main | a pad pressed as a note: velocity 1-127 |
| `void on_pad_release(int pad)` | main | that note released |
| `void on_pad_pressure(int pad, int pressure)` | main | a held pad's pressure changed: 0-127 |
| `void on_midi(const MidiMessage &message)` | main | a MIDI message arrived ([MIDI](#midi)) |
| `void on_transport(int what, uint32_t position)` | main | the clock started, stopped, continued or was located ([Clock](#clock-and-transport)) |
| `void on_midi_sysex(int port, const uint8_t *data, uint32_t bytes, bool start, bool end)` | main | a chunk of System Exclusive arrived |
| `void on_pad(int pad, unsigned pressure)` | main | every raw pad reading, 0-16383, about 3,930 a second in all |
| `void on_card(bool usable)` | main | the SD card was inserted and mounted (`true`), or removed or unusable (`false`) |
| `void audio(const float *in, float *out, uint32_t frames)` | audio | every block ([Audio](#audio)) |

**Life of an app.** Enaga loads the package, runs its static constructors, constructs the app object and calls
`init()`. Then the main loop delivers events and calls `update` and `draw`, while the audio interrupt calls
`audio`. To switch apps or switch off, Enaga fades the output to silence over about 11 ms (the app's `audio` still
running), removes the audio callback, calls `stop()` and discards the app's whole memory. **Destructors never
run**, the app object's included: save in `stop()`, not in a destructor.

**While the Enaga menu is open** (F1 + POWER, or `enaga::system::open_menu()`) the app is paused: its `audio` goes
on, but it gets no events, `update` or `draw`. As the menu opens it is sent a release for every key and pad it was
told is down, so it never holds a stuck note. F1 + POWER again, or CLR, resumes it.

**Limits.** Enaga's watchdog stops the app when a main-context callback (`init` included) has not returned after
1 s, or `audio` after 100 ms; `stop()` gets 3 s in all, even while saving to the card. A stopped app is dropped as
by `fatal`: Enaga shows the reason on its status page and keeps running; OK opens the menu. An app that fails to
start is not restarted automatically at the next power-on.

## The two contexts

| Context | Runs | May | Must not |
|---|---|---|---|
| main | every callback except `audio` | take time (within the watchdog), use files, draw, allocate | block on something that never comes |
| audio | `audio`, in the audio interrupt, every 1.33 ms | compute, read and write streams | allocate, use files (they return `ENAGA_FILE_WRONG_CONTEXT`), draw, set LEDs (ignored), wait |

The audio interrupt can arrive at any point in main-context code. Share state between the two with single words
written by one side only (`volatile`, or `std::atomic` with lock-free types), or a single-producer
single-consumer ring. Allocate everything `audio` needs in `init()`.

## Audio

`audio(in, out, frames)` gets `frames` (`ENAGA_BLOCK_FRAMES`, 64) frames of interleaved stereo float, L R L R, at
`ENAGA_SAMPLE_RATE` (48,000 Hz): `in` is the codec input, and `out` must be written completely. Full scale is
±1.0; Enaga clips beyond it. The user's VOLUME setting and the switch fade are applied after `audio`, so write
full level.

| Function | Gives |
|---|---|
| `uint32_t enaga::audio_load()` | the last block's cost, per mille of its real-time budget (1000 = all of it) |
| `uint32_t enaga::audio_load_peak()` | the highest since the app started |

Enaga itself takes about 3 % of the budget.

### The input: `enaga::system::input(source, level)`

`in` carries the input the app chooses, as Lofi-12 XT and CyDrums choose theirs: a source and a level. Each
pair sets the codec and the analogue front end the way the original SmplTrek firmware sets the matching REC SRC.
Returns 0, -1 for an unknown source or level, or -2 if the codec did not answer. Main context; it takes a few
milliseconds. `in` is silent for about 10 ms around the change, which hides the switching. When the app stops,
the input goes back to stereo line, NORMAL, which is also what an app starts with.

| Source | Input | `in` | LOW | NORMAL | HIGH |
|---|---|---|---|---|---|
| `ENAGA_INPUT_MIC` | the built-in microphone | both channels; silent unless headphones are plugged in | one level | | |
| `ENAGA_INPUT_HIZ` | INPUT L, high impedance (guitar) | both channels | no front-end gain | +10 dB stage | +10 dB stage, +10 dB in the codec |
| `ENAGA_INPUT_DYNAMIC_MIC` | INPUT R through the mic preamplifier | both channels | preamplifier off | preamplifier on | preamplifier, +21 dB in the codec |
| `ENAGA_INPUT_LINE_STEREO` | INPUT L and R | left and right | -20 dB PAD | no PAD | as NORMAL |
| `ENAGA_INPUT_LINE_MONO` | INPUT L | both channels | -20 dB PAD | no PAD | as NORMAL |

The levels are `ENAGA_INPUT_LOW`, `ENAGA_INPUT_NORMAL` (the default argument) and `ENAGA_INPUT_HIGH`. The
built-in microphone is heard only with headphones in, a rule every original firmware keeps against feedback
from the speaker; Enaga applies it, so apps need not. The front-end gains are the original firmware's
settings. The values in dB come from its manual and code and have not been measured; the PAD's -20 dB is the
manual's figure.

## Input

### Keys: `on_key(int key, bool down)`

`key` is an `enaga_key`. POWER never reaches an app. The 2 × 4 function block is numbered F1-F8 in reading order
(F1-F4 the upper row) because its printed labels differ between the SmplTrek, Lofi-12 XT and CyDrums; F1 is the
app's function key, though F1 + POWER always opens the Enaga menu.

| Keys | Names |
|---|---|
| Function block | `ENAGA_KEY_F1` `ENAGA_KEY_F2` `ENAGA_KEY_F3` `ENAGA_KEY_F4` (upper row), `ENAGA_KEY_F5` `ENAGA_KEY_F6` `ENAGA_KEY_F7` `ENAGA_KEY_F8` (lower row) |
| STEP 1-16 | `ENAGA_KEY_STEP1` + n - 1 |
| Cursor | `ENAGA_KEY_UP` `ENAGA_KEY_DOWN` `ENAGA_KEY_LEFT` `ENAGA_KEY_RIGHT` |
| Confirm and cancel | `ENAGA_KEY_OK` `ENAGA_KEY_CLR` |
| Octave | `ENAGA_KEY_OCTAVE_LEFT` `ENAGA_KEY_OCTAVE_RIGHT` |
| Selection buttons | `ENAGA_KEY_SELECT_LEFT` `ENAGA_KEY_SELECT_RIGHT` |
| Knob pushes | `ENAGA_KEY_PUSH_A` `ENAGA_KEY_PUSH_B` `ENAGA_KEY_PUSH_C` `ENAGA_KEY_PUSH_D` `ENAGA_KEY_PUSH_VALUE` |

`ENAGA_KEY_COUNT` (48) bounds key numbers, for arrays indexed by key. Keys are debounced by Enaga.

### Encoders: `on_encoder(int encoder, int delta)`

`encoder` 0-3 are A-D, 4 is VALUE; `delta` is signed detents since the last event, clockwise positive.

### Pads

Fifteen pads, numbered 0-14 as the panel controller reports them (docs/hardware.md "LED map" has their places).

- `on_pad_press(pad, velocity)`: a note starts when a pad's reading crosses the threshold the user's PADS setting
  chooses (1,600 / 1,000 / 600 for LOW / MEDIUM / HIGH). It is sent one reading later (at most 3.8 ms), with
  velocity 1-127 from the higher of the two readings, 127 at 3,300 or more.
- `on_pad_release(pad)`: the reading fell below 3/4 of the threshold. Every press gets its release.
- `on_pad_pressure(pad, pressure)`: aftertouch, 0-127, between a press and its release. The hit is left to
  settle for 40 readings (about 150 ms) and the reading then is the pad's baseline; pressure starts once the pad
  is pressed more than 200 above it, so resting a finger on a pad after hitting it sends nothing. The value is the
  mean of the last 9 readings put through a cube curve from the threshold to 3,300 (127 u³, as Lofi-12 XT's
  shape: most of the range is in the firmer part of the press), sent only when it moves by more than 1.
- `on_pad(pad, pressure)`: every raw reading, 0-16383, whatever the threshold, for an app's own pressure handling.
  Untouched pads read 1-2; a light press about 1,500, a hard one about 3,400.

They are independent: an app can use notes, pressure, raw readings or any mix. A press or release comes just before the
raw reading that caused it.

### The card: `on_card(bool usable)`

`true` when a card was inserted and mounted; `false` when it was removed, or an inserted card cannot be used.
Files open before a removal are dead: close them.

## Display

128 × 128 pixels (`ENAGA_SCREEN_W`, `ENAGA_SCREEN_H`), 16 greys: 0 black to 15 white. `draw(canvas)` paints a
fresh frame each time; Enaga sends it to the display by DMA after `draw` returns.

**`enaga::Canvas`**

| Member | Does |
|---|---|
| `width`, `height` | 128, 128 (`static constexpr`) |
| `clear(grey = 0)` | fills the canvas |
| `pixel(x, y, grey)` | one pixel; outside the canvas is ignored |
| `rect(x, y, w, h, grey)` | a filled rectangle, clipped |
| `text(x, y, s, grey = 15)` | text in a 5 × 7 font, 6 pixels a character, `y` the top row |
| `data()` | the pixels, `uint8_t[128 * 128]`, row-major: draw anything else here |

The font has `A`-`Z` (lower case is shown as upper case), `0`-`9`, space and `! # + - . / : = ? % >`; other
characters are left blank.

## LEDs

Levels run 0 (off) to `ENAGA_LED_MAX` (10). Main context only; ignored from `audio`. Enaga switches every LED
off when an app stops.

| Function | LED |
|---|---|
| `enaga::pad_led(pad, r, g, b)` | pad 0-14, red, green and blue |
| `enaga::step_led(step, red, green)` | STEP 1-16 |
| `enaga::button_led(key, red, green)` | a key with an LED: F1-F8, CLR, OK (an `enaga_key`) |
| `enaga::vol_led(channel, level)` | 0 = VOL L, 1 = VOL R (one colour) |
| `enaga::mic_led(level)` | the microphone LED (one colour) |
| `enaga::value_led(r, g, b)` | the VALUE encoder's ring |

The octave buttons have no LED. At the C level these are one call, `led(n, red, green, blue)`, with `n` an
`enaga_led`: `ENAGA_LED_PAD` + pad, `ENAGA_LED_VOL_L`, `ENAGA_LED_VOL_R`, `ENAGA_LED_MIC`, `ENAGA_LED_VALUE`,
`ENAGA_LED_KEY` + key, below `ENAGA_LED_COUNT`.

## System

In `<enaga/app.hpp>`. Main context.

| Function | Does |
|---|---|
| `enaga::log(text)` | one line on Enaga's log (its status page shows the last lines) |
| `enaga::ms()` | milliseconds since power-on |
| `enaga::fatal(reason)` | stops the app at once; Enaga shows `reason` and keeps running. Does not return |
| `enaga::system::open_menu()` | opens the Enaga menu once the current callback returns, as F1 + POWER does |
| `enaga::system::set_busy(bool)` | while `true`, the unit does not switch itself off for being idle (the AUTO OFF setting), e.g. while playing or recording unattended. Cleared when the app stops |
| `enaga::system::speaker(bool on)` | `false` mutes the built-in speaker, e.g. while monitoring the built-in microphone (feedback). The user's SPEAKER setting still wins. Released when the app stops |
| `enaga::system::headphones()` | `true` while headphones are plugged in; the speaker is cut in hardware meanwhile. Poll it, e.g. in `update` |
| `enaga::system::input(source, level)` | chooses the audio input ([The input](#the-input-enagasysteminputsource-level)) |
| `enaga::system::battery()` | the battery: 0-4 (4 full) while the unit runs on batteries, -1 on USB or adapter power. It follows the voltage over several seconds, using the user's BATTERY setting (ALKALINE, NiMH, LITHIUM). At 0 Enaga shows a warning and, if the level stays there for 30 seconds, switches the unit off, stopping the app as POWER does, so its `stop()` and file closing run before the supply gives out |
| `enaga::system::device()` | which device Enaga runs on: `ENAGA_DEVICE_SMPLTREK`, `ENAGA_DEVICE_LOFI12XT`, `ENAGA_DEVICE_CYDRUMS` or `ENAGA_DEVICE_UNKNOWN`. The three have the same board and controls in the same places; use it for what differs, such as labels printed on the panel |

Volume, brightness, pad sensitivity, AUTO OFF, SPEAKER, POL IN, POL OUT and BATTERY are the user's settings, in the Enaga menu; apps
do not set them.

## Files

In `<enaga/files.hpp>`, namespace `enaga::files`. **Main context only.**

**Paths** are absolute, UTF-8, up to `ENAGA_FILE_PATH_MAX` (255) bytes: `/SAMPLES/Kick 01.wav`. Long names, FAT
and exFAT cards; names compare without regard to case. `.`, `..` and empty parts are refused.

**Where an app may write:** its own folder, `data_folder()` (`/ENAGA/DATA/<package name>`), and the shared
`/SAMPLES` and `/RECORD`. It may read anywhere. Folders are created as needed.

**Results** are a byte count, a count, a handle or `ENAGA_FILE_OK` (0), or a negative `enaga_file_error`:

| Error | Meaning |
|---|---|
| `ENAGA_FILE_NO_CARD` | no card, or it was removed (handles opened before are dead) |
| `ENAGA_FILE_NOT_FOUND` | no such file or folder |
| `ENAGA_FILE_EXISTS` | it exists already |
| `ENAGA_FILE_FULL` | the card is full: a write stored fewer bytes |
| `ENAGA_FILE_DENIED` | a write outside the app's areas, or a read-only file |
| `ENAGA_FILE_TOO_MANY` | `ENAGA_FILES_OPEN` (8) files open already, or the request queue is full |
| `ENAGA_FILE_CARD_ERROR` | the card or its file system failed |
| `ENAGA_FILE_CANCELLED` | the file was closed before the request ran |
| `ENAGA_FILE_BAD_PATH` | not absolute, `.` or `..` in it, an empty part, too long |
| `ENAGA_FILE_BAD_HANDLE` | not an open file |
| `ENAGA_FILE_TOO_BIG` | the file is larger than the buffer given |
| `ENAGA_FILE_BUSY` | open elsewhere for writing, or the file has requests pending |
| `ENAGA_FILE_WRONG_CONTEXT` | called from `audio` |

`error_name(result)` gives a short upper-case name for one (`"NOT FOUND"`), or `"OK"`.

### Immediate calls

These finish before they return, stalling the main loop (never audio) meanwhile: milliseconds for small files.
They are what `init()` and `stop()` should use.

| Function | Does |
|---|---|
| `read_all(path, buffer, max)` | reads a whole file into `buffer`: its size, or `ENAGA_FILE_TOO_BIG` if over `max` |
| `save(path, data, bytes)` | writes a whole file **safely**: a card pulled or power lost mid-save leaves the old file or the new one, never a mix (`read_all` finds either) |
| `info(path, Info &out)` | name, size and flags (`ENAGA_FILE_FOLDER`) of a file or folder |
| `remove(path)` | a file, or an empty folder |
| `make_folder(path)` | a folder and its parents; `ENAGA_FILE_OK` if it exists |
| `data_folder()` | the app's own folder, `/ENAGA/DATA/<package name>` (no trailing `/`) |

`Info` is `enaga_file_info_t`: `name` (UTF-8, no folder part), `size` (bytes; 4 GiB or more reads as
0xFFFFFFFF), `flags`.

### Background calls

These queue a request and return `ENAGA_FILE_OK` (or an error at once); Enaga works on the queue in the main loop
between the app's callbacks, so the screen and controls stay live, and calls `done(result)` in the main context
when it finishes. Requests run in the order made. **A buffer given to one belongs to Enaga until its callback.**

| Function | Does |
|---|---|
| `load(path, buffer, max, done, user)` | reads a whole file; `done` gets its size or an error |
| `list(path, entry, done, user)` | calls `entry(user, &info)` for each entry of a folder, then `done` with the count |

The callbacks are a plain function and a pointer (`enaga_file_done_t`: `void (void *user, int32_t result)`;
`enaga_file_entry_t`: `void (void *user, const enaga_file_info_t *)`), or member functions of an object:

```cpp
class Player : public enaga::App {
    void loaded(int32_t result) { if (result >= 0) bytes_ = result; }
    void entry(const enaga::files::Info &info) { /* one file */ }
    void listed(int32_t count) {}
    bool init() override {
        enaga::files::load<&Player::loaded>("/SAMPLES/Kick.raw", buffer_, sizeof buffer_, this);
        enaga::files::list<&Player::entry, &Player::listed>("/SAMPLES", this);
        return true;
    }
    ...
};
```

A request still pending when `stop()` returns is cancelled without its callback.

### `enaga::files::File`

An open file, closed when the object goes. Not copyable. At most `ENAGA_FILES_OPEN` (8) open at a time.

| Member | Does |
|---|---|
| `File(path, mode)`, `open(path, mode)` | opens: `ENAGA_FILE_READ`, or `ENAGA_FILE_WRITE` (created, or emptied if it exists) |
| `close()` | closes; requests still pending end with `ENAGA_FILE_CANCELLED` |
| `is_open()` | whether it is open |
| `size()` | its size in bytes |
| `seek(position)` | moves to a byte position; `ENAGA_FILE_BUSY` while requests are pending |
| `read(buffer, bytes, done, user)`, `read<&T::done>(buffer, bytes, object)` | background read from the position: `done` gets the bytes read (fewer at the end) |
| `write(data, bytes, done, user)`, `write<&T::done>(data, bytes, object)` | background write at the position |

## Streams

A **stream** plays a file into `audio`, or records one from it, through a ring buffer the app gives. Enaga keeps a
playback ring topped up, and a recording ring drained to the card, in the main loop; in `audio` the app reads or
writes the ring, which never waits. The card's speed (about 7,600 KiB/s reading, 2,000 writing on a unit) allows
dozens of playback streams of 16-bit stereo and several recordings.

**`enaga::files::Stream`**: closed when the object goes; not copyable. Up to `ENAGA_STREAMS` (8) per app, besides
the 8 files. `open`, `create`, `status` and `close` are for the main context; `read` and `write` for either, one
context per stream.

| Member | Does |
|---|---|
| `open(path, offset, ring, ring_bytes)` | playback from byte `offset` (e.g. a WAV's `data_offset`) |
| `create(path, reserve, ring, ring_bytes)` | recording into a new file that starts with `reserve` zero bytes (room for a header) |
| `ready()` | playback: the ring has been filled, start reading |
| `read(out, bytes)` | in `audio`: the bytes copied; fewer is an underrun, or the end of the file |
| `write(data, bytes)` | in `audio`: the bytes taken; fewer is an underrun (the ring was full) |
| `status()` | a `Status` (`enaga_stream_status_t`): `state`, `position` (bytes read or written so far), `buffered` (bytes in the ring), `underruns` |
| `close(header = nullptr, header_bytes = 0)` | a recording is written out to the end, then `header` over the file's first bytes; this waits for the card |
| `is_open()` | whether it is open |

`state` is `ENAGA_STREAM_FILLING` (playback, not yet full), `ENAGA_STREAM_RUNNING`, `ENAGA_STREAM_ENDED`
(playback, whole file read out), or a negative error (the card went, failed or is full).

The ring is a power of two of at least `ENAGA_STREAM_RING_MIN` (4,096) bytes, from the app's heap, and belongs to
Enaga until `close`. Size it for the card's slowest moments: 64 KiB holds about a third of a second of 16-bit
stereo.

```cpp
ring_ = new uint8_t[1 << 16];
stream_.open("/SAMPLES/Loop.wav", wav.data_offset, ring_, 1 << 16);    // in init(); wait for ready()
// in audio():
uint32_t got = stream_.read(pcm_, frames * 4);                          // 16-bit stereo: 4 bytes a frame
```

## WAV headers

In `<enaga/wav.hpp>`, namespace `enaga::wav`. Fields are read byte by byte, as WAV is packed (the C674x silently
rounds misaligned word accesses down).

| Name | Is |
|---|---|
| `Format` | `encoding` (`pcm`: 8-bit unsigned, 16/24/32-bit signed; `floating`: 32-bit), `channels`, `rate`, `bits`, `data_offset` (where the samples start), `data_bytes`, `frame_bytes()` |
| `Encoding` | `pcm` = 1, `floating` = 3 |
| `parse(start, bytes, Format &out)` | `true` for a RIFF WAVE whose `fmt ` chunk and the start of its `data` chunk lie in the bytes given, in a format above (WAVE_FORMAT_EXTENSIBLE included). Other chunks are skipped |
| `header(out, format, data_bytes)` | writes the 44-byte header of a file holding `data_bytes` of samples |

Recording a WAV: `create(path, 44, ...)`, write samples, then `wav::header(head, format, status().position)` and
`close(head, 44)`.

## App settings

`enaga::Settings` in `<enaga/settings.hpp>`: a small key/value store kept as text, `KEY=VALUE` lines, in the
app's data folder (`SETTINGS.TXT`, or the name given to the constructor), readable and editable on a computer.
Main context only.

```cpp
enaga::Settings settings;                              // a member of the app
bool init() override { settings.load(); tempo = settings.get("TEMPO", 120); return true; }
void stop() override { settings.set("TEMPO", tempo); settings.save(); }
```

| Member | Does |
|---|---|
| `Settings(file = "SETTINGS.TXT")` | the file name in the data folder |
| `load()` | reads the file; a missing one is empty settings. `ENAGA_FILE_OK` or an error |
| `save()` | writes the file (safely, as `files::save`) if anything changed since `load()` or the last `save()` |
| `get(key, fallback)` | a whole-number value, or `fallback` if missing or not a number |
| `text(key, fallback = "")` | the text value |
| `set(key, value)` | a number or text; `false` if the key or value is invalid or 32 keys are in use |
| `changed()` | whether there is something to save |

Limits: `max_keys` (32) keys; keys up to `key_bytes` - 1 (23) bytes without `=` or a line break; text values up
to `value_bytes` - 1 (31). Enaga's own settings are separate and not here.

## MIDI

The MIDI IN and OUT jacks (DIN) and USB MIDI: plugged into a computer, the unit is a class-compliant USB MIDI
device ("Enaga SmplTrek", or the device's name), with no driver needed. The user's MIDI IN setting (DIN, USB or
both) says which ports reach the app, and MIDI OUT where its messages go; both default to BOTH. In
`<enaga/app.hpp>` (receiving) and `<enaga/midi.hpp>` (sending).

### Receiving: `on_midi(const MidiMessage &)` and `on_midi_sysex`

Every message but System Exclusive comes to `on_midi`, whole, in the main context, also while the Enaga menu is
open, so notes from outside keep playing. Enaga's parser follows the MIDI 1.0 specification:
- **Running status:** expanded, so every message has its status byte.
- **Real-time bytes:** messages of their own, even in the middle of another message or a SysEx.
- **Note on with velocity 0:** arrives as a note off (status `0x8n`, velocity 0).
- **Undefined bytes** (F4, F5, F9, FD) and data bytes without a status are dropped.
- **Active sensing** (FE) is handled by Enaga and not delivered. Once it has been seen, a gap of over 300 ms gives
  `on_midi` All Notes Off (CC 123) on all 16 channels, as the specification asks of a receiver.

`MidiMessage` has `status`, `data1`, `data2` (unused ones 0) and `port` (`ENAGA_MIDI_DIN` or `ENAGA_MIDI_USB`),
and readers for the common cases:

| Member | Gives |
|---|---|
| `type()` | `MidiMessage::note_off`, `note_on`, `poly_pressure`, `control_change`, `program_change`, `channel_pressure`, `pitch_bend`, or `system` for F0-FF (then see `status`) |
| `channel()` | 0-15 |
| `note()`, `velocity()` | for notes; `pressure()` for poly and channel pressure |
| `controller()`, `value()` | for control changes; `program()` for program changes |
| `bend()` | for pitch bend, -8192 to 8191 |

`on_midi_sysex(port, data, bytes, start, end)` gets System Exclusive in chunks of up to `ENAGA_SYSEX_CHUNK` (256)
bytes as they arrive. The first chunk (`start`) begins with F0. The last (`end`) ends with F7, unless the message
was cut short by another status byte. The data is valid only during the call.

### Sending: `enaga::midi::`

Each call queues one whole message for the ports MIDI OUT names and returns `true`. If a queue (2 KiB each) is
full it queues nothing and returns `false`: a message is never cut. With no computer connected, USB is skipped,
and so is a computer that has stopped taking what the unit sends (no program has the port open, say): after
100 ms without progress USB gets only what fits, so it never holds up the MIDI OUT jack.
USB MIDI out leaves within about a millisecond (a main-loop pass and a USB frame). Real-time messages (clock, start, continue, stop) go ahead
of whatever is queued, behind at most the byte already being sent (0.32 ms). Main or audio context. Channels are
0-15 and data bytes are masked to 7 bits.

| Function | Sends |
|---|---|
| `note_on(ch, note, velocity)`, `note_off(ch, note, velocity = 0)` | notes |
| `poly_pressure(ch, note, pressure)`, `channel_pressure(ch, pressure)` | aftertouch |
| `control_change(ch, controller, value)`, `program_change(ch, program)` | controls and programs |
| `pitch_bend(ch, bend)` | -8192 to 8191 |
| `song_position(sixteenths)`, `song_select(song)` | system common |
| `clock()`, `start()`, `continue_()`, `stop()`, `realtime(byte)` | real-time |
| `sysex(data, bytes)` | a whole SysEx, F0 to F7 |
| `send(data, bytes, ports = 0)` | raw bytes: one or more whole messages, each with its status byte; `ports` 0 means the user's routing, or `ENAGA_MIDI_DIN` / `ENAGA_MIDI_USB` bits |

**Channels.** An app receives and sends on all 16 channels: Enaga filters nothing, `message.channel()` says
where a message came from, and every sending helper takes its channel. So one app can take several tracks from an
outside sequencer (channel 1 to the drums, 2 to the bass) or play several synths. `enaga::midi::channel()` is the
user's MIDI CH setting (0-15), only a default: the channel a single-voice app uses when it has no reason to choose.
An app with several tracks keeps their channels in its own settings (`<enaga/settings.hpp>`), as the original
firmware keeps a channel per track. MIDI THRU (a setting, OFF by default) copies what arrives at the MIDI IN jack to the MIDI OUT jack,
merged with the app's own messages without breaking either. SysEx is not copied, and USB is not involved.

## Clock and transport

One tempo clock for the whole unit, counted in ticks of `ENAGA_CLOCK_PPQN` (96) to the quarter note. In
`<enaga/clock.hpp>`, as `enaga::clock::`.

**Who drives it** is the user's CLOCK SRC setting:
- **INT:** the app.
- **MIDI:** MIDI clock and transport at MIDI IN.
- **SYNC:** pulses at SYNC IN, 2 to the quarter note.

**Controlling it (INT).** The app calls `start()` (from tick 0), `stop()`, `continue_()`, `locate(ticks)` and
`set_tempo(bpm)` (20-300; 120 to begin with). With an outside source these do nothing and return `false`. The
clock stops when the app stops.

**Following an outside clock.** The tempo is measured from the clock or pulses:
- **MIDI clock:** the mean of the last 24 intervals, kept while stopped, so a START runs at the right tempo from
  its first tick.
- **SYNC pulses:** the mean of the last 2 intervals.

The clock runs at that tempo but never passes the tick of the next clock or pulse. If one comes early and finds it
more than one clock or pulse behind, it catches up at once, every tick still given out (the original firmware's
way).
- **MIDI:** START, STOP, CONTINUE and Song Position Pointer move it.
- **SYNC:** the first pulse starts it. A gap of more than 2.5 intervals stops it (at least 100 ms; 2 s before
  the second pulse).

**Following it in the audio callback.** `ticks(out, max)` (audio context) lists the ticks that fall in the block
being computed. Each `Tick` has `.tick` (its number) and `.frame` (where in the block it falls), so a step can
start on the right sample. At ordinary tempos a block holds at most one or two. A catch-up gives several at frame
0, at most 32 a block, so ask for enough. `state()` (main or audio context) returns a `State`:
- `.running`;
- `.source` (`ENAGA_CLOCK_INTERNAL`, `ENAGA_CLOCK_MIDI` or `ENAGA_CLOCK_SYNC`);
- `.tempo` in BPM;
- `.position` in whole ticks, and `.fraction`.

`running()`, `tempo()`, `position()` and `source()` read single fields.
`ppqn` (96) and `sixteenth` (24) are the units.

**Transport events.** `on_transport(what, position)` (main context) reports every change, whoever made it:
`ENAGA_TRANSPORT_START`, `ENAGA_TRANSPORT_STOP`, `ENAGA_TRANSPORT_CONTINUE` or `ENAGA_TRANSPORT_LOCATE`, at a
position in ticks.

**Out.** These follow the clock whatever its source, so the unit can also turn SYNC into MIDI clock:
- **CLOCK OUT on:** MIDI clock every 4 ticks (24 to the quarter note), START, STOP, and Song Position Pointer
  before CONTINUE or on a locate while stopped.
- **SYNC OUT on** (the default): a 15 ms pulse every 48 ticks, at least 60 ms apart.

**Polarity.** The user's POL IN and POL OUT settings (RISE or FALL, both RISE by default) choose the SYNC IN edge
that counts and which way SYNC OUT pulses, with the original firmware's meaning for each label. Apps never see them.

Both are sent from the audio interrupt: exact on average, and late by less than one block (1.33 ms). With CLOCK
SRC MIDI, CLOCK OUT and MIDI THRU both on would send the clock twice.

## UI toolkit

`<enaga/ui.hpp>`, as `enaga::ui::`: a list, a page of parameters on the knobs, a confirmation dialog and text
entry. They're drawn on the `Canvas` with Enaga's font, so apps look alike, and are header-only, with nothing in
the firmware. `examples/widgets` uses all four.

**Driving them.** An app passes its own key and knob callbacks to the widget it is showing and draws it in `draw`.
Each `key(key, down)` and `encoder(encoder, delta)` returns an `Action`:
- `Action::none`: not for this widget, or nothing changed;
- `moved`, `changed`;
- `chosen` (OK on a list entry, or text accepted);
- `back` (CLR on a list);
- `yes` and `no` (a dialog answered).

The widgets keep pointers to what the app gives them (names, parameters, values), which must outlive them. Main
context only. The controls follow the original firmware's: knobs A-D for parameters, the selection buttons under
the screen for groups, UP / DOWN or VALUE in lists, OK and CLR to answer. F1 stays the app's.

```cpp
enaga::ui::List list;                                  // members of the app
void on_key(int key, bool down) override {
    if (list.key(key, down) == enaga::ui::Action::chosen) open(list.cursor());
}
void on_encoder(int encoder, int delta) override { list.encoder(encoder, delta); }
void draw(enaga::Canvas &canvas) override { canvas.clear(); list.draw(canvas); }
```

**`List`**: a scrolling list with a title.
- **Movement:** UP / DOWN or VALUE move the cursor, stopping at the ends. OK gives `chosen`, CLR `back`.
- **Entries:** `set_items(names, count)` for fixed strings, or `set_items(count, label, user)` with a `Label`
  function, `void (int index, char *out, int size, void *user)`, for entries that change. `set_count` changes
  how many there are and `set_title` sets the title.
- **Cursor:** `cursor()` and `select(index)`. `count()` and `name(index, out, size)` read the list back.
- **Drawing:** `draw(canvas, top, bottom)` shows the title and as many rows (`row_height`, 11 px) as fit, the
  cursor's on a dim bar, with marks at the right when there is more above or below.

**`ParamPage`**: the original firmware's way of editing.
- **Groups:** the page's parameters go in groups of four, in order, on knobs A-D. The selection buttons flip
  between groups (wrapping). Turning a knob changes its parameter, clamped; pushing it returns the parameter to its
  initial value.
- **Setting up:** construct it, or call `set(title, params, count, values)`, with an array of `Param` and the
  app's own `int` values, one per parameter. The page only changes those values.
- **Changes:** after `Action::changed`, `changed()` is the parameter. `set_value(param, value)` sets one from the
  app, clamped. `value(param)` reads one.
- **Groups from the app:** `groups()`, `group()` and `show_group(n)`.
- **Drawing:** `draw(canvas, top)` shows the title and "group/groups". Then a row per knob: its letter, the name
  with a bar for the value's place in its range, and the value at the right, brighter when it is not the
  initial one.
- **A `Param`** has, in this order, `name` (12 characters show), `min`, `max` and `initial`, and how to show the
  value: `labels` (a name per step from `min`), `format`, a `void (*)(int value, char *out)` writing up to 12
  characters and a terminating 0, or the number with an optional `unit`. So
  `{"CUTOFF", 0, 127, 64, nullptr, hertz}` or `{"LEVEL", 0, 100, 80, nullptr, nullptr, "%"}`. The page's
  `format(param, out)` gives the text shown.

**`Dialog`**: a question in a box over the screen.
- `open(message, yes = "OK", no = "CLR")`. The message has up to three lines, split at `'\n'`, 19 characters each.
- While it is `active()`, `key` takes every key. OK gives `yes`, CLR `no`, and both close it; `close()` closes it
  without an answer.
- `draw(canvas)` after the page, so it sits on top. `encoder` does nothing.

**`TextEntry`**: editing a short name.
- `open(title, initial, chars)` takes up to `max_chars` (16). Lower case in `initial` becomes upper case, and
  characters the font lacks become spaces.
- **Moving:** LEFT / RIGHT or knob A move the cursor.
- **Changing:** VALUE, knob B or UP / DOWN step the character under it through `characters`: space, A-Z, 0-9 and
  `- + . # !`. All are safe in file names.
- **Answering:** OK gives `chosen` and CLR `no`, both closing it. While it is `active()` it takes every key.
- **Reading:** `text()` is the text without trailing spaces, and `cursor()` the position.
- `draw(canvas, top)` shows the title, the text in boxes with the cursor's lit, and the controls.

**Layout helpers.** `screen_width` (128), `char_width` (6, the font's advance), `line_chars` (21 to a line), and
`text_right(canvas, y, text, grey, x_end)` for right-aligned text. The font has A-Z, 0-9, space and
`! # + - . / : = ? % >`.

## DSP library

`<enaga/dsp.hpp>`, namespace `enaga::dsp`. Everything is float, at any sample rate. Each class wraps a plain C
state struct from `<enaga/dsp.h>` (its public `state_` member) and calls the C functions, so C code can work on
the same object.

**Conventions**

- **Process in blocks.** Methods that take a buffer and a count (`render_add`, `process`, `render`, `read`) are
  *block functions*, built with TI's cl6x when it is installed: several times faster than GCC's code, and the
  same samples either way. Per-sample `step()` calls are for code that needs them, such as feedback loops.
- **`_add` adds** into the output buffer; clear it first (`enaga_clear`) when starting a mix.
- **Coefficients at control rate.** Filter and envelope settings cost a reciprocal or an exponential; compute them
  when a parameter changes, or once per block, not per sample.
- **Nothing divides.** The C674x has no divide instruction, and a division stops cl6x from pipelining a loop. The
  library uses `reciprocal`; oscillators take both `dt` and `inv_dt` so the caller computes `1 / dt` once.
- **Buffers on the heap, in `init()`.** Delay lines, effects and FIR history allocate when initialised; never in
  `audio`.
- Unless stated, buffer arguments must not overlap; "out may be in" marks the ones that work in place.

### Math

Fast float math without libm, in `enaga::dsp` (C: `enaga_` + the same name, `<enaga/dsp/math.h>`).

| Function | C | Gives | Accuracy |
|---|---|---|---|
| `exp2(x)` | `enaga_exp2` | 2^x, x clamped to ±126 | relative 3e-7 |
| `exp(x)` | `enaga_exp` | e^x | as `exp2` |
| `log2(x)` | `enaga_log2` | log₂ x, x > 0 | absolute 1e-4 |
| `semitones(s)` | `enaga_semitones` | the frequency ratio of `s` semitones | as `exp2` |
| `sin_turns(t)`, `cos_turns(t)` | `enaga_sin_turns`, `enaga_cos_turns` | sin and cos of 2π t, `t` in turns, any value | absolute 4e-6 |
| `tan_pi(x)` | `enaga_tan_pi` | tan(π x) for 0-0.49 (clamped): a filter's prewarped cutoff, x = cutoff / rate | relative 1e-4 |
| `reciprocal(x)` | `enaga_reciprocal` | 1 / x without a division, 1e-37 < \|x\| < 1e37 | relative 2e-7 |
| `soft_clip(x)` | `enaga_soft_clip` | x for small x, smoothly to ±1 at \|x\| ≥ 1.5 | exact cubic |
| `clamp(x, lo, hi)`, `lerp(a, b, t)`, `floor_int(x)` | `enaga_clamp`, `enaga_lerp`, `enaga_floor_int` | the usual | |
| `pi` | `ENAGA_PI` | π | |

`Random(seed)`: xorshift32. `next()` (32 bits), `bipolar()` [-1, 1), `unipolar()` [0, 1),
`add_noise(out, n, level)` (adds white noise, a block function), `state()` (the state, for C code).

### Buffers

C functions from `<enaga/dsp.h>`, callable from C++ as they are. Block functions.

| Function | Does |
|---|---|
| `enaga_clear(buf, n)` | buf = 0 |
| `enaga_add(out, in, n)` | out += in |
| `enaga_mix(out, in, n, gain)` | out += gain × in |
| `enaga_scale(buf, n, gain)` | buf ×= gain |
| `enaga_multiply(buf, gain, n)` | buf ×= gain, sample by sample (a VCA from an envelope buffer) |
| `enaga_mix_product(out, a, b, n, gain)` | out += gain × a × b (ring modulation) |
| `enaga_soft_clip_buffer(buf, n, drive)` | buf = soft_clip(drive × buf) |
| `enaga_ramp_stereo_add(in, left, right, n, &gl, &gr, step_l, step_r)` | pans `in` into `left` / `right` with gains stepping by `step_l` / `step_r` each sample (click-free gain changes); `gl` and `gr` are left at their final values |
| `enaga_exp2_buffer(buf, n)`, `enaga_reciprocal_buffer(buf, n)`, `enaga_tan_pi_buffer(buf, n)` | the math function on every value |
| `enaga_noise_add(&state, out, n, level)` | adds white noise (`Random::add_noise`) |

The math buffers are for control values too: gather every voice's pitch or cutoff into one array and convert them
in one call. cl6x's fast loops need 10 values or more, so pad a short array (16 values cost less than 8).

### Oscillators: `Oscillator`, `Wave`

Saw and pulse are PolyBLEP band-limited; triangle and sine are plain. `dt` is the frequency in cycles per sample
(frequency / rate), in (0, 0.5), and `inv_dt` its reciprocal. The phase is a 32-bit fraction of a turn and wraps by
itself.

| Member | Does |
|---|---|
| `render_add(out, n, wave, dt, inv_dt, level, width = 0.5)` | adds `level` × the wave; `wave` is `Wave::saw`, `pulse`, `triangle` or `sine`; pulse `width` 0.05-0.95 |
| `sync_add(out, n, master, master_dt, master_inv_dt, carry, wave, dt, inv_dt, level, width = 0.5)` | hard sync: this oscillator's phase restarts whenever `master`'s wraps, band-limited. Call it **before** rendering the master's own block (it reads the master without moving it). `carry` is a float the caller keeps per voice, zeroed at the note's start |
| `advance(n, dt)` | moves the phase on silently, as `render_add` would (a muted sync master) |
| `table_add(out, n, table, bits, dt, level)` | a wavetable the app fills: 2^`bits` samples of one cycle (bits 1-24) plus a guard sample equal to the first; linear interpolation; `dt` in (-0.5, 0.5), negative runs backwards. Band-limiting is the table's job |
| `morph_add(out, n, a, b, bits, morph, dt, level)` | two tables of one size crossfaded, (1 - morph) × a + morph × b |
| `phase()`, `reset(phase = 0)` | the phase in turns [0, 1) |

### Filters

**`Svf`, `SvfCoefficients`**: the topology-preserving (trapezoidal) state-variable filter, stable under fast
modulation, with low-, band- and high-pass at once.

| Member | Does |
|---|---|
| `SvfCoefficients::make(cutoff, resonance, rate)` | cutoff in Hz, resonance 0 (none) to 1 (just short of self-oscillation) |
| `SvfCoefficients::from_g(g, k)` | from g = tan(π cutoff / rate) and k = 1 / Q (2 for none, towards 0) |
| `Svf::process(in, out, n, c, output = low_pass)` | a block of one output, an `Svf::Output`: `Svf::low_pass`, `band_pass` or `high_pass` (out may be in) |
| `Svf::cascade(a, ca, b, cb, in, out, n, output = low_pass)` | two in series (a 24 dB filter from two 12 dB ones), about twice as fast as two `process` calls |
| `Svf::step(x, c)` | one sample: an `Svf::Out` with `low`, `band`, `high`; `low(x, c)` the low-pass alone |
| `Svf::reset()` | clears the state |

For many filters at once, `enaga_svf_coefficients_buffer(g, k, c, n)` computes `n` sets from arrays of g and k
(`SvfCoefficients` has the C struct's layout, so an array of either works).

**`OnePole`**: y += a (x - y), for smoothing and damping. `OnePole::coefficient(cutoff, rate)` gives
a = 1 - exp(-2π cutoff / rate); `step(x, a)`, `process(in, out, n, a)` (out may be in), `value()`,
`reset(y = 0)`.

**`Biquad`, `BiquadCoefficients`**: Audio EQ Cookbook shapes, direct form I.
`BiquadCoefficients::make(type, frequency, q, rate, gain_db = 0)` with `type` one of `low_pass`, `high_pass`,
`band_pass` (0 dB at the centre), `notch`, `all_pass`, `peak`, `low_shelf`, `high_shelf`; frequency in Hz (clamped
to 0.0001-0.49 of the rate); q > 0, 0.7071 Butterworth (and the steepest shelf without overshoot); `gain_db` for
peak and the shelves. `Biquad::process(in, out, n, c)` (out may be in), `Biquad::cascade(a, ca, b, cb, in, out,
n)` (fourth order, faster than two calls), `step(x, c)`, `reset()`.

**`Fir`**: out[i] = Σ taps[k] × in[i - k] on taps the app keeps (they must outlive the filter).
`init(taps, length)` allocates the history on the heap (call it in `init()`); `process(in, out, n)` (out may be
in). Sums in the order k = 0, 1, ... as the plain loop would.

### Envelopes and LFOs

Both run at whatever rate the caller steps them: per sample for an amplitude envelope, once per block for
modulation. Their settings are computed for that rate.

**`Envelope`, `EnvelopeShape`**: ADSR from one-pole segments; the attack aims past full level
(`kAttackTarget`, 1.3) and stops at 1, a curve like an analogue envelope's.

| Member | Does |
|---|---|
| `EnvelopeShape::make(attack_s, decay_s, sustain, release_s, step_rate)` | times in seconds (attack to full, decay and release to within 1 % of their target), sustain 0-1, `step_rate` the rate it is stepped at |
| `gate_on(retrigger_from_zero = false)` | starts the attack, from the current level unless told to restart from 0 |
| `gate_off()` | starts the release |
| `kill()` | silent and idle at once |
| `render(out, n, shape)` | `n` steps into `out` (a block function) |
| `step(shape)` | one step; returns the level |
| `stage()`, `active()`, `level()` | the `Envelope::Stage`: `Envelope::idle`, `attack`, `decay`, `sustain`, `release`; whether not idle; the level |

The shape's constants `kAttackSpan` and `kSpan` are what `make` uses to turn times into coefficients.

**`Lfo`, `LfoShape`**: bipolar, -1 to 1. `step(shape, dt, random)` with `dt` = rate in Hz / step rate and a
`Random` for sample-and-hold; `value(shape)` without stepping; `phase()`, `reset(phase = 0)`. Shapes:
`LfoShape::sine`, `triangle`, `saw`, `square`, `sample_hold`.

### Delays and effects

Construct or `init` these in `init()`: their buffers come from the heap.

**`DelayLine`**: a ring of a power-of-two size.

| Member | Does |
|---|---|
| `DelayLine(max_samples)`, `allocate(max_samples)` | room for delays up to `max_samples` |
| `capacity()` | the longest delay it holds |
| `write(x)`, `tap(delay)`, `tap_linear(delay)` | per sample: write one, read the one `delay` writes ago (0 = the last), or a fractional delay interpolated |
| `read(delay, out, n)`, `write(in, n)` | blocks: read a block's taps first, then write the block. Sample i sees what `tap(delay)` would, provided delay ≥ n - 1 |
| `read_linear(delay, out, n)` | a modulated read (chorus, flanger, vibrato): out[i] = `tap_linear(delay[i])`, each delay[i] ≥ i |

**`StereoDelay`**: echoes with feedback through a low-pass, optional ping-pong; the time glides to new settings
(bending the pitch like tape, not clicking). `init(max_seconds, rate)`; `set(seconds, feedback, tone, ping_pong)`
(feedback 0-0.95, tone 0 dark to 1 bright); `process(in_l, in_r, out_l, out_r, n, mix)` adds `mix` × the echoes.

**`Chorus`**: each channel through an 8 ms delay swept by a sine, the two a quarter turn apart, so mono comes out
wide. `init(rate)`; `set(sweep_hz, depth)` (depth 0-1, up to ±5 ms); `process(in_l, in_r, out_l, out_r, n, mix)`
adds `mix` × the chorus (out may be in).

**`Reverb`**: four Schroeder all-passes into an eight-line (`kLines`) feedback delay network. `init(rate)`;
`set(decay_s, damping)` (time to fall 60 dB, 0.2-30 s; damping 0 bright to 1 dark);
`process(in_l, in_r, out_l, out_r, n, mix)` adds `mix` × the reverb of in_l + in_r.

**`Memory`**: the zeroed heap block behind these classes, freed with its owner; `allocate(floats)`. Use it for C
state of your own (an `enaga_delay_line` in a `*.ti.c` loop, say).

### Writing inner loops for cl6x

Files named `*.ti.c` in an app's folder are built by TI's cl6x when it is installed (GCC otherwise, which must also
compile them). They are C and export plain C functions, since C++ objects don't mix between the compilers; they
can include `<enaga/dsp.h>` and work on a class's `state_`. cl6x pipelines only loops without branches inside:
write `cond ? a : b` rather than `if`, and use the library's math, which never divides.

## The C interface

`<enaga/abi.h>` is the boundary under the C++ layer: plain C, since C++ ABIs are not stable across compilers. A C
app defines `const enaga_app_t *enaga_app_main(void)` (what `ENAGA_APP` writes) returning its callback table; the
runtime calls it after the static constructors.

Its types: the enums `enaga_key`, `enaga_led`, `enaga_device`, `enaga_input_source`, `enaga_input_level`,
`enaga_midi_port`, `enaga_sysex_flags` (`ENAGA_SYSEX_START`, `ENAGA_SYSEX_END`, `ENAGA_SYSEX_CUT`), `enaga_clock_source`,
`enaga_clock_command` (`ENAGA_CLOCK_START`, `ENAGA_CLOCK_STOP`, `ENAGA_CLOCK_CONTINUE`, `ENAGA_CLOCK_LOCATE`,
`ENAGA_CLOCK_TEMPO`: BPM x 100), `enaga_transport`, the structs `enaga_clock_state_t` and `enaga_clock_tick_t`
(tags `enaga_clock_state`, `enaga_clock_tick`),
`enaga_event_type`, `enaga_file_error`,
`enaga_file_mode` and `enaga_stream_state`, and the structs `enaga_app_t`, `enaga_host_t`, `enaga_event_t`,
`enaga_package_header_t`, `enaga_file_info_t` and `enaga_stream_status_t` (also by their tags, `struct
enaga_app`, `enaga_host`, `enaga_event`, `enaga_package_header`, `enaga_file_info`, `enaga_stream_status`).

**`enaga_app_t`** (the app's table): `abi_version` (`ENAGA_ABI_VERSION`), `size` (`sizeof(enaga_app_t)`), `name`,
`context` (passed to every callback), then `init` (0 on success), `update`, `draw` (the canvas as
`uint8_t *`), `stop`, `event`, `audio` and `midi_sysex` (port, data, bytes, `enaga_sysex_flags`; called only if
`size` covers it); any may be 0.

**`enaga_event_t`** (to `event`): `type`, `id`, `value`, `ms` (when Enaga read it).

| `type` | `id` | `value` |
|---|---|---|
| `ENAGA_EVENT_KEY` | `enaga_key` | 1 down, 0 up |
| `ENAGA_EVENT_ENCODER` | 0-3 A-D, 4 VALUE | signed detents |
| `ENAGA_EVENT_PAD` | pad 0-14 | raw reading, 0-16383 |
| `ENAGA_EVENT_PAD_PRESS` | pad | velocity 1-127 |
| `ENAGA_EVENT_PAD_RELEASE` | pad | 0 |
| `ENAGA_EVENT_CARD` | 0 | 1 inserted and mounted, 0 removed, or a negative `enaga_file_error` |
| `ENAGA_EVENT_PAD_PRESSURE` | pad | pressure 0-127 |
| `ENAGA_EVENT_MIDI` | `enaga_midi_port` | status \| data1 << 8 \| data2 << 16 |
| `ENAGA_EVENT_TRANSPORT` | `enaga_transport` | the position in ticks |

**`enaga_host_t`** (Enaga's table, `enaga::host()` in C++; valid until the app is unloaded): `abi_version`,
`size`, `heap` and `heap_size` (the app's heap region, which the runtime manages), `sample_rate`, `block_frames`,
and the functions the C++ layer wraps:

| Host function | C++ |
|---|---|
| `log`, `ms`, `fatal`, `audio_load`, `audio_load_peak` | `enaga::log`, `ms`, `fatal`, `audio_load`, `audio_load_peak` |
| `text(canvas, x, y, text, grey)` | `Canvas::text` |
| `led(led, red, green, blue)` | `pad_led`, `step_led`, `button_led`, `vol_led`, `mic_led`, `value_led` |
| `file_open`, `file_close`, `file_size`, `file_seek`, `file_read`, `file_write` | `files::File` |
| `file_info`, `file_remove`, `file_make_folder`, `file_read_all`, `file_save`, `file_data_folder` | `files::info`, `remove`, `make_folder`, `read_all`, `save`, `data_folder` |
| `file_load`, `file_list` | `files::load`, `list` |
| `stream_open`, `stream_create`, `stream_read`, `stream_write`, `stream_status`, `stream_close` | `files::Stream` |
| `open_menu`, `set_busy`, `speaker`, `headphones`, `device`, `input`, `battery` | `enaga::system::` the same names |
| `midi_send(ports, data, bytes)` (0 queued, -1 no room), `midi_channel` | `enaga::midi::send` and the helpers, `channel` |
| `clock_control(command, value)`, `clock_state`, `clock_ticks(ticks, max)` | `enaga::clock::` control, `state`, `ticks` |

**The package** on the card is an `enaga_package_header_t` (64 bytes: `magic` `ENAGA_PACKAGE_MAGIC`,
`header_size`, `abi_version`, `load_address`, `load_size`, `bss_size`, `entry_offset`, `crc32`, `app_version`,
`flags`, `name[ENAGA_NAME_BYTES]`) followed by the image, linked at `ENAGA_APP_BASE`; `tools/build_app.py` writes
it. With `ENAGA_PACKAGE_ICON` in `flags`, `ENAGA_ICON_BYTES` (128) of icon follow the image: `ENAGA_ICON_SIZE` x `ENAGA_ICON_SIZE` (16 x 16) greys, row by
row, two pixels a byte with the left one in the high nibble. The entry point is an `enaga_entry_t`, `const enaga_app_t *(const enaga_host_t *)`, in the runtime.

**Memory**: the app's image starts at `ENAGA_APP_BASE` (0xC0400000) and its heap runs from the end of the image to
`ENAGA_APP_LIMIT` (0xC8000000), about 124 MiB in all.

**The DSP library in C** (`<enaga/dsp.h>`): each class above is a state struct and functions named after it.

| C++ | C state | C functions |
|---|---|---|
| `Oscillator` | `enaga_oscillator` | `enaga_oscillator_add`, `enaga_oscillator_sync_add`, `enaga_oscillator_advance`, `enaga_oscillator_phase`, `enaga_oscillator_reset`, `enaga_wavetable_add`, `enaga_wavetable_morph_add`; waves `ENAGA_WAVE_SAW`, `ENAGA_WAVE_PULSE`, `ENAGA_WAVE_TRIANGLE`, `ENAGA_WAVE_SINE` |
| `Svf` | `enaga_svf`, `enaga_svf_coefficients` | `enaga_svf_step`, `enaga_svf_process`, `enaga_svf_cascade`, `enaga_svf_coefficients_buffer`; outputs `ENAGA_SVF_LOW`, `ENAGA_SVF_BAND`, `ENAGA_SVF_HIGH` |
| `OnePole` | `enaga_one_pole` | `enaga_one_pole_coefficient`, `enaga_one_pole_step`, `enaga_one_pole_process` |
| `Biquad` | `enaga_biquad`, `enaga_biquad_coefficients` | `enaga_biquad_design`, `enaga_biquad_step`, `enaga_biquad_process`, `enaga_biquad_cascade`; types `ENAGA_BIQUAD_LOW_PASS`, `ENAGA_BIQUAD_HIGH_PASS`, `ENAGA_BIQUAD_BAND_PASS`, `ENAGA_BIQUAD_NOTCH`, `ENAGA_BIQUAD_ALL_PASS`, `ENAGA_BIQUAD_PEAK`, `ENAGA_BIQUAD_LOW_SHELF`, `ENAGA_BIQUAD_HIGH_SHELF` |
| `Fir` | `enaga_fir` | `enaga_fir_init`, `enaga_fir_memory` (history floats), `enaga_fir_process`; `ENAGA_FIR_CHUNK` |
| `DelayLine` | `enaga_delay_line` | `enaga_delay_size`, `enaga_delay_init`, `enaga_delay_capacity`, `enaga_delay_write`, `enaga_delay_tap`, `enaga_delay_tap_linear`, `enaga_delay_read`, `enaga_delay_write_block`, `enaga_delay_read_linear` |
| `StereoDelay` | `enaga_stereo_delay` | `enaga_stereo_delay_memory`, `enaga_stereo_delay_init`, `enaga_stereo_delay_set`, `enaga_stereo_delay_process` |
| `Chorus` | `enaga_chorus` | `enaga_chorus_memory`, `enaga_chorus_init`, `enaga_chorus_set`, `enaga_chorus_process` |
| `Reverb` | `enaga_reverb` | `enaga_reverb_memory`, `enaga_reverb_init`, `enaga_reverb_set`, `enaga_reverb_process`; `ENAGA_REVERB_LINES`, `ENAGA_REVERB_DIFFUSERS` |
| `Envelope` | `enaga_envelope`, `enaga_envelope_shape` | `enaga_envelope_step`, `enaga_envelope_render`; stages `ENAGA_ENV_IDLE`, `ENAGA_ENV_ATTACK`, `ENAGA_ENV_DECAY`, `ENAGA_ENV_SUSTAIN`, `ENAGA_ENV_RELEASE`; `ENAGA_ENV_ATTACK_TARGET` |
| `Random` | a `uint32_t` | `enaga_xorshift`, `enaga_bipolar`, `enaga_unipolar`, `enaga_noise_add` |

The `_memory` functions give the floats of zeroed memory the `_init` functions need; the caller provides it.
`math.h` also has `enaga_float_bits` and `enaga_bits_float` (a float's bits and back).

## Language and runtime

- **C++20** with `-fno-exceptions -fno-rtti`, and C11. The freestanding standard library: `<array>`,
  `<algorithm>`, `<span>`, `<optional>`, `<variant>`, `<type_traits>`, `<bit>`, `<atomic>`, `<numbers>`,
  `<cstdint>` and the like. No allocating containers (`std::vector`, `std::string`), no iostreams.
- **Memory:** `new`, `delete`, `malloc` and `free` work on the app's heap (everything above the image, about
  124 MiB). Running out calls `fatal("out of memory")`. `memcpy`, `memmove`, `memset`, `memcmp` and `strlen` are
  there; the rest of libc is not.
- **No libm:** use the DSP library's math.
- **Stack:** a function's frame is limited to 16 KiB, and VLAs and `alloca` are refused at build time (Enaga's
  interrupt entry uses the 16 KiB below the stack pointer). Put large arrays in statics or on the heap.
- **Static constructors** run before the app object is made; **destructors never run**.
- **Alignment:** a misaligned 32-bit access does not fault; it silently uses the aligned address below, and a
  misaligned store overwrites the word below. Read and write packed data (file formats, SysEx) with
  `<enaga/bytes.hpp>`, which goes byte by byte: `load_le16`, `load_le32`, `load_be16`, `load_be32`, `store_le16`,
  `store_le32`, `store_be16`, `store_be32` in `enaga::bytes`, each taking any address (`le` little-endian, the
  processor's and WAV's order; `be` big-endian, as in standard MIDI files).
- **Null pointers:** reading address 0 does not fault; it returns garbage. Writes to wild pointers can corrupt
  Enaga: there is no memory protection (the C6748 has no MMU), only recovery by the watchdog and the exception
  handler.
