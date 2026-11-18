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

## Files (E2 design, 2026-11-18)

What E2's file service looks like to an app, and how Enaga runs it. The owner decided the write area, blocking
calls and exFAT on 2026-11-18 (marked **decided**); the rest is the proposal they accepted.

### What exists

FatFs (R0.15a, read-write, 8.3 names only, no exFAT, no clock) over `hal/mmcsd.c`: polled, one 512-byte block per
command, 4-bit bus at 23 MHz, the CPU copying every word. Only the app loader uses it, in 32 KiB `f_read` calls
with the watchdog fed between them. The card's real throughput on a unit is not measured.

### How it runs: one queue, worked in slices in the main loop

There are no threads, so "asynchronous" means Enaga does the work in the main loop between the app's
callbacks. A request (open, read, write, list a folder, ...) goes into a queue; each main-loop pass Enaga works
on the queue for up to a time budget (**proposal:** 2 ms, in chunks of at most 8 KiB per FatFs call), then
returns to inputs, `update` and `draw`. A finished request's completion callback runs in the main context, like
any other event. The audio context never waits for the card: it is interrupt-driven and keeps running whatever
the main loop is doing.

So a 1 MiB sample loads in the background while the screen and controls stay live; the cost is that a load
takes longer than the card alone would (the budget per pass). Later the SD driver can move to EDMA with a
completion interrupt, freeing the CPU during transfers; the API does not change.

### What an app sees

C functions in the host table, wrapped by C++ classes (`enaga::File`, `enaga::Folder`, `enaga::files`):

