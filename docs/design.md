# Enaga design

Enaga is replacement firmware for the Sonicware SmplTrek. Its job is to run **apps**: whole instruments
written by outside developers in C++, on top of a library that hides the hardware. This document records
what has been decided, how the system is meant to work, and the plan to 1.0. Items marked **proposal** are
starting points open to change; **open** items need evidence or a decision.

The hardware facts Enaga relies on (memory map, peripherals, pins, the update format) are in
[hardware.md](hardware.md).

## Decisions

Made by the project owner on 2026-11-15.

1. **An app is a whole instrument.** One app runs at a time and owns the musical experience. It must be
   possible, in principle, to write something of the scale of the SmplTrek's own firmware or a
   Lofi-12-class sampler as an Enaga app. That requirement drives the memory, I/O and CPU budgets below.
2. **No app picker at power-on.** Enaga loads the last app that ran; if none is recorded and exactly one app
   is installed, that one; otherwise it opens the Enaga menu. Apps are switched from the Enaga menu while
   the unit is on, without a reboot.
3. **The Enaga menu is F1 + POWER.** While running, pressing POWER while F1 is held opens it. Holding
   F1 while powering on boots straight into it without loading an app (the escape from an app that
   crashes at start-up). The power key is never delivered to apps.
4. **Apps are written in C++** (C also works). The boundary between Enaga and an app is a C ABI; the SDK
   provides a header-only C++ layer over it.
5. **Outside developers are the audience.** A feature-complete 1.0 is to be released publicly, with a
   stable ABI, documentation, examples and tools.

## Execution model

Enaga owns time and hardware; an app owns musical logic. There is no RTOS and an app has no threads.
Two contexts run app code:

| Context | Driven by | App code | Rules |
|---|---|---|---|
| Audio | the EDMA block-complete interrupt of the McASP ring | `audio(in, out, frames)` | Hard deadline. No file I/O, drawing, allocation or blocking. Talks to the main context only through lock-free queues or atomics. |
| Main | Enaga's event loop | event callbacks, `update`, `draw`, completion callbacks | May be slow. Never blocks on I/O: long operations are asynchronous. |

Work an app might otherwise put in a thread is an Enaga service instead: SD streaming, asynchronous file
reads and writes, display DMA, MIDI transport. Blocks are 64 frames at 48 kHz (1.33 ms); **proposal:** 32
or 128 selectable at app start. Samples are float32 (the C674x has hardware floating point; the codec's
fixed-point slots are hidden).

## Boot and app switching

Power-on sequence (proposal):

1. The bootloader hands over (it is never modified; F4 + POWER still reaches its BOOT MODE, the recovery
   path, see hardware.md "Installing and recovery").
2. Enaga takes the power hold, starts the display and the key matrix, and samples F1 while the power key
   is still held (the original firmware checks F1 at about 0.58 s for its factory test mode). The
   bootloader only tests F4, so F1 at power-on is Enaga's to use.
3. F1 held: Enaga menu. Otherwise the boot policy in decision 2.
4. A last app that faults or trips the watchdog during start-up is not auto-loaded on the next boot.

Switching apps while on:

1. Fade audio to silence, stop the audio callback.
2. Call the app's `stop()` so it can save state (with a bounded time budget).
3. Cancel everything the app holds: pending file operations, timers, MIDI routes, streaming voices.
4. Discard the app's memory region and heap in one step.
5. Load the new package, run its static constructors, call `init()`.

F1 + POWER rules: the menu opens on the POWER press edge while F1 is down (no wait for release);
POWER first and then F1 does nothing; while the menu was opened by the chord, keeping POWER held does
not start the one-second power-off. When the menu opens the app receives an F1 release (or an
input-cancel event) and is paused. A plain short POWER press is reserved for Enaga and unused for now.
POWER held one second with F1 up switches off.

## Memory

DDR is 128 MiB at `0xC0000000`-`0xC7FFFFFF` (hardware.md "Memory").

