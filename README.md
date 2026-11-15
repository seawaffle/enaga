# Enaga

Enaga is replacement firmware for the **Sonicware SmplTrek** that turns it into a platform for apps. An app
is a whole instrument written in C++: Enaga starts it from the SD card, hands it the pads, keys, encoders,
display and a real-time audio callback, and recovers cleanly if it crashes.

**Status: early development.** The kernel, audio engine, display and crash handling work on an emulator;
nothing has run on a unit yet, and the app interface (`include/enaga/abi.h`) is provisional until 1.0.
The design and plan are in [docs/design.md](docs/design.md); the hardware facts Enaga relies on are in
[docs/hardware.md](docs/hardware.md).

## Writing an app

```cpp
#include <enaga/app.hpp>

class Beep : public enaga::App {
public:
    void on_key(int key, bool down) override { if (key == ENAGA_KEY_PLAY) playing_ = down; }
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

- `init`, `update(ms)`, `draw(canvas)`, `stop`, `on_key`, `on_encoder`, `on_pad` run in Enaga's main loop.
  `audio` runs in the audio interrupt every 64 frames (1.33 ms at 48 kHz) and must not block, allocate,
  draw or touch files.
- The canvas is 128 x 128 pixels, one byte per pixel, grey 0 (black) to 15.
- Apps are C++20 with `-fno-exceptions -fno-rtti`. The freestanding standard library offers headers such as
  `<array>`, `<algorithm>`, `<span>`, `<optional>`, `<variant>`, `<type_traits>`, `<bit>`, `<atomic>` and
  `<numbers>`. `new`/`delete` work on the app's heap (everything above Enaga, about 124 MiB). There is no
  libm yet, and no allocating standard containers. Global destructors never run: Enaga discards an app's
  memory after calling `stop()`.
- Panel LEDs by name, levels 0-10, from the main loop: `enaga::pad_led(pad, r, g, b)`, `step_led(n, red, green)`,
  `button_led(key, red, green)`, `vol_led`, `mic_led`, `value_led`. Enaga switches them all off when an app stops.
- `enaga::fatal(reason)` stops the app; Enaga shows the reason and keeps running. Enaga's watchdog stops an
  app the same way when a main-loop call (including `init` and `stop`) has not returned after 1 s, or `audio`
  after 100 ms.

See `examples/tone` for a complete app.

## Building

Requirements: Linux, Python 3.10 or newer, host `gcc`/`g++`, and the GCC cross-compiler for TI C6000
(`tic6x-elf`, with C++), built once from source by the scripts in `tools/`:

```sh
tools/build_tic6x_binutils.sh       # a few minutes; installs into tools/tic6x
tools/build_tic6x_gcc.sh            # 10-30 minutes
```

Set `ENAGA_TOOLCHAIN` to use an existing installation elsewhere. Then:

```sh
python3 -B tools/build_app.py examples/tone     # -> build/apps/tone/APP.ENA
python3 -B firmware/build.py                    # -> build/firmware/ENAGA_0_1_0.bin
```

## Installing

Not yet recommended: 0.1.0 has not been run on a unit. When it is:

1. Copy `build/firmware/ENAGA_<version>.bin` to the root of the SD card as `SmplTrek.bin`, and an app's
   package to `/ENAGA/APPS/<NAME>/APP.ENA` (8.3 names).
2. Hold **HOME** while switching on to reach BOOT MODE, and choose SYSTEM UPDATE.

To go back, install an official SmplTrek system update the same way. Enaga never modifies the bootloader or
the original sounds in flash, so that restores the unit completely. See docs/hardware.md, "Installing and
recovery".

## Testing without a unit

Enaga's tests and `tools/emulate.py` run the real firmware and apps on the **SmplTrek emulator**, a
cycle-level C674x and board emulator published as part of the separate SmplTrek reverse-engineering
project. Point `ENAGA_EMULATOR` at its emulator directory (the one containing `cpu.py`):

```sh
export ENAGA_EMULATOR=/path/to/emulator
python3 -B -m unittest discover -s tests -v                           # 22 tests, about 2 minutes
python3 -B tools/emulate.py examples/tone --key 32@300000000           # press STEP 1; writes build/emulate/
```

Without `ENAGA_EMULATOR` the tests are skipped. `tools/emulate.py` writes the last screen to
`build/emulate/frame.png`, the captured output to `build/emulate/audio.wav`, and a JSON report.

## Layout

| Path | What it is |
|---|---|
| `include/enaga/abi.h` | The C boundary between Enaga and an app: package header, host table, app table, events, key names |
| `include/enaga/app.hpp` | The C++ layer: `enaga::App`, `enaga::Canvas`, `ENAGA_APP(Type)`, `enaga::log`, `enaga::fatal` |
| `runtime/` | Linked into every app: entry point, static constructors, heap, `new`/`delete`, C++ ABI hooks, `app.ld` |
| `loader/` | The package loader (freestanding C, used by the firmware and the tests) |
| `firmware/` | Enaga itself: `hal/` drivers, `kernel/` (interrupts, audio engine, app host, exceptions, watchdog, main loop), `start.s`, `link.ld`, `build.py` |
| `examples/` | `tone` (STEP 1-8 play 750 Hz x step); `cxxcheck` (exercises the C++ runtime) |
| `tests/` | `test_e0.py`: packages through the loader and runtime on the emulator's CPU; `test_e1.py`: the whole firmware on the emulated board |
| `tools/` | `build_app.py`, `emulate.py`, `sysimage.py` (the update-file format), `paths.py`, toolchain build scripts |
| `docs/` | `design.md` (decisions, architecture, plan, log), `hardware.md` (the hardware notes) |

## Licence

Not yet chosen (see docs/design.md). FatFs (`firmware/third_party/fatfs`) is © ChaN under its own BSD-style
licence. Enaga contains no Sonicware code.