- **Handles, not FatFs objects.** Enaga keeps the open files in its own memory (an app cannot corrupt them),
  at most 8 at a time per app (**proposal**); everything still open is closed, and every pending request is
  cancelled with no callback, when the app stops (the switching sequence's step 3).
- **Asynchronous calls:** `read(handle, buffer, bytes, callback)`, `write(...)`, `list(path, callback)` (one
  entry per callback, then an end marker), `load(path, buffer, max, callback)` (open, read whole, close: the
  common case). The buffer belongs to Enaga until the callback; the callback gets the byte count or an error.
- **Blocking calls for small files:** `read_file(path, buffer, max)` and `write_file(path, data, bytes)`, which
  stall the main loop (not audio) until done and feed the watchdog. Meant for settings and patches, a few KiB:
  milliseconds. They are what `init()` and `stop()` use, since a request still pending when `stop()` returns is
  cancelled. (**decided:** both blocking and asynchronous calls.)
- **Safe saves:** `write_file` writes `name.TMP`, syncs, then renames over `name`, so a card pulled or power
  lost mid-save leaves the old file or the new one, never half of each.
- **Errors** as small negative codes: no card, not found, exists, full, denied (outside the app's area),
  too many open, card error, cancelled.

### Where an app may read and write

**Proposal:** an app reads anywhere on the card and writes only in its own folder, `/ENAGA/DATA/<NAME>/`
(created on first use; `<NAME>` is the package name, so it survives reinstalling the app), plus the shared
user folders `/SAMPLES/` and `/RECORD/`. Paths are absolute card paths; Enaga refuses a write elsewhere and
`..` anywhere. (**decided**)

### Names and card formats

Long file names: FatFs's LFN support on (`FF_USE_LFN`; a few KiB of code and a 512-byte name buffer), ASCII
and Latin-1 shown as is. Cards: FAT12/16/32 as now. Cards over 32 GB come formatted exFAT; FatFs supports it
(`FF_FS_EXFAT`), and the format's specification has been public since 2019. (**decided:** exFAT from the
start.) No real-time clock is known on the board, so files get a fixed date.

### Card removal

The card-detect pin (GP4[0]) is read every pass. When the card goes, every request fails with "no card", the
open handles become dead (calls on them fail), and the app gets an event; when a card comes back Enaga mounts
it and sends another. An app that was saving keeps its data and can save again.

### Streaming (second step)

A stream is a file Enaga reads ahead into a ring buffer for the audio context: `stream_open(path, offset,
ring_bytes)`, then in `audio()` `stream_read(stream, out, bytes)`, which never waits (an empty ring gives what
it has and counts an underrun). Enaga tops up the emptiest rings first in the main loop's file budget. Playing
16-bit stereo at 48 kHz takes 192 KiB/s per stream; how many streams a card can feed is the measurement the
next unit session should take (a card-speed page in the CHECK app: sequential and scattered reads, writes).
WAV parsing (`enaga::wav`) sits on top: header, format, data offset.

### Order of work

1. LFN and exFAT on; the request queue and its slice in the main loop; handles; blocking and asynchronous reads, writes
   and listing; the app's data folder; safe saves; card removal. Tested on the emulator with FAT card images
   (the emulator already models the card), including a full card and a removal signalled on the card-detect
   pin (the emulator drives that pin; a card that stops answering mid-transfer is not modelled).
2. The synth saves and loads patches (E2's exit app gets storage).
3. The card-speed page in the CHECK app; the next unit session measures it.
4. Streams and WAV, sized by those measurements.

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

E0 and E1 are complete. E1's kernel, audio engine, display DMA, exception handling, watchdog, key debouncing
and cache configuration ran on a unit in the first session (2026-11-18, results below and in docs/hardware.md);
its exit measurement, audio headroom with caches on, is taken: Enaga's own share is about 3 % of the audio
budget, a computed voice costs about 1,870 cycles a block and one reading samples from DDR about 2,600-2,700.

E2 has begun, driven by its exit app: pads as press / release with velocity, the start of
the DSP library (`include/enaga/dsp`, tested on the host), and `examples/synth`, an eight-voice virtual analogue
synthesizer (emulator-tested; on the unit: 8 % idle, 24 % with eight notes). The library's block functions, and apps' own `*.ti.c` files, are built with TI's
cl6x when it is installed (GCC otherwise); the synth's worst case is 30 % of the audio budget that way and 65 % with
GCC alone (every feature on: sync, ring, chorus, eight voices). Asynchronous file
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
- **Pad velocity does not work on the unit** (session 1): velocity is the first reading over the 800 threshold,
  which with readings every 3.8 ms and a rising press is nearly always just over 800, and full scale is 12,000
  while a hard press reads about 3,400. Needs the peak over the first readings of a press, scaled to the measured
  range; best chosen from recorded press curves.
- Pad crosstalk (session 1): pressing a pad raises some other pad's reading slightly (0 -> 4, 1 -> 5, 4 -> 2; 2 and 3
  none), never to a press in the synth. Size not measured.
- Pad events: Enaga passes every MCU message on, about 3,930 raw pad events a second at rest, and press / release
  with velocity at provisional thresholds (press 800, release under 600, velocity 127 from 12,000). To set from
  readings on a unit: the release threshold, the velocity scale, whether one reading after the threshold gives a
  usable velocity (a pad is read every 3.8 ms), per-pad calibration.
- Synth on the unit: measured 8 % idle, 10 % one note, 24 % eight notes (INIT), 25 % SUPERSAW two notes (eight
  voices), no dropouts; the worst case (30 % on the emulator) not yet measured there. Saving patches waits for the file API; edits last until the app stops.
- DSP library: the SVF runs at 21 cycles a sample under cl6x (its recurrence), twice a biquad's 10; a faster
  form is worth trying. The synth's modulation is 6.4 % of the budget with cl6x, most of it the scalar part
  (matrix 1.5 %, LFOs 1.1 %, envelopes 0.7 %); GCC compiles the math-buffer loops poorly (about 80 cycles an
  exp2, mostly NOPs and constants rebuilt per use), so the GCC-only build lost 3 points to the restructure. The library is built from source with each app for now; a
  release SDK would ship it prebuilt. Before shipping cl6x-built code, check that it calls none of TI's run-time
  library (the link would show it: apps link only libgcc) and TI's licence terms.
- USB stack: the C6748's USB0 is a Mentor MUSB controller; a TinyUSB port (TinyUSB has a MUSB driver for
  other TI parts) is plausible and unverified.
- A fault in Enaga code called by an app (a host function given a bad pointer) currently counts as a kernel
  crash rather than the app's fault.