| Region | Size | Owner |
|---|---|---|
| Enaga code, data, stacks, DMA rings | 4 MiB at `0xC0000000`; 0.1.0 uses about 80 KB plus a 256 KB stack | Enaga |
| App image (code, data, bss) | from `ENAGA_APP_BASE` = `0xC0400000`, as large as the app | app |
| App heap | from the end of the image, rounded up to 4 KiB, to the end of DDR (`ENAGA_APP_LIMIT` = `0xC8000000`) | app (`new`, `malloc`, samples) |
| L1P, L1D, L2 | all cache since E1 (Enaga sets it at start-up); part of L2 as RAM for audio-critical code and buffers stays **open** | to be measured on the unit |

So an app has the whole 124 MiB above Enaga for its image plus heap. For comparison, the original firmware's
image and static buffers take about 21 MiB and its default heap is 64 MiB. App assets live on the card in the
app's directory. Enaga never writes the original sound content in the unit's SPI flash, so restoring the
original firmware stays a single system update.

The bootloader runs from `0xC7020000`-`0xC70B0000` only while it loads Enaga; apps are loaded by Enaga after
the hand-over, so that range is ordinary app heap afterwards.

## What a developer gets (the 1.0 API surface)

Proposal, grouped by service. Names are illustrative until each service exists.

- **App lifecycle:** `init`, `stop`, `update` (main-loop tick), `draw`, `audio`. A manifest with name,
  icon, version, minimum API level, memory and block-size needs.
- **Audio:** block callback (float32, stereo in/out); sample-accurate event scheduling into the callback;
  a CPU-load meter (cycles per block against the budget, shown in the Enaga menu and the log).
- **DSP library:** oscillators, biquad/SVF filters, envelopes, delay lines, interpolating sample players,
  mixers, saturation, resamplers; tuned for the C674x so app authors need not know about SPLOOP or cache
  placement. Placement attributes for hot code and data when L2 RAM is available.
- **Input as events:** pads (press/release with velocity, pressure/aftertouch from the 14-bit panel
  readings, system-owned thresholds and calibration); keys (down, up, long press, F1-shift combinations);
  encoders (delta with acceleration). Keys and LEDs by logical name (`F5`, `pad_led(3)`), never by
  switch-word bit or panel MCU address.
- **Display:** 128 x 128, 4-bit grey, a row-major canvas; Enaga converts to the panel order and sends it by
  DMA. Primitives (lines, rectangles, bitmaps, two or three fonts) and a UI toolkit (menus, parameter
  pages, file browser, dialogs, waveform view) so apps share a look and feel.
- **Storage:** an asynchronous file API with completion callbacks; long file names; directory listing; a
  per-app data directory; WAV load/save; streaming sample playback and streaming recording to WAV; a
  per-app key/value settings store. System settings are Enaga's; apps never see raw flash.
- **Time and music:** tempo clock and transport, MIDI in/out with clock sync (USB first; hardware MIDI if
  the unit has a port, **open**).
- **Runtime:** `new`/`delete`/`malloc` on the app heap, `printf` to the log, fast float math, a freestanding
  C++ standard library subset, `enaga::system::open_menu()`.
- **System:** battery and power source, auto-off control (apps report "busy"), the crash screen.

## Enaga itself

- **Drivers:** McASP/EDMA, the codec over I2C, the OLED over SPI with frame DMA, key matrix, encoders, the
  panel MCU link (UART), MMCSD, timers, power; SPI flash for settings and USB0 later.
- **Services:** audio engine, event queue, asynchronous I/O, streaming, settings, launcher/menu, watchdog,
  exception handling (crash screen, return to menu).
- **Enaga menu:** app list (name, icon), switch app, system settings (volume, pad sensitivity, MIDI channel,
  brightness, auto-off), Diagnostics (keys, encoders, pads, LEDs, audio routing, card), CPU load, versions.
- **No isolation guarantee:** the C6748 has no MMU. Apps are native code with full machine access; the goal
  is clean recovery (watchdog, exception handler, crash-loop protection), not sandboxing. Users are told so.

## ABI and packaging

- The boundary is a versioned C function table passed to the app entry, append-only within 1.x; the app
  returns a C callback table. C++ ABIs are not stable across compiler versions; a C boundary is. The
  current table (`include/enaga/abi.h`, version 0) is provisional until 1.0.
- Apps link at a fixed base (no relocation) and are flat images inside a package:
  `/ENAGA/APPS/<NAME>/APP.ENA`, plus (proposal) an optional `ICON` and data. The package header carries
  magic, header size, ABI version, load address and sizes, the entry offset, CRC-32, app version and name.
