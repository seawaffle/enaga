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

E2 has begun ahead of that session, driven by its exit app: pads as press / release with velocity, the start of
the DSP library (`include/enaga/dsp`, tested on the host), and `examples/synth`, an eight-voice virtual analogue
synthesizer (emulator-tested). Apps can build their inner DSP loops with TI's cl6x (`*.ti.c`, GCC as the
fallback); the synth's worst case is 37 % of the audio budget that way and 63 % with GCC alone. Asynchronous file
I/O and sample streaming, the rest of E2, are not started.

## Open items

- On the unit: F1 sampled during the power-key hold; the bootloader passes control normally with F1
  held; F1 + POWER mid-run; power-key bounce; bounce of keys other than STEP 1 (STEP 1 settles within 784 µs,
  smpltrek session M1, well inside the 6 ms sampling and two-sample release).
- LED map: 53 addresses known only from the code, 24 seen on another channel of their LED (docs/hardware.md
  "LED map"; smpltrek session L1); the level-to-brightness steps. Whether the unit has a hardware MIDI port;
  left/right on the line outputs and the inputs (headphones confirmed, M1); real audio CPU headroom with caches; the key names, confirmed key by key
  on a unit.
- The L2 split between cache and RAM.
- The SDK should give apps unaligned-safe load/store helpers for packed formats (misaligned LDW/STW/LDDW round
  the address down without a fault) and say that a null read returns garbage rather than trapping (smpltrek
  session M1; docs/hardware.md "Processor and memory"). A debug-build null check is worth considering.
- Pad events: Enaga passes every MCU message on, about 3,930 raw pad events a second at rest, and press / release
  with velocity at provisional thresholds (press 800, release under 600, velocity 127 from 12,000). To set from
  readings on a unit: the release threshold, the velocity scale, whether one reading after the threshold gives a
  usable velocity (a pad is read every 3.8 ms), per-pad calibration.
- Synth on the unit: its CPU load with real caches (worst case on the emulator 37 % with cl6x, 63 % GCC only),
  the pad keyboard's feel.
- DSP library and cl6x: the library is header-only C++, so apps compile it with GCC. Its inner loops could move to
  block functions in C built by cl6x (a library apps link, or functions in Enaga), plus low-level building blocks
  (vector mix, FIR, biquads, interpolated delay reads) for custom DSP. Before shipping cl6x-built code, check that
  it calls none of TI's run-time library (the link would show it: apps link only libgcc) and TI's licence terms.
  The branch-free style that cl6x needs made the synth's GCC fallback slower (59 % to 63 %).
  Saving patches waits for the file API; edits last until the app stops.
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

### SD error flags and merged audio completions (2026-11-16)

Two bugs reported from smpltrek's bring-up firmware (commit cdd968f) had the same pattern in Enaga.
- `hal/mmcsd.c`: the data-phase waits checked only the wanted flag, so a CRC error or time-out in the same
  MMCST0 read as DATDNE, DRRDY or DXRDY passed. They now fail on any error bit (`data_ok`). The emulator
  cannot inject SD errors, so this is a code fix without a test.
- `kernel/audio.c`: the transmit and receive buffers were chosen by the parity of a completion count, and
  completions that arrive while the pending bit is still set merge. Both are now taken from the PaRAM
  addresses (the buffer not in flight). The transmit buffer is chosen after the app's callback, so a callback
  that runs past its block still writes the buffer that is not playing; the same buffer twice in a row counts
  the replayed block in `audio_blocks` and as an overrun. `tests/apps/stall` and
  `test_long_callback_never_tears_a_block` stall one callback for 2.5 and 3.5 blocks: with the old code a
  played block came out half old, half new.
- Enaga's audio runs from the completion interrupt, not a poll, so merging needs interrupts held off for more
  than a block (a callback overrunning, or a long section with GIE clear); the PaRAM choice holds either way.
  The smpltrek note about reading EDMA registers before its power module is on does not apply: the interrupt
  is enabled only after audio set-up.

### Session M1 results (2026-11-16)

