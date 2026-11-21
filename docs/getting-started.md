# Getting started

From an empty folder to your own app running on a SmplTrek. You need:
- a Linux PC (x86-64) with Python 3.10 or newer. Windows works through WSL.
- a SmplTrek with Enaga installed, its SD card and a card reader. Installing Enaga is in the project's README,
  ["Installing"](https://github.com/seawaffle/enaga#installing): the firmware file from the same release goes on
  the card, and the unit's own SYSTEM UPDATE installs it (an official update puts the original firmware back).

The prebuilt compiler needs glibc 2.38 or newer and libzstd. Ubuntu 24.04, Debian 13 and Fedora 39 or newer
have both. On anything older, build the compiler from source instead (step 1, last paragraph).

## 1. Get the SDK and the compiler

From the [Releases](https://github.com/seawaffle/enaga/releases) page of the version you run, download:
- `enaga-sdk-<version>.zip`: the headers, the app runtime, the build tool, a template and the examples.
- `enaga-toolchain-<version>-linux-x86_64.tar.xz`: GCC 13.3 for the C6748.

In the folder you downloaded them to, unpack the SDK, then the compiler into the SDK's `tools/` folder, where the
build tool looks for it. The commands from here on run in that folder:

```sh
unzip enaga-sdk-v0.2.0.zip
tar -xf enaga-toolchain-v0.2.0-linux-x86_64.tar.xz -C enaga-sdk-v0.2.0/tools
```

You should now have `enaga-sdk-v0.2.0/tools/tic6x/bin/tic6x-elf-g++`. The SDK and the toolchain are the same
version as the firmware, and an app needs firmware of the same version: the interface is provisional until 1.0.

The compiler's source is published next to it (`enaga-toolchain-<version>-source.tar`, with how it was built).
To build it yourself instead, use `build_tic6x_binutils.sh`, then `build_tic6x_gcc.sh`, from that archive or
the repository's `tools/`. That needs flex, bison and a C++ compiler, and takes 20-30 minutes. Point
`ENAGA_TOOLCHAIN` at the result if it is not in the SDK's `tools/tic6x`.

## 2. Make your app

Copy the template to a folder of your own, anywhere. Its name becomes the app's name in the Enaga menu, in
upper case (`my-synth` shows as MY-SYNTH), unless you give the build another (step 3):

```sh
cp -r enaga-sdk-v0.2.0/template ~/my-synth
```

`app.cpp` is a small synth with one voice: the pads and MIDI notes play it, and knobs A-D set its wave, attack,
release and level. Every `.cpp` and `.c` file in the folder is part of the app. `icon.txt` is its icon in the menu,
16 lines of 16 greys from `0` (black) to `F` (white).

## 3. Build it

```sh
python3 enaga-sdk-v0.2.0/tools/build_app.py ~/my-synth
```

This writes `~/my-synth/build/APP.ENA`, the package you put on the card. Warnings stop the build
(`-Werror`). `--name "MY SYNTH"` sets the name shown in the menu (up to 23 characters).

The DSP library is compiled into every app. If TI's free C6000 compiler (cl6x) is installed in
`~/ti/ti-cgt-c6000_*`, the build tool uses it for the library, and its inner loops run several times faster. The
tool's last line says which compiler built it. Without cl6x, GCC builds it and everything still works, just with
less audio headroom. App source files named `*.ti.c` are built with cl6x too, when it is there (docs/api.md,
"Writing inner loops for cl6x"). Set `ENAGA_TI_CGT` to use a cl6x installed elsewhere, or to `none` for GCC
only.

The DSP library is built once, into the SDK's own `build/` folder, and again only when the SDK changes, so the
SDK folder must be writable.

## 4. Put it on the card

Each app has a folder under `/ENAGA/APPS` on the card, with a short name: up to 8 letters, digits, `_` or `-` (a
FAT "8.3" name). That folder name is the app's *package name*: it also names the app's data folder
(`/ENAGA/DATA/<name>`), so keep it when you update the app. It is separate from the name the menu shows, which
comes from the build (step 2), so here the folder is MYSYNTH and the menu says MY-SYNTH.

```sh
mkdir -p /media/$USER/SDCARD/ENAGA/APPS/MYSYNTH
cp ~/my-synth/build/APP.ENA /media/$USER/SDCARD/ENAGA/APPS/MYSYNTH/
```

(Your card's mount point may differ.) Eject the card, put it back in the SmplTrek and switch on. Enaga starts the
last app that ran (or the only one installed). **F1 + POWER** opens the menu: choose MY-SYNTH and press OK. Play
a pad.

If the app does not start, the menu says why (a build for another version, say). `enaga::log` lines and crash
reasons are on the menu's STATUS page.

## 5. Change it

Things to try in `app.cpp`, building and copying again after each:
- Change `kFirstNote` so the pads start an octave lower.
- Add a fifth parameter (a filter cutoff, with `Svf` from `<enaga/dsp.hpp>`). The page puts parameters in groups
  of four, and the selection buttons switch between groups.
- Draw your own icon in `icon.txt`.

The rules that matter most:
- **Two contexts.** The main context gets events and draws; `audio()` runs every 64 frames (1.33 ms) and must
  never wait. Share state between them in single words, each written by one side only, as the template does.
- **No libc or libm.** Use the DSP library's math (`enaga::dsp::exp2`, `semitones`, `sin_turns` and the rest).
- **Allocate in `init()`.** `new` works, but never in `audio()`.
- **Packed data** (file formats, SysEx) at odd addresses: read and write it with `<enaga/bytes.hpp>`. A misaligned
  32-bit access on this processor does not fault; it silently uses the wrong address.
- **A null pointer** read returns garbage, not a crash.

## Where next

- `docs/api.md`: the reference, header by header.
- `examples/tone`: the smallest app. `examples/widgets`: the UI toolkit's widgets. `examples/synth`: a full
  instrument (eight voices, filters, effects, patches on the card).
- To run apps without a unit: the SDK has no emulator. The project's repository has one (its README, ["Testing
  without a unit"](https://github.com/seawaffle/enaga#testing-without-a-unit)): with a clone of it and of the
  emulator, `tools/panel.py` plays any app folder in a browser, with the screen, the controls and the sound.