- C++ is compiled `-fno-exceptions -fno-rtti`. The app runtime (`runtime/runtime.cpp`) provides
  `operator new/delete` over the app heap, `__cxa_pure_virtual`, `__cxa_guard_*` (one context),
  `__cxa_atexit` (a no-op: apps are torn down by discarding memory), and runs the static constructors. This
  GCC emits them in `.ctors`; `.init_array` is handled too.

## Tooling and release

- Toolchain: GCC 13.3 `tic6x-elf` with C and C++ and a freestanding libstdc++ (`tools/build_tic6x_*.sh`).
  Prebuilt for Linux, macOS and Windows (Docker first) by 1.0.
- A project template and build tool, example apps (synth, sampler, step sequencer), an API reference.
- Testing at three levels: a host-native app lab for fast UI work (planned), the SmplTrek emulator running
  Enaga and the app with exact audio, frame and card output, and hardware. Every Enaga service gets
  emulator coverage when it is added; scripted scenarios (press keys, assert samples and frames) gate
  changes. The emulator is a separate open-source project (README.md, "Testing without a unit").
- Release hygiene: Enaga installs as a normal system update and never writes the bootloader; the original
  firmware can always be restored through BOOT MODE, documented prominently. No Sonicware code or branding.
  Licence: **open** (Apache-2.0 or MIT; FatFs's BSD-style licence fits either).

## Phases

| Phase | Content | Exit |
|---|---|---|
| **E0** | C++ toolchain; app-side C++ runtime; package format; loader that runs constructors | a C++ app with globals, virtuals and `new` runs from a package (done) |
| **E1** | Kernel: audio callback, display DMA, event loop, exception handling and watchdog, cache/L2 set-up | device load test: measured audio headroom with caches on |
| **E2** | Audio API and DSP library, asynchronous file I/O, sample loading and streaming | one real instrument app |
| **E3** | Enaga menu, boot policy, F1 escape, app switching, settings | switch between two apps repeatedly on the unit |
| **E4** | UI toolkit, MIDI, SDK packaging and documentation | an outside developer builds the template app from the docs |
| **E5 / 1.0** | USB (mass storage, MIDI), API freeze, release | public release |

## Status

E0 is complete. E1 has its kernel, audio engine, display DMA and exception handling, all tested on the
emulator; nothing has run on a unit yet. The watchdog, key debouncing and the start-up cache configuration are in
place (emulator-tested). Remaining for E1: the first session on the unit, prepared (docs/hardware.md "Checks on
a unit", the CHECK app in `tests/unit/check`, `tools/unit_card.py`) and not yet run.

## Open items

- On the unit: F1 sampled during the power-key hold; the bootloader passes control normally with F1
  held; F1 + POWER mid-run; power-key bounce; how long the matrix keys bounce (the 6 ms sampling and
  two-sample release follow the original firmware, not a measurement).
- LED map: 53 addresses known only from the code, 24 seen on another channel of their LED (docs/hardware.md
  "LED map"; smpltrek session L1); the level-to-brightness steps. Whether the unit has a hardware MIDI port;
  physical left/right channel routing; real audio CPU headroom with caches; the key names, confirmed key by key
  on a unit.
- The L2 split between cache and RAM. The bootloader's cache state at entry is simulated only (docs/hardware.md
  "Checks on a unit").
- USB stack: the C6748's USB0 is a Mentor MUSB controller; a TinyUSB port (TinyUSB has a MUSB driver for
  other TI parts) is plausible and unverified.
- A fault in Enaga code called by an app (a host function given a bad pointer) currently counts as a kernel
  crash rather than the app's fault.
- Hangs with interrupts disabled (in a kernel handler, or an app that clears GIE) escape the software
  watchdog; the C6748's hardware watchdog (TIMER64P1) would catch them by resetting the chip, which probably
  also drops the power hold. Whether a reset on the unit switches it off or restarts Enaga is to be checked.
- Loading a very large app: the loader's CRC and bss clearing run without feeding the watchdog, so an image
  that takes over a second to check would trip it. To be measured on the unit with caches on.
- Licence.

## Development log

### E0 (2026-11-15)

- `tools/build_tic6x_gcc.sh` builds C and C++ with `--disable-hosted-libstdcxx`; the C compiler's output
  was checked to be unchanged by adding C++.