- Hangs with interrupts disabled (in a kernel handler, or an app that clears GIE) escape the software
  watchdog; the C6748's hardware watchdog (TIMER64P1) would catch them by resetting the chip, which probably
  also drops the power hold. Whether a reset on the unit switches it off or restarts Enaga is to be checked.
- Loading a very large app: the loader's CRC and bss clearing run without feeding the watchdog, so an image
  that takes over a second to check would trip it. To be measured on the unit with caches on.
- Interrupt stack gap (firmware/start.s): GCC code is held to 16 KiB frames by the build; libgcc (prebuilt, small
  frames) and cl6x-built code are not checked. cl6x is not known to release frames early. An app that needs a
  bigger local array puts it in a static or on the heap.
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

### The DSP library on a C core built by cl6x (2026-11-17)

The owner wants app writers free to write their own DSP and still get a fast product easily, so the library is now
three layers, all optional: the maths (`include/enaga/dsp/math.h`, static inline C, written with selects so that
loops using it can pipeline; `math.hpp` wraps it); a C interface (`include/enaga/dsp.h`) with plain state structs,
per-sample steps (inline) and block functions; and the C++ classes, now thin wrappers whose state is the public C
struct, with block methods added (`Svf::process`, `Svf::cascade`, `OnePole::process`, `Envelope::render`,
`Random::add_noise`). The block functions live in `runtime/dsp/*.c` and `tools/build_app.py` builds them into
`build/dsplib/{cl6x,gcc}/libenaga_dsp.a` (cl6x when installed), rebuilt when a source, a header or the script
changes, and links the archive into every app. Every cl6x loop pipelines (oscillators ii 3-9 per sample, SVF 21,
envelope 15-17, buffer arithmetic 1-6).

The envelope renders without branches: attack, decay and sustain share one loop that selects between the attack
and decay updates (decay and sustain are the same update), release clamps at zero. The oscillator keeps the 32-bit
phase; its PolyBLEP selects for cl6x and branches for GCC (the same arithmetic): GCC was slower with the selects.
`Svf::cascade` runs two filters in one loop so the stages overlap: two separate calls cost the LP24 filter 6.0 %
of the budget against 4.5 %. Host tests check each block function against the per-sample code bit for bit
(they caught deliberately wrong envelope and one-pole code).

The synth uses the library again (`voice.ti.c` is gone): worst case 36.3 % with cl6x (oscillators and noise
6.4 %, filter 4.5 %, amplitude 3.1 %) and 57.6 % with GCC alone, better than the old header-only 59 %. The test
that both builds sound the same now aligns the two captures at their first sound: they matched sample for sample
but three blocks apart, because the main loop, which hands the keys to the engine, runs slower when the audio takes
more of the CPU. 32 tests pass.

### Delay and reverb as block functions (2026-11-17)

`enaga_stereo_delay` and `enaga_reverb` (C state and block functions in `runtime/dsp/effects.c`; the C++ classes
wrap them, with their memory on the app heap as before) and delay-line block reads and writes. Both effects work
in chunks no longer than their shortest delay (the stereo delay's 16-sample minimum, the reverb's first diffuser,
149 samples at 48 kHz, capped at 64): everything a chunk reads was written before it, so each chunk reads its
lines, computes and writes after, which gives the per-sample results and keeps loads and stores of one buffer out
of the same loop. The reverb's eight damping one-poles run side by side in one loop, and the Hadamard transform is
written out. All of the effect loops pipeline under cl6x except the chunk loops, which call functions.

The host test keeps the old per-sample delay and reverb as references and compares bit for bit over uneven block
sizes, setting changes, ping-pong and 22.05 kHz. Its first version never brought the gliding delay time near the
16-sample minimum and so missed a deliberately too-long chunk; it now holds the shortest time long enough.
cl6x subsections constant tables (`.const:NAME`), so `runtime/app.ld` places `.const:*` and the other data
subsections.

Synth worst case (GCC alone / cl6x): reverb 9.4 % to 4.3 % / 1.3 %, delay 1.4 % to 1.2 % / 0.4 %; the whole worst
case 52 % / 27 % of the audio budget. 32 tests pass.

### Biquads, FIR, wavetables and modulated delay reads (2026-11-17)