smpltrek's bench session M1 measured on the unit what Enaga had assumed (docs/hardware.md now marks each
as measured):
- The bootloader's cache hand-over matches the simulation, so the unit check's `CACHE 043 8F>444 FF` stands.
- STEP 1 bounces for at most 784 µs: the 6 ms two-sample release filter is ample.
- The panel MCU sends every channel all the time, about 4,192 messages (12,600 bytes) a second, so UART1's
  16-byte FIFO fills in about 1.3 ms. Enaga polled it once per main-loop pass: on the emulator, at 85 % audio
  load on CHECK's LOAD page, polling lost 94 of 840 messages. UART1 now has a receive interrupt (CPU interrupt
  6 <- event 46) filling a 1 KiB ring that `uart1_poll` parses; it may nest in the app's audio callback, whose
  block can last up to 1.33 ms. `uart1_overruns` counts LSR OE. `test_check.py` floods 840 messages at the
  measured rate under that load and expects all of them parsed with no overrun.
- CHECK's PANEL page showed the last pad, which with continuous traffic would cycle through every pad. It now
  shows the pad with the highest reading and pad messages per second, about 3,930 at rest: a direct check of
  the receive path on the unit. The dry run feeds the MCU's continuous stream in step 10 (141 checks).
- Audio (part A, headphones and speaker only): even buffer words play left only and odd words right only on
  headphones, with the same one-word-short first transfer Enaga uses; the speaker plays once GP2[8] is low.
  Line outputs and inputs were not tested (no equipment), and the LED map (part F) was postponed.
- Recorded without code changes: null reads return leftover data and do not fault; misaligned accesses round
  down without faulting; an EMIFA chip-select-0 store with EMIFA not set up is dropped; USB0's bring-up order
  and reserved registers; both SPI flash chips answer `C2 20 18`, and chip 0 has block-protect bits set.

### Synth example and the start of E2 (2026-11-17)

Started ahead of the first unit session, since none of it needs the unit (the emulator is cycle-level, so CPU
load is measured there):
- Pads as notes: `ENAGA_EVENT_PAD_PRESS` (velocity 1-127) and `ENAGA_EVENT_PAD_RELEASE`, sent before the raw
  reading that caused them; `on_pad_press` / `on_pad_release` in `enaga::App`. Thresholds in `kernel/main.c`,
  provisional (see Open items). Tested in test_e1 through the keys app.
- The DSP library, header-only so apps need no build change and the host can test it: `math.hpp` (exp2, log2,
  sin_turns, tan_pi, soft_clip, Random; nothing divides), PolyBLEP oscillators, the trapezoidal SVF, ADSR and
  LFO, a delay line, stereo delay and an eight-line FDN reverb. `tests/test_dsp.py` compiles `tests/dsp/host.cpp`
  with the host g++ and checks accuracy, pitch, filter response, envelope times, delay time and reverb decay.
  GCC inlines float division on the C674x (no libgcc call), and the synth has no double-precision code.
- `examples/synth`: two oscillators with sub and noise, LP12 / LP24 / BP / HP with drive, three ADSRs (amp and
  two assignable), two LFOs (free or key-synced), an eight-slot matrix (sources ENV2, ENV3, LFO1, LFO2, velocity,
  pad pressure, key, per-note random; 13 destinations), POLY / MONO / LEGATO with glide, delay (stereo or ping-
  pong) and reverb, nine factory patches. Modulation runs every 16 frames (3 kHz), the amplitude envelope per
  sample. Pages on F2-F8; the controls were reworked the same day (next entry). Pads are a piano keyboard by their physical rows (docs/hardware.md "Panel MCU").
- Measured on the emulator: reverb and delay alone 16 % of the audio budget; eight voices of INIT 43 %; the
  worst case (both oscillators, sub, noise, LP24 with drive, all eight slots) 67 %. `tests/test_synth.py`
  checks pitch from keys and pads, the worst case under 80 %, knobs, pages, patches, and MONO's note priority.

### Synth controls reworked; an emulator bug in predicated ADDKPC (2026-11-17)