- The provisional ABI (`include/enaga/abi.h`, `app.hpp`), the app runtime, the package loader,
  `tools/build_app.py` and `examples/cxxcheck`. `tests/test_e0.py` runs packages through the real loader and
  runtime on the emulator's CPU core: the check app's full log matches, ten malformed packages are rejected
  before any app code runs, and heap exhaustion reaches `fatal()`.
- Findings: this GCC emits `.ctors`, not `.init_array`, and `--gc-sections` silently dropped them until
  `app.ld` kept them; the build now fails if a constructor table is discarded. libgcc's 64-bit helpers use
  the C674x `DMV` instruction, which the emulator then lacked (since added there, with a test).

### E1 (2026-11-15)

- The kernel: `start.s` (vector table, a context-saving interrupt entry, IRP return, a resume path for app
  failures), `kernel/irq.c` (CPU interrupt 8 from event 8, EDMA shadow region 1), `kernel/audio.c` (64-frame
  ping-pong rings, one transmit-completion interrupt per block, float conversion, the app's callback in
  interrupt context, cycle accounting against the 496,000-cycle budget), `kernel/apps.c` (mount, load the
  first `/ENAGA/APPS/<dir>/APP.ENA` straight into the app window, host table, events, `fatal()` recovery from
  either context), `kernel/main.c` (start-up, event loop, 50 ms draw, power hold-off) and
  `firmware/build.py` (the ELF and an installable `ENAGA_0_1_0.bin`). The drivers in `hal/` are unchanged
  from the bring-up firmware that first ran on the unit, apart from `gfx.c`, which now draws on a row-major
  canvas and converts to the panel order when presenting.
- Display frames by EDMA: PaRAM 19 sends the 8192 bytes to SPIDAT1, PaRAM 18 drains SPIBUF and its
  completion marks the end; the main loop keeps running while a frame goes out, and a frame still in flight
  delays the next draw. A frame in flight is always allowed to finish, never cut short: the panel's write
  position only wraps after a whole frame, so a partial frame shifts every later one (the emulator showed
  exactly that before the fix).
- CPU exceptions: TSR.GEE enables internal and software exceptions; `nmi_entry` records NRP, EFR, IERR,
  NTSR, SP and B3 and calls `enaga_exception` on its own stack. A fault at a PC inside the app window drops
  the app as `fatal()` does (from either context) and keeps the PC, EFR and IERR for the status page;
  anywhere else Enaga shows the kernel crash screen (silenced, muted, register dump) and waits for the power
  key.
- Emulator results (`tests/test_e1.py`, `tools/emulate.py`): boots with no card and services one interrupt
  per block with no overruns; loads `examples/tone` from a FAT card; STEP 1 gives a 750 Hz sine at exactly
  0.25 on both channels; apps failing through `fatal()` or a CPU exception, from `update` or the audio
  callback, are dropped while audio continues as silence; an exception injected into Enaga's own code shows
  the crash screen. The tone app's audio interrupt peaks at about 1.6 % of the block budget on the emulator,
  which models no caches or bus latency, so this is not a hardware figure.
- Two bugs of one kind, found by the crash tests: the C6000 ABI lets a called function store at `*SP`.
  (1) The interrupt entry saved ILC/RILC at `*SP`, so an interrupt between an inner loop's `MVC` to ILC and
  its `SPLOOP` restored a return address into ILC and the loop ran for billions of iterations; the frame now
  leaves words 0-1 unused (`test_interrupts_preserve_loop_state` fails on the old layout). (2) Stacks started
  at their very end, so the kernel's first call wrote the first word of the app window and the exception
  handler overwrote its own saved NRP; every stack pointer now starts 8 bytes inside its stack.

### E1 watchdog (2026-11-15)

- `kernel/watchdog.c`: TIMER64P0 now has a 10 ms period, and its TINT12 (event 4, the original firmware's tick
  event) reaches CPU interrupt 4, the highest maskable priority. Each tick checks that the main loop has fed
  the watchdog within 1 s and that the app's audio callback has not been running for 100 ms. A hang while an
  app is loaded, starting (constructors, `init`) or stopping is the app's: it is dropped as with `fatal()`
  ("APP NOT RESPONDING", "APP HUNG IN AUDIO") and the kernel loop continues. With no app it is Enaga's: the
  crash screen ("WATCHDOG"). Times come from the time-stamp counter, so the timer's unconfirmed input clock
  only changes how often the watchdog looks.