The building blocks the Open items listed, as block functions in `runtime/dsp` with C++ wrappers:
`enaga_biquad_*` / `Biquad` (direct form I; `enaga_biquad_design` gives the Audio EQ Cookbook's eight shapes; a
two-stage cascade like the SVF's), `enaga_fir_*` / `Fir` (the app's taps; a history of length - 1 + 64 floats on
the app heap), `enaga_wavetable_add` and `_morph_add` on the oscillator's phase (`Oscillator::table_add`,
`morph_add`; tables of 2^bits samples and a guard sample, band-limiting left to the app), and
`enaga_delay_read_linear` / `DelayLine::read_linear` (a per-sample delay array, every delay[i] >= i). `Memory`
moved to `math.hpp` so filters can use it.

The biquad design computes 1 - cos w as 2 sin^2(w / 2): the plain form lost 1e-4 of gain at 1 kHz to cancellation,
and would lose about 10 % at 50 Hz. The FIR computes eight outputs per pass over the taps, eight sums in flight
against the adder's four-cycle latency, each summed in tap order, so the results match the plain loop. cl6x puts
switch tables in `.switch:NAME` sections; `runtime/app.ld` places them. The cl6x library calls C6000 EABI helpers
(`__c6xabi_divf` for float division, `__c6xabi_fixfu`), which libgcc provides under the same names; nothing from
TI's run-time library.

Host tests: each block against a per-sample loop bit for bit (uneven block sizes, in place, FIR lengths 1 to 150
across the 64-sample chunk), biquad responses at their defining frequencies (30 Hz included), the wavetable
against sin to 2e-6 and backwards; deliberately wrong FIR, delay, wavetable and high-pass code was caught.
Emulator, one 64-sample block, cycles (GCC / cl6x; the same samples): biquad 1699 / 628, two in series 3032 / 664,
FIR 16 taps 3269 / 1071, 128 taps 20559 / 6223, wavetable 1226 / 315, morph 2155 / 397, modulated delay read
1180 / 306; for comparison the SVF 1444 / 1395. 32 tests pass.

### The synth's modulation in blocks across voices (2026-11-17)

Profiled per voice and control step (cl6x build), the modulation was 8.5 % of the budget: envelopes, LFOs and the
matrix 2.7 %, pitch 3.2 % (five exp2 and two divisions), filter coefficients 2.3 % (tan_pi and two divisions),
all scalar GCC code. GCC inlines float division (RCPSP and two refinements, not a libgcc call), so the cost was
long serial chains. `Engine::control_step` now runs the scalar part voice by voice, gathers the exponents
(LFO rates, both oscillators, cutoff), the frequencies to invert and the filters' g and k into arrays across the
eight voices, and converts them with new library block functions: `enaga_exp2_buffer`, `enaga_tan_pi_buffer`,
`enaga_reciprocal_buffer`, `enaga_svf_coefficients_buffer`. `render_voice` keeps only the audio.

`enaga_reciprocal` (math.h): 1 / x by three Newton steps from a guess made of the bits, relative error under
2e-7, only multiplies and adds. A division in a loop is a call under cl6x, which stops it pipelining; the
reciprocal also gives the same bits under both compilers, where GCC's estimate-based division and cl6x's exact
routine can differ in the last place. `tan_pi` and `SvfCoefficients::from_g` use it, so scalar and block forms
agree exactly. cl6x pipelines exp2 at 7 cycles a value, tan_pi 16, reciprocal 5, but its pipelined exp2 and
tan_pi loops need ten iterations: eight cutoffs went through the plain fallback loop (1,000 cycles a step), so
the synth converts sixteen. Accumulating the matrix into the `Control` struct's array cost 30 % more than a
local array (the stores might alias the settings).

Result, worst case: cl6x 27 % to 25 % (modulation 8.5 % to 6.4 %: block math 1.2 %, the scalar part 5 %); GCC
alone 52 % to 55 %, since GCC's code for the buffer loops is poor (above) and the padding doubles its tan_pi work.
A four-way interleave for GCC and -O3 did not help. 32 tests pass.

### No float division in the library's C code; the emulator agent's GCC notes (2026-11-17)

