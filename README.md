# Enaga

Enaga is replacement firmware for the **Sonicware SmplTrek** that turns it into a platform for apps. The
**Lofi-12 XT** and **CyDrums** are the same board with differently printed panels, and the build makes an update
file for each, but Enaga has only run on a SmplTrek so far (below, "Installing"). An app
is a whole instrument written in C++: Enaga starts it from the SD card, hands it the pads, keys, encoders,
display and a real-time audio callback, and recovers cleanly if it crashes.

**Status: early development.** Enaga runs on a SmplTrek: the kernel, audio engine, display, keys, pads, SD
card, the synth example and the Enaga menu with app switching have all run on a unit (three sessions so far,
docs/hardware.md "Checks on a unit"); everything is also tested on an emulator. The app interface
(`include/enaga/abi.h`) is provisional until 1.0.
The design and plan are in [docs/design.md](docs/design.md); the hardware facts Enaga relies on are in
[docs/hardware.md](docs/hardware.md).

## Writing an app

```cpp
#include <enaga/app.hpp>

class Beep : public enaga::App {
public:
    void on_key(int key, bool down) override { if (key == ENAGA_KEY_F5) playing_ = down; }
    void audio(const float *, float *out, uint32_t frames) override {   // input unused
        for (uint32_t i = 0; i < frames; i++) {
            float s = 0.0f;
            if (playing_) s = (phase_++ & 32) ? 0.2f : -0.2f;   // a 750 Hz square wave
            out[2 * i] = out[2 * i + 1] = s;                    // interleaved stereo, full scale +-1.0
        }
    }
    void draw(enaga::Canvas &canvas) override { canvas.clear(); canvas.text(0, 0, "BEEP"); }

private:
    volatile bool playing_ = false;                      // written in the main context, read in audio
    uint32_t phase_ = 0;
};

ENAGA_APP(Beep)
```

- `init`, `update(ms)`, `draw(canvas)`, `stop`, `on_key`, `on_encoder`, `on_pad_press(pad, velocity)`,
  `on_pad_release`, `on_pad` (every raw pressure reading) run in Enaga's main loop, which sleeps between
  interrupts: a pass, so `update`, runs at least 750 times a second (after every audio block), not flat out.
  `audio` runs in the audio interrupt every 64 frames (1.33 ms at 48 kHz) and must not block, allocate,
  draw or touch files.
- The canvas is 128 x 128 pixels, one byte per pixel, grey 0 (black) to 15.
- Apps are C++20 with `-fno-exceptions -fno-rtti`. The freestanding standard library offers headers such as
  `<array>`, `<algorithm>`, `<span>`, `<optional>`, `<variant>`, `<type_traits>`, `<bit>`, `<atomic>` and
  `<numbers>`. `new`/`delete` work on the app's heap (everything above Enaga, about 124 MiB). There is no
  libm yet, and no allocating standard containers. Global destructors never run: Enaga discards an app's
  memory after calling `stop()`. A function's stack frame is limited to 16 KiB and VLAs and `alloca` are
  refused at build time (Enaga's interrupt entry protects the 16 KiB below the stack pointer; see
  `firmware/start.s`): put large arrays in statics or on the heap.
- Panel LEDs by name, levels 0-10, from the main loop: `enaga::pad_led(pad, r, g, b)`, `step_led(n, red, green)`,
  `button_led(key, red, green)`, `vol_led`, `mic_led`, `value_led`. Enaga switches them all off when an app stops.
- `<enaga/dsp.hpp>` is the DSP library: fast math without libm (`exp2`, `semitones`, `sin_turns`, `tan_pi`,
  `soft_clip`), band-limited oscillators, wavetable oscillators on the app's tables (with a morph between two),
  a state-variable filter (and a two-stage cascade), cookbook biquads (low- and high-pass, band-pass, notch,
  all-pass, peak, shelves), FIR filters on the app's taps, a one-pole, ADSR envelopes, LFOs, delay lines (with
  modulated, interpolated block reads), a chorus, a stereo delay and a reverb; hard sync between oscillators
  (`Oscillator::sync_add`, band-limited). Its classes wrap a C interface, `<enaga/dsp.h>`:
  plain state structs, per-sample steps, and block functions that process a buffer per call (`Svf::process`,
  `Envelope::render`, `Oscillator::render_add`, `StereoDelay::process`, `Reverb::process`, delay-line block reads
  and writes, buffer arithmetic such as `enaga_mix`, `enaga_multiply`, `enaga_ramp_stereo_add`, and `exp2`, `tan_pi` and
  `reciprocal` over a buffer, for control values gathered across voices). The block functions (`runtime/dsp`) are built with TI's C6000 compiler (cl6x) when it
  is installed, several times faster than GCC's code, and with GCC otherwise; both give the same samples. Process
  in blocks: per-sample `step()` calls are there for code that needs them (feedback loops), but compile with GCC.