- A callback that never returns holds the audio interrupt, and handlers run with interrupts off, so the
  watchdog is the one nested interrupt: `audio.c` wraps the app's callback in `irq_nest_begin`/`irq_nest_end`
  (start.s), which save IRP, ITSR and IER, unmask only the watchdog and set GIE. `irq_active` now counts
  handlers, and every abnormal exit from interrupt context (`fatal()`, a CPU exception, the watchdog) goes
  through `irq_unwind`, which unmasks every enabled interrupt again; without that the audio interrupt stays
  masked after a failure inside the callback (checked by breaking it: the audio tests then fail).
- Emulator tests: an app hung in `update`, in the audio callback and in `init` is dropped while audio and
  drawing continue; an injected hang in Enaga's own code shows the crash screen after one second and not
  before; the tick runs at one interrupt per 10 ms. Removing the nesting makes the audio-callback test fail.

### E1 key debouncing (2026-11-15)

- `hal/keys.c` `keys_poll` follows the original firmware's timing (smpltrek claim HW-51, "Key timing and
  debounce"): the matrix is sampled every 6 ms, the spacing at which the original samples each key; a press is
  taken on the first closed sample (0-6 ms), a release on the second consecutive open sample (6-12 ms), so a
  one-sample opening while a key is held is ignored. The original applies that release filter only to bits 0-31,
  so its STEP keys turn a one-sample opening into a release and a new press; Enaga filters every key. A press
  shorter than the sample spacing can be missed, as in the original. A first version (take a change at once,
  then hold the key for a guessed 10 ms) was replaced by this before merging. The power key keeps its own
  handling (a one-second hold, which bounce only restarts).
- Emulator tests (`tests/apps/keys` logs every event): a press and a release that each bounce four times at
  0.5 ms give one press and one release; a press is reported within one sample interval; four 3 ms openings
  while STEP 1 is held (9 ms apart, so at least one is sampled) give no release, where releasing on the first
  open sample, as the original's STEP keys do, gives two spurious release/press pairs; a 13 ms press and a
  25 Hz roll give every event.
- The layout change exposed a fragile check in the kernel crash test, which mapped each sampled PC to the
  function at PC - 4; a sample at a function's first packet named the function before it. It now accepts the
  function at either address.

### Panel LED API (2026-11-15)

- The LED map from the smpltrek notes (`sdk/docs/hardware-access.md` "Panel LEDs", `led_table.py`), restated in
  docs/hardware.md "LED map" with each LED's confirmation on a unit. ABI: `enaga_led` numbers (pads by MCU
  channel; VOL L, VOL R, microphone, VALUE; every key with an LED as `ENAGA_LED_KEY` + its `enaga_key`) and one
  host function, `led(led, red, green, blue)`; a colour an LED lacks is ignored and levels are clamped to 10.
  The C++ layer has `pad_led`, `step_led`, `button_led`, `vol_led`, `mic_led` and `value_led`.
- `kernel/leds.c` keeps the level last sent to each address and sends only changes (5 bytes, about 108 us at
  465,000 baud), writes the row-0 LEDs on all three a-planes as the original firmware does, and never writes the
  29 unused addresses. Calls from interrupt context are ignored, since their bytes could split a message the
  main context is sending. Every LED is switched off at start-up, when an app is dropped (the kernel loop's entry
  with no app) and when one is unloaded.
- The emulator drops what UART1 sends, so the test checks Enaga's per-address cache: `tests/apps/leds` walks every
  LED and the test, whose table was transcribed separately from the notes' (a, b, c) form and checked against
  `led_table.py`, compares all 144 addresses after each step; clamping, the ignored audio-context call and the
  switch-off on a drop are checked too.
- GCC compiled the brightest-of-three into `MAXU4`, which the emulator lacked; MAXU4 and MINU4 were added there
  with a test (smpltrek working tree, not yet committed). The C++20 header needed `int(ENAGA_LED_KEY)`, since
  arithmetic between two enum types is deprecated.

### E1 cache configuration (2026-11-15)