From smpltrek's emulator work: GCC 13.3 for tic6x divides floats by a reciprocal estimate (RCPSP, refined
twice), not exactly, so its quotients can be a unit in the last place off the host's, and cl6x's exact
`__c6xabi_divf`; the estimate's bits are not yet checked on hardware either. `-fno-reciprocal-math` would make
GCC exact at a cost. The library's C code (built by either compiler) no longer divides: the biquad design, the
one-pole coefficient and the reverb's gains use `enaga_reciprocal`, the effects' rate scale a constant
reciprocal, so cl6x and GCC builds give the same coefficients by construction and the cl6x library calls no
division helper (only `__c6xabi_fixfu`, an exact conversion). Apps' own C++ still divides the GCC way, the same in
both builds. The same note: `_smpyh`, `_smpyhl` and `_smpylh` from GCC's `c6x_intrinsics.h` emit plain SMPY
(wrong halves); Enaga uses none, and should write half selection in C, `(x >> 16) * (y >> 16)`. And the emulator
gains about 30 instructions GCC can emit (MPYH and family, saturating arithmetic, the packed ADD2 / SUB2 / ADD4 /
MAX2 / SWAP4) in smpltrek's working tree, not yet committed: rebuild the emulator and rerun the tests after that
commit. 32 tests pass.

### Synth: chorus, unison, hard sync, ring modulation, LFO fade-in (2026-11-17)

The owner kept `synth-modulation` (cl6x 25 %, GCC alone 55 %) and asked for these five, offered after the synth's
first version. Ten parameters, added at the end of the table so earlier numbers (and patches) stay: OSC MOD
(SYNC, RING) on the OSC page, FADE on each LFO, CHORUS (RATE, DEPTH, MIX) first on the FX page, UNISON (VOICES
1-4, DETUNE in cents at the outermost voices, SPREAD) on the VOICE page. Three patches show them: SYNC LEAD (OSC 2
alone, synced, ENV2 sweeping its pitch, vibrato fading in), SUPERSAW (four voices, chorus), RING METAL.

- Chorus: `enaga_chorus` / `Chorus` in the library, two lines read at 8 ms +-5 ms swept by a sine, the channels a
  quarter turn apart, on `enaga_delay_read_linear` in 64-sample chunks (the depth is capped so every delay stays
  over the chunk). In place; the synth runs it on the voices before the delay, and with MIX at 0 only feeds its
  lines, so turning it up plays nothing stale. 0.6 % of the budget.
- Hard sync: `enaga_oscillator_sync_add` / `Oscillator::sync_add`. Every jump, the restart's and the wave's own
  (and a pulse's falling edge, also when it falls in a restart's sample), gets the two-sample PolyBLEP correction
  -J/2 (1 - a)^2 now and J/2 a^2 on the sample before; the loop runs a sample ahead so the output stays in step
  with `enaga_oscillator_add` (`carry` holds the sample in hand). The master is read, not moved; a silent master
  moves with `enaga_oscillator_advance`. A loop per wave written out by a macro: cl6x would not inline a generic
  helper (it emitted calls, which stop pipelining), and the per-wave copies pipeline at 12 (saw), 24 (pulse) and
  15 (triangle) cycles a sample; sine, with three sin_turns a sample, does not pipeline. Branch-free for cl6x,
  branching around the restart for GCC, the same arithmetic: the cl6x / GCC test now runs with sync, ring and
  chorus on and still matches sample for sample.
- Ring: OSC 1 x OSC 2 (`enaga_mix_product`) into the mixer. With sync or ring on, the two oscillators render to
  their own buffers.
- LFO fade-in: a per-voice linear ramp from note-on over FADE, multiplying both LFOs' outputs.
- Unison: a note takes VOICES voices (POLY: allocated as usual, so polyphony divides; MONO / LEGATO: voices 0 to
  N - 1), each at its place -1..1 for detune and pan, from random oscillator phases (one phase would make them
  one), gain 1 / sqrt(N).