- Inner loops of your own can go in `*.ti.c` files, built by cl6x the same way (see "Building"). cl6x pipelines
  only loops without branches inside: use `cond ? a : b` selects rather than `if`. The files must be C, compile
  with GCC too, and export plain C functions, since C++ objects don't mix between the two compilers; they can
  include `<enaga/dsp.h>` and work on the library's state. Everything else builds with GCC (`-O2
  -funroll-loops`). The synth's worst case takes 30% of the audio budget with cl6x and 65% with GCC alone
  (emulator figures).
- `<enaga/files.hpp>` reads and writes the SD card (FAT or exFAT, long names; paths like `/SAMPLES/Kick 01.wav`).
  Small files at once: `files::save(path, data, bytes)` (safe against a pulled card: the old file or the new one
  survives) and `files::read_all`. Large ones in the background, with a callback in the main context when done:
  `files::load`, `files::list`, and `files::File` reads and writes. An app reads anywhere and writes in its own
  folder (`files::data_folder()`, `/ENAGA/DATA/<package name>`), `/SAMPLES` and `/RECORD`. `on_card(bool)` says
  when the card is removed or inserted. Not from `audio()`, except streams: `files::Stream` plays a file into
  `audio()` or records one from it through a ring buffer Enaga keeps filled or drained in the main loop, and
  `<enaga/wav.hpp>` reads and writes WAV headers. `<enaga/settings.hpp>` keeps an app's own settings
  (`settings.get("TEMPO", 120)`, `settings.set("TEMPO", tempo)`, `save()`) as text in its data folder.
- `enaga::system::open_menu()` opens the Enaga menu as F1 + POWER does; `enaga::system::set_busy(true)` keeps the
  unit from switching itself off (the AUTO OFF setting) while the app works unattended, e.g. recording;
  `enaga::system::speaker(false)` mutes the built-in speaker (e.g. while monitoring the built-in microphone). `headphones()` says whether headphones are plugged in; `device()`
  says which of the SmplTrek, Lofi-12 XT and CyDrums Enaga runs on.
- `enaga::fatal(reason)` stops the app; Enaga shows the reason and keeps running. Enaga's watchdog stops an
  app the same way when a main-loop call (including `init` and `stop`) has not returned after 1 s, or `audio`
  after 100 ms; `stop` has 3 s in all, even while it is saving to the card. When an app is switched away from,
  its sound fades out before `stop` is called.

See `examples/tone` for a minimal app and `examples/synth` for a whole instrument: an eight-voice virtual
analogue synthesizer with hard sync and a ring modulator, a filter, three envelopes, two LFOs that can fade in,
a modulation matrix, unison, chorus, delay and reverb, played from the pads and STEP keys, edited page by
page, with sixteen user patch slots and the last sound kept on the card.

## Building

Requirements: Linux, Python 3.10 or newer, host `gcc`/`g++`, and the GCC cross-compiler for TI C6000
(`tic6x-elf`, with C++), built once from source by the scripts in `tools/`:

```sh
tools/build_tic6x_binutils.sh       # a few minutes; installs into tools/tic6x
tools/build_tic6x_gcc.sh            # 10-30 minutes
```

Set `ENAGA_TOOLCHAIN` to use an existing installation elsewhere. Optionally install TI's C6000 code generation
tools (free from TI; tested with 8.5.0.LTS) for the DSP library and `*.ti.c` files: `tools/paths.py` uses the newest
`~/ti/ti-cgt-c6000_*`, or `ENAGA_TI_CGT` (`ENAGA_TI_CGT=none` builds them with GCC). The library is built once
into `build/dsplib/` and again when it changes. Then:

```sh
python3 -B tools/build_app.py examples/tone     # -> build/apps/tone/APP.ENA
python3 -B firmware/build.py                    # -> build/firmware/ENAGA_0_1_0.bin
```

## Installing

For developers and testers: 0.1.0 runs on a SmplTrek, but it is early software (docs/design.md "Open items").
The Lofi-12 XT and CyDrums files are **untested**: their update format is checked against official releases,
but nobody has installed Enaga on one, and what their own bootloaders leave behind is unknown. Only try one if
you are prepared to restore it with its official update.

1. Copy the file for your device to the root of the SD card, keeping its name: `build/firmware/smpltrek/SmplTrek.bin`,
   `build/firmware/lofi12xt/Lofi-12 XT.bin` or `build/firmware/cydrums/CyDrums.bin` (`ENAGA_<version>.bin` is
   the SmplTrek's under the release name). Copy an app's package to `/ENAGA/APPS/<NAME>/APP.ENA` (8.3 names).
2. Hold the fourth key of the upper function row (F4: HOME on a SmplTrek, PAD on a Lofi-12 XT or CyDrums) while
   switching on to reach BOOT MODE, and choose SYSTEM UPDATE.

Enaga starts the last app that ran (or the only one installed); **F1 + POWER** opens the Enaga menu, which lists
the apps and switches between them and holds the settings (volume, brightness, pad sensitivity, auto-off, speaker, kept in
`/ENAGA/SETTINGS.TXT`), and holding F1 while switching on opens it instead of an app.

To go back, install an official SmplTrek system update the same way. Enaga never modifies the bootloader or
the original sounds in flash, so that restores the unit completely. See docs/hardware.md, "Installing and
recovery".

## Testing without a unit

Enaga's tests and `tools/emulate.py` run the real firmware and apps on the **SmplTrek emulator**, a
cycle-level C674x and board emulator published as part of the separate SmplTrek reverse-engineering
project. Point `ENAGA_EMULATOR` at its emulator directory (the one containing `cpu.py`):

```sh
export ENAGA_EMULATOR=/path/to/emulator
python3 -B -m unittest discover -s tests -v                           # 52 tests, about 5 minutes
python3 -B tools/emulate.py examples/tone --key 32@300000000           # press STEP 1; writes build/emulate/
```

To play an app live, with the display, every control and the sound in a browser, run it in the emulator's web
panel:

```sh
python3 -B tools/panel.py examples/synth              # then open http://127.0.0.1:8766, press Enable audio
python3 -B tools/panel.py examples/tone -- --port 8770 # options after -- go to the panel (stock_ui.py --help)
```

It builds the firmware and the apps, puts them on a fresh card image (`build/panel/card.img`) and starts the panel
on Enaga's update file; Ctrl-C stops it. The emulator runs the synth at about 0.8x real time on a current PC, so
heavy patches break up.

Without `ENAGA_EMULATOR` the emulator tests are skipped; `tests/test_dsp.py` (the DSP library on the host) needs
only `g++`. `tools/emulate.py` writes the last screen to
`build/emulate/frame.png`, the captured output to `build/emulate/audio.wav`, and a JSON report.

## Layout

| Path | What it is |
|---|---|
| `include/enaga/abi.h` | The C boundary between Enaga and an app: package header, host table, app table, events, key names |
| `include/enaga/app.hpp` | The C++ layer: `enaga::App`, `enaga::Canvas`, `ENAGA_APP(Type)`, `enaga::log`, `enaga::fatal` |
| `include/enaga/dsp.hpp`, `dsp.h`, `dsp/` | The DSP library: C++ classes over a C interface |
| `runtime/dsp/` | The DSP library's block functions, built by cl6x or GCC into every app |
| `runtime/` | Linked into every app: entry point, static constructors, heap, `new`/`delete`, C++ ABI hooks, `app.ld` |
| `loader/` | The package loader (freestanding C, used by the firmware and the tests) |
| `firmware/` | Enaga itself: `hal/` drivers, `kernel/` (interrupts, audio engine, app host, menu, settings, exceptions, watchdog, main loop), `start.s`, `link.ld`, `build.py` |
| `examples/` | `synth` (the virtual analogue synthesizer); `tone` (STEP 1-8 play 750 Hz x step); `cxxcheck` (exercises the C++ runtime) |
| `tests/` | `test_e0.py`: packages through the loader and runtime on the emulator's CPU; `test_e1.py`: the whole firmware on the emulated board; `test_check.py` and `unit/check`: the CHECK app for sessions on a unit; `test_synth.py`: the synth example; `test_files.py` and `apps/files`: the file service; `test_streams.py` and `apps/streams`: streams and WAV; `test_menu.py` and `apps/menu`: the Enaga menu, boot policy, app switching and settings; `test_dsp.py` and `dsp/`: the DSP library on the host; `test_sysimage.py`: the update files for each device |
| `tools/` | `build_app.py`, `emulate.py`, `panel.py` (apps live in the emulator's web panel), `card.py` (FAT and exFAT card images through Enaga's own FatFs), `unit_card.py` and `unit_dry_run.py` (files for a session on a unit, and the session walked on the emulator), `sysimage.py` (the update-file format), `paths.py`, toolchain build scripts |
| `docs/` | `design.md` (decisions, architecture, plan, log), `hardware.md` (the hardware notes) |

## Licence

Not yet chosen (see docs/design.md). FatFs (`firmware/third_party/fatfs`) is © ChaN under its own BSD-style
licence. Enaga contains no Sonicware code.