The owner's model for editing, now the synth's: a page holds groups of at most four parameters, one per knob
A-D; the selection buttons under the screen flip between the groups; the D-pad has no job on parameter pages.
F1 is the app's function button, as in the stock firmware (Enaga's menu is the F1 + POWER chord, decision 3,
so F1 alone belongs to the app): F1 + F2 opens PATCH, where VALUE chooses and OK loads. Pushing a knob puts its
parameter back to the patch's value; edited values show dimmer than stored ones. The five-parameter groups were
split (FX gained DELAY ROUTING with the delay's mode and a new delay-to-reverb send; VOICE gained KEYS with the
pad layout and a new STEP velocity).

The rework crashed the synth on the emulator: a value bar's fill ran 22 KB past the canvas and wiped kernel
data. Traced to the emulator, not Enaga or GCC: libgcc's `__c6xabi_divi` issues `[a1] addkpc ..., b3, 4` in the
delay slots of its call to `__c6xabi_divu`, relying on ADDKPC's NOP cycles happening even when the predicate is
false (as BNOP's do). The emulator (smpltrek bcce9b3, `core/core.cpp`) drops them for a false predicate, so with
both operands non-negative divi falls into the next packet's `b b5` and returns twice; the caller's loop then
runs with two branches in flight. smpltrek's emulator now has the two-line fix (in its working tree after
bcce9b3, not yet committed there), and all 31 tests pass against it; emulators without it fail `test_synth.py`.

### cl6x for apps' inner loops (2026-11-17)

Question from the owner: does GCC limit DSP apps, against TI's compiler? Measured on the emulator (fixed core),
kernels modelled on the synth's loops, one 64-sample block, cycles (GCC -O2 / cl6x 8.5.0 -O3): LP24 filter
4314 / 1407, 32-tap FIR 29461 / 9177, mix 916 / 162, PolyBLEP saw 2885 / 2163, envelope 2715 / 3607. The two give
identical samples. GCC's tic6x backend does no software pipelining (`-fmodulo-sched` changes nothing) and put
nearly all of the float work on the A side, so one multiplier; `-funroll-loops` helps it a lot (FIR 13781, mix
387; synth worst case 67 % to 59 %), and four accumulators in the FIR bring GCC to 8661. cl6x refuses to pipeline
a loop with branches in it ("Loop contains control code"), so its gains depend on writing loops with selects.

Building Enaga itself with cl6x would not help: its own share of an audio block is about 6,000 cycles (1.2 %).
The time is in the app's DSP, which apps compile. So the app build now takes `*.ti.c` files to cl6x when it is
installed (`tools/paths.py` TI_CGT: `ENAGA_TI_CGT`, else the newest `~/ti/ti-cgt-c6000_*`; `none` to switch it
off): far data model (apps have no data page pointer), function subsections for `--gc-sections`; `runtime/app.ld`
takes `.text:NAME` and drops cl6x's `.TI.*` and `__TI_*` sections. The same file builds with GCC when cl6x is
absent, and exports plain C functions. Everything else gains `-funroll-loops`. The owner preferred not to tie
developers to library functions: any app can put its own loops in a `.ti.c` file.

The synth's voice loops (oscillators, noise, filter) moved to `examples/synth/voice.ti.c`. Timed per stage with
TSCL in the worst case (GCC alone / cl6x): oscillators and noise 28.3 % / 6.3 %, filter 6.6 % / 3.0 %, modulation
8.3 %, amplitude and pan 5.8 %, reverb 9.4 %, delay 1.4 %; worst case 63 % / 37 %. Getting there took a
branch-free oscillator: the phase as a 32-bit fraction of a turn (wrapping by itself, top 24 bits as the float
phase) and a PolyBLEP that computes both corrections and selects one. With branches cl6x managed only 23 % for the
oscillators. The noise now has its own xorshift state. The emulator's packet-sampling profiler under-counts code
with many NOPs (GCC's), so the per-stage figures come from TSCL reads in a scratch copy. New test: the cl6x and GCC
builds of the synth give identical samples (skipped without cl6x). 32 tests pass.