Host tests: the chorus against a per-sample reference on `DelayLine` (and in place, two rates); the synced
oscillator without restarts equal to `render_add` (saw and pulse), equal over block sizes, periodic at the
master's period, smoother than naive sync, a sine following the exact synced phase with restarts between
samples, and a pulse's mean matching the continuous wave's at two ratios (the edge 0.7 and 0.07 of a sample
before the restart). Deliberately wrong code at each jump correction, the restart's phase and the chorus's
quadrature was caught; the first versions of the tests missed the restart phase (restarts on whole samples) and
the edge-before-restart terms, so the last three checks were added. Emulator tests: sync makes OSC 2 a fourth up
play at OSC 1's pitch; ring sounds with both oscillator levels at 0; a centred voice is identical left and right
until the chorus is up; three unison voices a note, in tune; a square LFO on pitch an octave up at once, and
barely moved a quarter second into a 19 s fade.

Worst case, now with sync, ring, LFO fade and chorus on: cl6x 30 %, GCC alone 65 % (oscillators 10.2 % / 28.7 %;
the first, branchy sync loop took cl6x to 35 %). 34 tests pass.

### GPIO enabled at start-up; a GCC stack hazard under interrupts (2026-11-17)

Enaga now enables GPIO (PSC1 module 3) itself, before the power hold, as the original firmware's board set-up
does. The bootloader leaves it on (smpltrek's own firmware, which never enables it, drove the power hold and
read keys on a unit in session M1), so on a unit nothing changes; it lets Enaga start from the power-on state the
emulator's panel (`stock_ui.py`) uses, so the panel runs Enaga with no wrapper:
`stock_ui.py --firmware build/firmware/ENAGA_0_1_0.bin --card <card with /ENAGA/APPS/...>`. It runs at about
0.5-0.6x real time on the development host, so its sound breaks up.

The smpltrek emulator work found that tic6x GCC 13.3 at -O2/-O3/-Os can raise SP (release the frame) a few
cycles before a function's last reads of a local array; an interrupt there, pushing onto the interrupted stack,
overwrites the array. Reproduced in Enaga: `tests/apps/stack` calls such a function (`addk 72, b15` in the same
packet as the last store, two loads after) 2 million times under audio interrupts: 6 wrong results. Their
suggested fix, `-fno-schedule-insns2`, costs too much here (synth worst case cl6x 30 % -> 40 %, GCC alone
65 % -> 104 %: it is GCC's scheduler for this VLIW CPU). Instead each interrupt vector first moves SP 16 KiB
down, and back on return (one instruction each, in slots that were NOPs), so a just-released frame is not
written; the build makes any GCC frame over 16 KiB, a VLA or alloca an error (the largest today is the reverb's
2,824 bytes). After: 0 wrong, regression test in test_e1. The C interrupt handler was already entered through an
assembly stub saving the registers GCC needs (their other note: GCC ignores `__attribute__((interrupt))`). 35
tests pass; the unit dry run passes 141/141.

### The main loop sleeps between interrupts (2026-11-17)

From the emulator agent: the emulator ran Enaga slowly mainly because `kernel_loop` never slept. Each pass now
ends with IDLE; the audio block (every 1.33 ms), UART1 (each panel MCU byte, about 12,600 a second on the unit)
and the timer (10 ms) wake it, so a pass still runs at least 750 times a second, faster than the original
firmware's 1 ms encoder poll. Work an interrupt leaves after a pass's polls waits at most one audio block; the
UART ring holds 80 ms. Measured in the panel (`stock_ui.py`, compiled blocks), synth with four notes held:
0.27x real time before, 0.81x after. The whole test suite went from 265 s to 109 s. Checked: 35 tests (watchdog,
keys, pads, display DMA among them) and the unit dry run (141/141) pass; in the panel with the MCU stream off
(audio the only regular wake), 27 fast detents on a knob (2 ms a phase) all counted. On the unit it also lowers
power draw. `update` is no longer called flat out: background work in it gets at least 750 calls a second.

### tools/panel.py (2026-11-17)

`tools/panel.py [app_dir ...] [-- panel options]` builds the firmware and apps (examples/synth by default), writes
them to a fresh 64 MiB card image and starts the smpltrek emulator's web panel (`stock_ui.py`) on Enaga's update
file, so apps can be played live with the display, controls and sound. Panel options go after `--` (some take
paths, so they can't be told from app directories otherwise). Tried with the synth and the tone app.

### First start on a unit: a watchdog crash (2026-11-18)

The first Enaga run on a unit (unit-session-synth build, d62587f2...): the install worked; the splash went by
quickly, the status page showed `NO /ENAGA/APPS` for under a second, then the kernel crash screen with WATCHDOG
(its six lines all zero, as they are for any watchdog crash: they are filled only by CPU exceptions). So
interrupts were still taken (the watchdog runs in the timer interrupt) while the main loop stopped feeding it for
a second. Not seen on the emulator. The watchdog's crash screen now shows where the main loop was: IRP when it
tripped (the interrupted code), the loop's stage (1 inputs, 2 update, 3 power key, 4 display DMA poll, 5 draw,
6 present, 7 idle), passes, interrupts taken on CPU interrupts 4 (timer), 6 (UART1), 8 (audio), audio blocks.
Two diagnostic images in `build/unit-diag/`: A as built, B without the main loop's IDLE.

Diagnostic image A's crash screen (2026-11-18): PC `C00082DC` (the branch after the main loop's IDLE, stage 7: the
loop was parked as designed), 88 passes, 103 timer ticks, 12,715 UART1 interrupts, **658,339 audio interrupts**
for 761 audio blocks (the right count for the second). An interrupt storm on the EDMA's shadow-region-1
interrupt: `audio_isr` clears only channels 0 and 1 and then writes IEVAL, which raises the interrupt again while
any other channel is pending and enabled. Enaga never reset IER, and the bootloader, which copies Enaga by EDMA,
evidently leaves such a completion behind; the emulator starts the EDMA clean, so it never showed. Reproduced on
the emulator by simulating that hand-over (one finished transfer on channel 20, pending and enabled): 2.46 million
interrupts for 1,382 blocks. Fix: audio's EDMA set-up clears every interrupt enable (IECR, IECRH) and every
pending completion (ICR, ICRH) before enabling its own; then one interrupt per block. Regression test in test_e1
(fails without the fix). The idle was not involved; image B was not needed. 36 tests, dry run 141/141.