- Enaga had relied on the bootloader having configured the caches. smpltrek's trace and simulation of BOOT 1.0.17
  (docs/hardware.md "Boot") show otherwise: it leaves L2 all RAM, L1D half cache, MAR 196-198 (48 MiB of the app
  heap) uncached, and does no cache maintenance before branching. The original firmware's configuration comes
  from its own SYS/BIOS start-up.
- The first instructions of `start.s` record the cache registers (into `.data`, since bss is cleared later), write
  back and invalidate L1D and invalidate L1P, so nothing cached from before the jump is relied on. The image
  itself is in DDR at entry: smpltrek's code reading shows the bootloader reads flash by EDMA, behind the caches,
  with no L2 cache (docs/hardware.md "Boot").
- `cache_setup()` (`hal/cache.c`), right after bss is cleared: L1D and L2 written back again, L2, L1P and L1D all
  cache (mode 4 each, read back), and every writable MAR (16-255) set: cacheable for DDR only. L1D is all cache
  rather than the original's half, since Enaga uses no on-chip RAM. The log's second line gives the modes and DDR
  MAR bits at entry and after set-up.
- The emulator keeps these registers but models no caches; its own hand-over is L2 all cache with DDR uncached.
  The new test writes the simulated BOOT 1.0.17 state into the registers before the first instruction and checks
  the log (`CACHE 043 8F>444 FF`) and every register afterwards; keeping the bootloader's MAR 17 and 128 fails it,
  as does leaving `cache_setup` out. The cache operations themselves are not observable there (no MMIO trace of
  this block), so their order is reviewed against SPRUFK5A/SPRUG82 only.

### First unit session prepared (2026-11-15)

- `tests/unit/check` (CHECK) is one app for the whole session, since Enaga loads only the first app it finds:
  an AUDIO page (left-only, right-only and both tones, input peaks), a LOAD page (SYNTH, SAMPLE and GRAIN voice
  workloads, 8 MiB sample bank in DDR, cycles per voice and voices that fit) and a PANEL page (key names and press
  counts for the bounce check, encoder positions, pad readings; held keys and pads light their LEDs). Pages change
  on HOME held one second.
- Overloading the callback past its budget took every cycle: the main loop, and so key presses, stopped, and the
  watchdog would have dropped the app. The LOAD page caps itself from the audio side: a block over 90 % cuts the
  voices to fit 85 % (one overrun at the step), which also makes FIT a direct reading.
- Emulator baseline (no caches or bus latency): about 2,500 cycles per voice per 64-frame block for all three
  workloads; GCC's saw + biquad loop is about 39 cycles a frame.
- `tests/test_check.py` runs CHECK on the emulator: the tones' channels, the cap holding with keys still served,
  and a held key's LED. `tools/unit_card.py` stages the update and the app with their SHA-256.
- `tools/unit_dry_run.py` walks the whole checklist on the emulator from the simulated BOOT 1.0.17 hand-over: 139
  checks (status texts, the cache line, block rate, power-off releasing GP4[11], channel separation, the load cap
  per workload, every key's LED, encoders through their GPIO phases, all 15 pads' LEDs) and a screen per step.
  The screens found what the checks could not: the font had no `>` or `%` (both added to `hal/gfx.c`, after `Z`
  so the `?` fallback keeps its index), and PEAK on the LOAD page stayed at the cap's one over-budget block, so
  it was replaced by LOAD in percent. Received audio follows the transmit side's belief: slot 1 reads as left.

### Function keys named F1-F8 (2026-11-16)

- The 2 x 4 function block left of OK / CLR is printed differently on the SmplTrek, Lofi-12 XT and CyDrums, which
  share the panel, so Enaga names it by position: F1-F4 the upper row, F5-F8 the lower (`ENAGA_KEY_F1`-`F8`;
  docs/hardware.md "Keys" maps them to bits and SmplTrek labels). The old names (`ENAGA_KEY_FUNC`, `HOME`, `REC`,
  `PROJECT`, `PLAY`, `STOP`, `MIXER`, `EDIT`) are gone; OK, CLR, the D-pad, STEP and the knobs keep theirs.
- The menu chord is F1 + POWER (func on all three products); BOOT MODE is F4 + POWER, the SmplTrek bootloader's
  HOME. The CHECK app shows the new names and changes page on F4.