### First unit session: results (2026-11-18)

Image 9ebb7e07 (the EDMA fix above), CHECK app and synth; every step of docs/hardware.md "Checks on a unit" passed:

1. Install and start: status page with `NO /ENAGA/APPS`, stays up. 2. `CACHE 043 8F>444 FF`, as M1 measured.
3. Audio with no app: status 0, about 46,000 blocks in a minute (750 a second), peak load 3.8 %, 0 overruns.
4. Power: off after a one-second hold, nothing on a short press. 5. The CHECK app starts.
6. Channels: left and right as expected. 7. Input: IN L and IN R follow their jacks.
8. Load with caches (LOAD page; idle 2.5-3.5 %):

| Workload | 16 voices | 64 | 128 | CAP | Cycles a voice (large counts) | Fit at 90 % |
|---|---|---|---|---|---|---|
| SYNTH | 8.7 % | 26.9 % | 50.3 % | 228 voices, 85.9 % | 1,868 | 230 |
| SAMPLE | 12.1 % | 36.2 % | 69.0 % | 163 voices, 85.3 % | 2,592 | 170 |
| GRAIN | 11.5 % | 37.2 % | 70.2 % | 160 voices, 87.0 % | 2,690 | 166 |

   The emulator (no caches) gave about 2,500 cycles a voice for each: computation runs about 25 % faster on the
   unit, and reading samples from DDR costs about 40 % more than computing. Per-voice figures fall with the count
   because Enaga's own share, about 13,000 cycles a block (3 %), is spread wider.
9. Keys and LEDs: every name, count and LED right. 10. Encoders one count a detent; pads: MCU messages at the
   expected rate, at rest RAW 1-2, pressed about 1,500 (light) to 3,400 (hard); crosstalk as in Open items.
11. Synth: starts; load 8 % idle, 10 % one note, 24 % eight notes, 25 % SUPERSAW two notes (eight voices); no
   extra notes, no crackle or dropouts; no velocity response (Open items).

Also confirmed on the unit: GPIO enabled by Enaga, the interrupt stack gap and the idling main loop (keys,
encoders and pads all right with it).
