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

Built 2026-11-18 and 2026-11-19 (E3; see the log): the menu (`kernel/menu.c`), F1 + POWER, F1 at power-on, the boot
policy with the start-up record, switching without a reboot (with the fade and the `stop()` limit), `open_menu`,
and the SETTINGS page ("Settings" below). Where the build differs from the plan below it says so.

Power-on sequence (proposal):

1. The bootloader hands over (it is never modified; F4 + POWER still reaches its BOOT MODE, the recovery
   path, see hardware.md "Installing and recovery").
2. Enaga takes the power hold, starts the display and the key matrix, and samples F1 while the power key
   is still held (the original firmware checks F1 at about 0.58 s for its factory test mode). The
   bootloader only tests F4, so F1 at power-on is Enaga's to use.
3. F1 held: Enaga menu. Otherwise the boot policy in decision 2. (Built: F1 is read when start-up ends, about
   0.7 s after power-on, as "held through start-up"; the keys are not scanned earlier.)
4. A last app that faults or trips the watchdog during start-up is not auto-loaded on the next boot. (Built:
   `/ENAGA/LAST.TXT` holds the last app's folder and STARTING until it has run 2 s, then RUNNING; a boot that
   finds STARTING opens the menu with "<NAME> DID NOT START", every boot until an app chosen there settles. A
   record of an app no longer installed is ignored.)

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

As built: the paused app's audio goes on and it gets no events, `update` or `draw`; it is told of the release of
every key and pad press it was given when the menu opens, and of none it was not. An app can open the menu itself
(`open_menu`, `enaga::system::open_menu()`); it opens once the app's callback returns. The menu lists the apps
under `/ENAGA/APPS` by the display name in each package's header (the folder name if the header cannot be read),
then SETTINGS, USB DRIVE (E5) and STATUS (Enaga's status page); UP / DOWN or VALUE move, OK chooses (the running app resumes,
another replaces it through steps 1-5 above), CLR or F1 + POWER resume. With no app running (a crash, a failed
load, a card without apps) Enaga shows the status page, and OK there opens the menu.

Steps 1 and 2 as built: the output fades to silence over 8 blocks (11 ms) while the app's callback still runs, then
the callback is removed and `stop()` called; a new app's output fades in the same way. `stop()` has 3 s
(`APPS_STOP_MS`, checked by the watchdog tick whether or not the main loop is fed, since saving to the card feeds
it); a `stop()` that overruns, hangs or calls `fatal` drops the app as any failure does, and the switch then goes
on to the chosen app (or the power-off goes on, if it was switching off). App icons came in E4 ("App icons in the
menu", E4 step 6, in the log).

## Settings

Built 2026-11-19. Enaga's own settings live in `/ENAGA/SETTINGS.TXT` on the card, one `NAME=VALUE` line each
(`kernel/settings.c`). The plan named the SPI flash; the card is used instead because the flash holds the original
firmware's sound content, which Enaga promises never to write, and a text file can be read, edited or deleted on a
computer. A missing file, line or value leaves that setting at its default. The menu's SETTINGS page shows them:
UP / DOWN choose, LEFT / RIGHT or VALUE change (applied at once), OK or CLR go back and write the file if anything
changed.

| Setting | Values (default) | Effect |
|---|---|---|
| VOLUME | 0-10 (10) | output gain in the audio engine, 3 dB a step, 0 silent; ramps like the fade |
| BRIGHTNESS | 1-10 (10) | the display controller's contrast command (0x81), 55 to 255 |
| PADS | LOW, MEDIUM, HIGH (MEDIUM) | pad sensitivity: a press from 1,600 / 1,000 / 600, released below 3/4 of that |
| AUTO OFF | OFF, 5, 10, 30, 60 MIN (OFF) | switch off (stopping the app normally) after that long with no key, knob, pad press or POWER, unless the app has called `set_busy` |
| SPEAKER | ON, OFF (ON) | the built-in speaker's mute, GP2[8], as the original firmware's SPEAKER MUTE |
| MIDI CH | 1-16 (1) | the channel apps listen and send on by default (`enaga::midi::channel`) |
| MIDI THRU | OFF, ON (OFF) | MIDI IN copied to MIDI OUT (SysEx excepted), merged with the app's own messages |
| MIDI IN | DIN, USB, BOTH (BOTH) | which ports' messages reach apps, and whose MIDI clock the clock follows |
| MIDI OUT | DIN, USB, BOTH (BOTH) | where apps' messages (unless an app picks a port) and MIDI clock out go |
| CLOCK SRC | INT, MIDI, SYNC (INT) | who drives the tempo clock: the app, MIDI clock at MIDI IN, or SYNC IN pulses |
| CLOCK OUT | OFF, ON (OFF) | MIDI clock and transport at MIDI OUT, following the clock |
| SYNC OUT | ON, OFF (ON) | pulses at SYNC OUT, 2 to the quarter note |
| POL IN | RISE, FALL (RISE) | the SYNC IN edge that counts, with the original firmware's meaning of each label |
| POL OUT | RISE, FALL (RISE) | SYNC OUT's pulse: RISE idles low and pulses high, FALL the other way round |

The SETTINGS page shows nine rows and scrolls to keep the cursor on screen.

The speaker, as in the original firmware: it plays unless the user has switched it off, the app has muted it
(`speaker(false)`, e.g. while monitoring the built-in microphone, where the original firmware mutes it against
feedback), or headphones are plugged in, which cuts it in hardware (a switched jack: smpltrek-re HW-15, confirmed on
the unit in session 3). Switching it fades the output around the change. Apps can read whether headphones are plugged in
(`headphones()`, GP3[8]: 1 with a plug in PHONES, unaffected by the other jacks, confirmed on the unit); the
status page shows it as `H`.

MIDI IN and MIDI OUT are the stock MIDI CONFIG page's IN FROM / OUT TO. An app's own settings are separate: `<enaga/settings.hpp>` gives it a key/value
store in its data folder (`SETTINGS.TXT`, the same text format), built on the file API, with nothing new in Enaga.

## Memory

DDR is 128 MiB at `0xC0000000`-`0xC7FFFFFF` (hardware.md "Memory").

| Region | Size | Owner |
|---|---|---|
| Enaga code, data, stacks, DMA rings | 4 MiB at `0xC0000000`; 0.1.0 uses about 190 KB plus a 256 KB stack | Enaga |
| App image (code, data, bss) | from `ENAGA_APP_BASE` = `0xC0400000`, as large as the app | app |
| App heap | from the end of the image, rounded up to 4 KiB, to the end of DDR (`ENAGA_APP_LIMIT` = `0xC8000000`) | app (`new`, `malloc`, samples) |
| L1P, L1D, L2 | all cache since E1 (Enaga sets it at start-up); L2 as RAM for apps measured and set aside (log, 2026-11-24) | measured on the unit |

So an app has the whole 124 MiB above Enaga for its image plus heap. For comparison, the original firmware's
image and static buffers take about 21 MiB and its default heap is 64 MiB. App assets live on the card in the
app's directory. Enaga never writes the original sound content in the unit's SPI flash, so restoring the
original firmware stays a single system update.

The bootloader runs from `0xC7020000`-`0xC70B0000` only while it loads Enaga; apps are loaded by Enaga after
the hand-over, so that range is ordinary app heap afterwards. Enaga reads it once at start-up, before any app,
to tell which device it runs on (`kernel/device.c`; the log, "Update files for the Lofi-12 XT and CyDrums").

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
- **Time and music:** tempo clock and transport, MIDI in/out with clock sync, on the DIN jacks and over USB
  (both in E4), and the SYNC jacks.
- **Runtime:** `new`/`delete`/`malloc` on the app heap, `printf` to the log, fast float math, a freestanding
  C++ standard library subset, `enaga::system::open_menu()`.
- **System:** battery and power source, auto-off control (apps report "busy"), the crash screen.

## Enaga itself

- **Drivers:** McASP/EDMA, the codec over I2C, the OLED over SPI with frame DMA, key matrix, encoders, the
  panel MCU link (UART), MMCSD, timers, power; USB0 later. (Settings went to the card, not the SPI flash.)
- **Services:** audio engine, event queue, asynchronous I/O, streaming, settings, launcher/menu, watchdog,
  exception handling (crash screen, return to menu).
- **Enaga menu:** app list (name, icon), switch app, system settings (volume, pad sensitivity, MIDI channel,
  brightness, auto-off, speaker; built in E3 but for icons and MIDI channel), Diagnostics (keys, encoders, pads, LEDs, audio routing, card), CPU load, versions.
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
  Prebuilt for Linux x86-64 with each release from E4 (`tools/package_toolchain.py`, with its source); macOS
  and a Docker image are still to come.
- A project template and build tool, example apps (synth, sampler, step sequencer), an API reference (started:
  docs/api.md, kept complete by `tests/test_api_docs.py`).
- Testing at three levels: a host-native app lab for fast UI work (planned), the SmplTrek emulator running
  Enaga and the app with exact audio, frame and card output, and hardware. Every Enaga service gets
  emulator coverage when it is added; scripted scenarios (press keys, assert samples and frames) gate
  changes. The emulator is a separate open-source project (README.md, "Testing without a unit").
- Release hygiene: Enaga installs as a normal system update and never writes the bootloader; the original
  firmware can always be restored through BOOT MODE, documented prominently. No Sonicware code or branding.
  Licence: MIT (decided 2026-11-20; FatFs's BSD-style licence fits it).

## Files (E2 design, 2026-11-18)

What E2's file service looks like to an app, and how Enaga runs it. The owner decided the write area, blocking
calls and exFAT on 2026-11-18 (marked **decided**); the rest is the proposal they accepted.

### What existed before E2

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

C functions in the host table, wrapped by C++ classes. As built (`include/enaga/files.hpp`): `files::read_all` and
`files::save` are the blocking calls below, `files::load`, `files::list` and `files::File` the asynchronous ones,
and `files::Stream` the streams; there is no `Folder` class. The plan as written:

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

### Streaming (step 4, built 2026-11-18)

A stream is a file played into the audio context, or recorded from it, through a ring buffer the app gives (a
power of two, 4 KiB or more, from its own heap): `stream_open(path, offset, ring, bytes)` for playback,
`stream_create(path, reserve, ring, bytes)` for a recording that starts with `reserve` zero bytes, then in
`audio()` `stream_read` / `stream_write`, which never wait (a dry or full ring moves what it can and counts an
underrun). `stream_status` gives the state (filling, running, ended, or the error), position, bytes buffered and
underruns; `stream_close(stream, header, bytes)` writes a recording out to the end, then the header over its first
bytes, so a WAV header with the final size goes in the reserved space. Up to 8 streams an app, besides its 8 files.

The ring is single-producer, single-consumer with free-running counters, so the audio side never locks. In the
main loop's file slice, each step of the request queue follows a step of the neediest stream (a playback ring's
free share, a recording ring's full share), one FatFs call of at most a quarter of its ring and 8 KiB. Card
removal fails every stream with "no card"; an app that stops has its streams closed (recordings flushed); a
dropped app's are discarded.

`<enaga/wav.hpp>` parses a WAV header (PCM 8/16/24/32-bit, float 32-bit, WAVE_FORMAT_EXTENSIBLE, other chunks
skipped), reading byte by byte since the format is packed, and makes the 44-byte header for a recording.

Card speed on a unit: one sector a command, reads 2,860 KiB/s and writes 170 KiB/s, short of one recording; with
up to 128 sectors a command (CMD18 / CMD25, ended by CMD12), reads about 7,600 KiB/s and writes 1,800-2,400 KiB/s,
room for dozens of playback streams (192 KiB/s each for 16-bit stereo at 48 kHz) and several recordings.

### Order of work

1. (**Done** 2026-11-18, see the log.) LFN and exFAT on; the request queue and its slice in the main loop; handles; blocking and asynchronous reads, writes
   and listing; the app's data folder; safe saves; card removal. Tested on the emulator with FAT card images
   (the emulator already models the card), including a full card and a removal signalled on the card-detect
   pin (the emulator drives that pin; a card that stops answering mid-transfer is not modelled).
2. (**Done** 2026-11-18, see the log.) The synth saves and loads patches (E2's exit app gets storage).
3. (**Done** 2026-11-18, see the log.) The card-speed page in the CHECK app; the next unit session measures it.
4. (**Done** 2026-11-18, see the log.) Streams and WAV, and multi-block SD transfers for recording.

## E4 plan (2026-11-20)

Scope, as agreed: full MIDI on the DIN jacks and over USB (USB MIDI moves here from E5; mass storage and USB audio
stay in E5), and a UI toolkit that starts small. Steps in order; each ends tested on the emulator where it can be,
and the ones that need the unit collect into a session.

1. (**Done** 2026-11-20, see the log; feel to be checked on the unit.) **Pad pressure.** An `ENAGA_EVENT_PAD_PRESSURE` beside press / release. Derived on the DSP from the normal pad
   stream, as Lofi-12 XT does (XF-23): a press starts a 40-sample countdown and at 1 the raw value becomes the
   baseline; raw > baseline + 200 enables pressure until release; pressure is the mean of the last 9 raw readings
   passed through a steep curve (Lofi's runs 800, 1277, 1401 ... 3200), scaled to 0-127, and sent only when it
   moves by more than 1/127. Use our own curve fitted to that shape rather than Lofi's table. CyDrums sends every
   sample unfiltered (MIDI Channel Pressure, a flood): copy Lofi's change filter, not that.
2. (**Done** 2026-11-20, see the log; each route to be checked on the unit.) **Input selection.** Model it on Lofi-12 XT and CyDrums: a (source, level) pair, sources built-in mic, Hi-Z L,
   dynamic mic R, stereo line, mono line (USB and resample later), levels 0-2, which maps one-to-one onto the
   SmplTrek's REC SRC routes: codec routing plus the pins GP1[2] (mic switch), GP7[14] / GP7[15] (INPUT L front
   end, +10 dB), GP5[11] (INPUT R mic stage) and GP1[3] (line pad). Table: smpltrek-re `docs/01-platform-map.md`
   "Hardware use per product", Audio (XF-21). Keep the stock rule in Enaga, not in apps: the built-in mic is used
   or monitored only while headphones are in (GP3[8] high), in every product, against speaker feedback. The pin
   roles come from the stock code and want a unit check per route (the next unit session, with the PADS check).
3. (**Done** on the emulator 2026-11-20, see the log; the unit to come.) **MIDI on DIN.** UART0 at 31,250 baud 8N1 (smpltrek-re HW-11: PSC0 module 9, PINMUX3 nibbles 4-5 = 2, divisor 372 at
   186 MHz), interrupt-driven receive and transmit rings. A parser for the whole protocol: running status,
   real-time bytes inside other messages, SysEx of any length (delivered in chunks), system common. Apps get
   messages as events with their port, and send through `<enaga/midi.hpp>` (channel messages, SysEx, real-time);
   THRU (input copied to output) and the MIDI channel as Enaga settings, the channel also in the menu. The stock
   MIDI CONFIG rows (IN FROM / OUT TO DIN, USB or both; clock out; active sensing) are the model for which
   settings Enaga owns. Testing: a UART0 model in the emulator (smpltrek-re has none yet; requested through the user, who
   passes it to the agent working there); then a loop through a cable and an outside instrument on the unit.
4. (**Done** on the emulator 2026-11-20, see the log; the unit to come.) **Clock, transport and SYNC.** An Enaga tempo clock that apps follow or drive: internal tempo, or MIDI clock
   in (24 PPQN, tempo derived), or SYNC IN (GP7[8] edges, 2 PPQN); START / STOP / CONTINUE, and Song Position
   Pointer sent before CONTINUE, as Lofi-12 XT does (XF-24). SYNC OUT on GP7[9]: 2 PPQN, 15 ms pulses, polarity
   settings as stock (HW-27). Clock source and clock out as settings.
5. (**Done** on the emulator 2026-11-20, see the log; the unit and the USB IDs to come.) **MIDI over USB.** A USB device stack for USB0 (a Mentor MUSB controller, full speed as stock: HW-12), class
   compliant USB MIDI (bulk endpoints, 64 bytes), same messages and routing as DIN. Built so mass storage and
   audio can join it in E5 as a composite device. The stack is a decision at this step: a TinyUSB port (MIT; it
   has a MUSB driver for other TI parts) or StarterWare usblib as the stock firmware uses (LIB-1; licence to
   check). The emulator would need a USB model or a host-side shim; the unit and a computer test it for real.
6. (**Done** 2026-11-20, see the log: the toolkit, then app icons.) **UI toolkit, first cut.** Small: a list menu, a parameter page (name / value rows on the encoders), a
   confirmation dialog and text entry (patch names), drawn with the existing fonts on `Canvas`, driven from the
   app's events. Then app icons in the Enaga menu (an optional `ICON` in the package). File browser, waveform
   view and anything larger wait for real uses.
7. (**Done** 2026-11-20, see the log.) **SDK.** A project template and a build tool that work outside this repository; unaligned-safe load/store
   helpers; the null-read note; the DSP library prebuilt if TI's licence allows (check that cl6x code calls none
   of TI's run-time library). Getting-started docs from an empty folder to an app on the card.
8. (**Simulated** 2026-11-20, see the log; a person still to do it.) **Exit.** Someone who has not worked on Enaga builds the template app from the docs alone.

## E5 plan (2026-11-21)

Scope: the rest of the hardware, so that 1.0 (E6) freezes an API that covers the whole machine and has been used.
Survey of what E4 left uncovered: the battery (read, ignored), USB mass storage and audio, and SYNC IN polarity.
Left out on purpose: the original firmware's sounds in SPI flash. What is there depends on what was installed last
(a stock update, another product's firmware), so apps could not count on it; apps bring their own samples on the
card. Enaga still neither reads nor writes the flash. Steps in order, smallest and safest first; each ends tested on the
emulator where it can be, and the ones that need the unit collect into a session.

1. (**Done** on the emulator 2026-11-21, see the log; the unit to come.) **Battery and power.** Panel channel 15 is the battery voltage and GP3[7] is high on batteries (smpltrek-re
   "Power management", XF-8, HW-24). Enaga turns that into a level (0-4, the stock threshold tables per BATTERY
   TYPE: alkaline, NiMH, lithium, with hysteresis), a BATTERY TYPE setting, a battery mark on the menu and STATUS,
   and `enaga::system::battery()` for apps (level, or -1 on USB or adapter power). At the lowest level: a warning,
   then a clean switch-off that closes the app's files first, so a fading battery never cuts a card write. The
   thresholds are the stock ones; the voltages behind them want a reading on the unit with each battery type.
2. (**Done** on the emulator 2026-11-21, see the log; the unit to come.) **SYNC IN polarity.** The stock POLARITY IN setting (rising or falling edges, HW-27), and POLARITY OUT for
   SYNC OUT's idle level. Small; the unit with a falling-edge device checks it.
3. (**Done** on the emulator 2026-11-21, see the log; the unit to come.) **USB mass storage.** The card as a USB drive, so apps and samples go on without taking the card out. The SD
   card cannot be used by FatFs and by the computer at once. *Decided (2026-11-21):* a USB DRIVE page in the Enaga menu that
   stops the app, unmounts the card and hands it to the computer until it is ejected (or OK is pressed, which waits for the writes to stop), then
   remounts and goes back to the menu; rather than a drive that appears while apps run (files would fail
   underneath them, and a computer writes whenever it likes). Class: bulk-only transport, SCSI transparent,
   on top of the existing SD driver, added to the USB MIDI device as a composite (TinyUSB's MSC class). Testing:
   the emulator's scripted host speaks bulk transfers already; a host-side check reads and writes a FAT image
   through it. The unit with Windows, macOS and Linux checks it for real, including writing a large file.
4. (**Done** on the emulator 2026-11-21, see the log; the unit to come.) **USB audio.** As stock (HW-18, HW-40): class 1.0, 2 in / 2 out, 48 kHz, 16-bit, isochronous (OUT adaptive,
   IN asynchronous, 1 ms). *Decided (2026-11-21):* what apps see: the computer's audio arrives as a new input
   source (`enaga::system::input(ENAGA_INPUT_USB)`), so `audio()` gets it in place of the jacks; the unit's
   output goes to the computer as well as to the jacks, with no app code. A later step can mix both inputs. The
   codec runs from its own crystal, so the two clocks drift: the stock firmware trims the codec PLL from TIMER64P1
   while streaming; Enaga does the same, or resamples by a fraction of a sample (to decide at the step from
   measurements). Testing: the emulator has no isochronous transfers yet (a request through the user for
   smpltrek-re); the unit plays and records with a DAW.
5. (**Done** 2026-11-22, see the log.) **UI toolkit, second part: the pieces every screen needs**. What a large app written without the
   toolkit had to build for itself shows what apps need. In `<enaga/ui.hpp>`, header-only:
   - `Line`, a line of text built in place without printf, never longer than the screen: text, numbers (with
     width and padding), fixed point ("130.0"), hex, MIDI note names, outside text made drawable (lower case
     raised, characters the font lacks shown as `-`), a part added only if it fits, padding to a column.
     `drawable(c)` says whether the font has a character.
   - `Notice`, a message that clears itself after a moment (1.5 s by default), drawn over the bottom line.
   - The screen's frame: `status_line` at the top with a rule under it, `hint_line` for key hints at the
     bottom, and the constants between them (`body_top`, `body_bottom`) for the widgets' `top` and `bottom`.
   - `bar` and `bar_centred` (a value's place in its range), `meter` (a level, upright or across, with a peak
     mark) and `meter_db` (an amplitude on a dB scale, as a meter's fraction).
   - api.md says LED writes go out only when a level changes, so apps need no copy of their own.
   Tested on the host (tests/ui/host.cpp); `examples/widgets` uses them and drops its own number formatting.
6. (**Done** 2026-11-22, see the log.) **UI toolkit, third part: larger widgets.** A map of the pads as they lie
   (two staggered rows of 8 and 7, not a 4 x 4 grid as first written here), and a `Grid` of cells for banks and
   slots, both with labels, a selection and lit states; a file browser on `List` and the file service (folders,
   back up, a file type); a four-column layout and page tabs for `ParamPage`. Each with a host test and a page in
   an example. The 16-step grid stays with apps: every sequencer draws its own.
7. (**Done** 2026-11-22, see the log.) **What the first outside apps ran into.** A developer writing an app on
   0.2.0 listed what they had to work around: 64-bit division failing to link; no `<string.h>`; no number
   formatting (step 5's `Line`); `ParamPage` without a step size or faster turning for wide ranges, and resetting
   its group when values are swapped; characters missing from the font; and the emulator script's fixed card,
   keys-only input, no way to run an app's `stop()`, and an unstated `audio.wav` format. All but the formatting are
   this step.
8. (**Done** 2026-11-22, see the log.) **Enaga's own screens and the synth on the newer work.** The synth draws
   with the toolkit where it fits (the status line, a `List` of patches, bars, a `Notice`, `Line` for its text)
   and takes pressure from Enaga's aftertouch; the menu, SETTINGS and USB DRIVE pages share the toolkit's frame.
9. **Exit.** On a unit: copy an app over USB DRIVE and run it; play audio from a DAW through the unit and record
   the unit into it; the battery mark falls with the batteries and the unit switches
   itself off cleanly.

E6, 1.0: a review of the whole API before freezing it (names, the C interface, what is provisional), a person's
exit test of the SDK, the pid.codes USB IDs, the Lofi-12 XT and CyDrums run by someone with one, and the public
release.

## 1.0 addition: input gain (draft, 2026-11-24)

Something the hardware has that apps cannot reach yet: a draft for the API review, not built.

**Input gain.** The codec's input amplifiers (registers 15 and 16, the left and right ADC PGAs) go from 0 to 59.5
dB in 0.5 dB steps. Enaga uses 0 except HIZ HIGH (+10 dB) and DYNAMIC_MIC HIGH (+21 dB), so an app has three levels
per source and nothing between them. The draft:
- `int enaga::system::input_gain(float db)`: extra gain on top of what the source and level set, 0 to 59.5 dB,
  rounded to 0.5 dB, the total capped at 59.5. Returns 0, -1 out of range, -2 if the codec did not answer.
  `float enaga::system::input_gain()` reads it back. C: `input_gain(half_db)`, an integer 0-119, and its getter.
- **No break in the sound:** unlike `input()`, which silences `in` for about 10 ms, the gain changes live. The
  codec steps its amplifier 0.5 dB a sample (soft-stepping: bits 1-0 of registers 19 and 22, 00 in every route,
  per the data sheet, SLAS509; to be heard on the unit), so a knob can drive it. Main context: one I2C write each side, well under a millisecond.
- The gain stays through `input()` changes of source or level and goes back to 0 when the app stops, as the
  input does. It has no effect on `ENAGA_INPUT_USB`; for that an app scales `in` itself.
- One value for both channels. A pair (`input_gain(left, right)`) is an open question: the stereo line input is
  the only source where the two sides could want different gains.
- Testable on the emulator, which models the codec's registers. On the unit: a quiet source with the gain swept,
  listening for steps or clicks.
- Not in the draft: the codec's automatic gain control (registers 26-31), for recording at a steady level. It could
  follow as `input_agc(target_db)` if an app asks.

## Phases

| Phase | Content | Exit |
|---|---|---|
| **E0** | C++ toolchain; app-side C++ runtime; package format; loader that runs constructors | a C++ app with globals, virtuals and `new` runs from a package (done) |
| **E1** | Kernel: audio callback, display DMA, event loop, exception handling and watchdog, cache/L2 set-up | device load test: measured audio headroom with caches on (done) |
| **E2** | Audio API and DSP library, asynchronous file I/O, sample loading and streaming | one real instrument app (done) |
| **E3** | Enaga menu, boot policy, F1 escape, app switching, settings | switch between two apps repeatedly on the unit (done) |
| **E4** | Input selection, pad pressure, MIDI on DIN and USB, clock and SYNC, a first UI toolkit, SDK packaging and documentation | an outside developer builds the template app from the docs |
| **E5** | Battery and power, SYNC polarity, USB mass storage and audio | apps copied over USB, audio to and from a DAW, a clean low-battery switch-off, on a unit |
| **E6 / 1.0** | API review and freeze, the SDK's exit test by a person, the USB IDs, the other two devices, release | public release |

## Status

E0 and E1 are complete. E1's kernel, audio engine, display DMA, exception handling, watchdog, key debouncing
and cache configuration ran on a unit in the first session (2026-11-18, results below and in docs/hardware.md);
its exit measurement, audio headroom with caches on, is taken: Enaga's own share is about 3 % of the audio
budget, a computed voice costs about 1,870 cycles a block and one reading samples from DDR about 2,600-2,700.

E2 is complete: its exit app, `examples/synth`, runs on a unit and keeps its patches on the card. E2 brought pads
as press / release with velocity, the DSP library (`include/enaga/dsp`, tested on the host), and `examples/synth`, an eight-voice virtual analogue
synthesizer (emulator-tested; on the unit: 8 % idle, 24 % with eight notes). The library's block functions, and apps' own `*.ti.c` files, are built with TI's
cl6x when it is installed (GCC otherwise); the synth's worst case is 30 % of the audio budget that way and 65 % with
GCC alone (every feature on: sync, ring, chorus, eight voices). The file service is in (the
"Files" section), and the synth saves patches with it; SD writes work on a unit (polled, FIFO primed before
the command; hardware.md "SD card"); the CHECK app measured the card on a unit; streams (playback and recording
from `audio()`) and WAV headers are in, with multi-block SD transfers (on a unit: reads 7,600 KiB/s, writes about 2,000).

E3 is complete: the Enaga menu on F1 + POWER (and `open_menu`), F1 at power-on, the boot policy
(last app, only app, or the menu) with a start-up record, switching apps without a reboot with a fade and a 3 s limit
on `stop()`, display names, the SETTINGS page (volume, brightness, pad sensitivity, auto-off, speaker) in
`/ENAGA/SETTINGS.TXT`, and apps' own settings (`<enaga/settings.hpp>`). On the unit (session 3, 2026-11-19) its
exit passed: switching, the menu, the boot policy, volume, brightness, the speaker and headphone detect behave as
expected. Session 4 found AUTO OFF working (session 3 saw it revert; not reproduced) and set the PADS steps
by feel to 1,600 / 1,000 / 600. App icons are left for E4.

Since E3: one image serves the SmplTrek, Lofi-12 XT and CyDrums (the same board), and the build writes an update
file for each; Enaga tells which it runs on from the bootloader's file name, confirmed on a SmplTrek in session 4.
The other two are untested.

E4 is complete apart from two checks, and is released as 0.2.0 (2026-11-21): pad pressure, input selection, MIDI on
the DIN jacks and over USB, the clock with transport and SYNC, the UI toolkit, app icons, and the SDK (an SDK zip, a
prebuilt toolchain and its source with each release). On the unit (session 5) pad pressure, the menu icons, an app
built from the released SDK, and USB MIDI passed; still to check there are DIN MIDI, the clock and SYNC, and three
of the input routes, which need equipment. The exit test passed as a simulation (a fresh agent with only the
downloads); a person doing it moves to E6. USB uses test IDs until pid.codes grants one.

E5 ("E5 plan"): steps 1-8 are merged (battery, SYNC polarity, USB DRIVE, USB audio, the UI toolkit's second and
third parts, an outside developer's list, the synth and menu on the toolkit) and released as 0.3.0 (2026-11-23).
On the unit (session 6, Windows) USB DRIVE and USB audio work after four rounds of fixes: full speed, the SD
remount, single-buffered bulk endpoints, one isochronous IN packet a frame. Still owed for the exit: a recording
under a heavy app, checklist steps 7, 16, 17, 19 and 22, and the drive's write speed (about 155 KiB/s).
Its card is staged by `tools/unit_card.py`.

## Open items

- Input gain ("1.0 addition: input gain"): one value or a left/right pair; build before or at the API review.
- **Unit session 6 (2026-11-22/23, E5 exit, in progress; Windows host): USB DRIVE and USB audio failed on the unit;
  both work since round 4 (below).**
  Build under test: `a24ba51d` (branch `usb-audio-requests`, c9df676), card a 4 GB Toshiba microSDHC in a PNY
  adapter.
  - USB DRIVE: Windows lists the drive but asks to format it, with endless errors until the page is ended. Ending
    it leaves the card unusable until the unit is switched off: `MOUNT 3 -2 00000010`, `SD CYCLED MOUNT 3` (ACMD41
    no response after CMD8 answered). The size is right: `CSD 400E0032 5B590000`, `CSD 1D177F80 0A40008D`,
    `CARD 7626752 SECTORS` (CSD 2.0, C_SIZE 7447). Ruled out: size, MSC offset handling, caches (sd_read copies
    by CPU, no DMA). Untested ideas: what data Windows gets back (misaligned accesses in FIFO code, which the
    emulator may not model as the C674x does; bulk IN on the real MUSB), and what leaves the card unanswering.
  - USB audio: after the request fix (9775a5e) the streams start (`USB AUDIO OUT ON`, `IN ON` / `OFF`), but a
    video player playing to the unit still hangs; Audacity records silence (CHECK plays nothing unless STEP 1-3,
    so that may be right). The `UA O.. I..` STATUS line never appeared: either packets never moved and the
    streams were off when looked at, or a crash record took its row. Suspect: the SOF interrupt reaching the CPU
    through the TI wrapper on the real chip (the emulator gates SOF by the core's INTRUSBE; ENAGA_USB0_SOURCES
    includes USB bits 16-24).
  - Windows session's findings (`~/smpltrek-debug/FINDINGS.md` on the user's machine): over USB the 4,534 sectors
    Windows got match the card reader's byte for byte. The failure is a 128-sector READ(10) cut short: 57,856 or
    58,112 of 65,536 bytes (seven 8 KiB chunks, then 8 or 12 packets of the eighth), then the CSW (passed, residue
    0), after 5 to 35 good reads; Windows waits 20 s, cancels, and the drive reports no medium. Every control
    request answered OK. Isochronous: Windows completes its own URBs at once with USBD_STATUS_INVALID_PARAMETER
    (0x80000300), 10 packets of 192 (OUT) or 196 (IN) bytes, ASAP: nothing reaches the wire. No MIDI device
    appears in Windows at all (USB MIDI was only ever tested through WSL).
  - Fix tried for the drive: `process_epin_isr` wrote the next packet on every TX interrupt without checking the
    FIFO had room; an extra TX interrupt with TXRDY still set overwrote an unsent packet and counted it, so a chunk
    "finished" early. It now skips while TXRDY is set and counts the skips (`USB TX BUSY n` logged when the page
    ends); drive read / write failures log the SD driver's code, command and status (`DRV R -3 C18 0008`). The
    emulator never delivers such an interrupt, so `test_large_reads_as_windows_makes_them` (128-sector reads) is
    coverage, not a reproduction.
  - Round 2 (`FINDINGS2.md`): **Windows ran the unit at high speed** (USB Device Tree Viewer, the xHCI trace's
    DeviceSpeed 2). The core's POWER.HSENAB is on after reset and nothing cleared it (the original firmware writes
    POWER = 0x40; TinyUSB only ORs SOFTCONN in), so the core chirped. At high speed the full-speed descriptors mean
    other things: isochronous bInterval 1 is 125 us, so usbaudio's 10-packet URBs are not whole frames and the
    stack rejects them (USBD_STATUS_INVALID_PARAMETER, never dispatched); bulk at 64 bytes is illegal there (the
    drive's BABBLE). `enaga_usb_phy_start` now clears HSENAB. The drive's photo showed no USB TX BUSY line: the
    TX guard (kept, harmless) was not the cause. Windows groups interfaces 0-3 as one usbaudio function (the AC
    header's baInterfaceNr), so MIDI is usbaudio's, with no MIDI child; worth checking once audio works.
  - **The remount failure was the SD driver**: `sd_init` kept the RCA from the last start-up and sent it with
    CMD55 before ACMD41; a card just reset answers CMD55 only for RCA 0, hence `MOUNT 3 -2 00000010` after every
    USB DRIVE and the power-cycle retry. It now starts from RCA 0; `test_ok_ends_it` checks the CMD55 arguments in
    the emulator's SD trace and fails on the old code.
  - The emulator now matches the hardware on both (done by its agent from our brief, 2026-11-23): POWER resets
    to 0x20 (HSENAB on), each bus reset picks the speed from HSENAB and sets HSMODE, and `usb_state()` reports
    `speed` and `high_speed_resets` (its host still sends only full-speed traffic); the card answers CMD55 only
    for RCA 0 before CMD3 and only for its own RCA after. With the two firmware fixes reverted, `test_usb` and
    five `test_usb_drive` tests fail; with them, all pass. `test_usb` also checks HSMODE and `high_speed_resets`.
    Not asked for yet: high-speed rules in the host (512-byte bulk, bInterval in microframes, device qualifier),
    needed only if Enaga moves to high speed on purpose.
  - Round 3 (`FINDINGS3.md`, build `ea4d223f`, 2026-11-23): the unit enumerates at Full-Speed, playback works at
    the USB level (EP 0x03 OUT, all URBs succeed), apps survive USB DRIVE. Still failing:
    - **USB DRIVE never mounts**: the drive sends INQUIRY's 36 bytes and then never its CSW (from plug-in; earlier,
      a CSW then no sense data). Cause, from SPRUH79C 33.2.7.1.2.1.2: with double packet buffering TXPKTRDY clears
      and the interrupt comes when a packet is *loaded*; when it is later sent there is no interrupt. TinyUSB's
      MUSB port waits for FIFONOTEMPTY to clear before completing an IN transfer, so the completion never came
      (at high speed the packet had usually gone before the ISR looked). Bulk endpoints are now single-buffered.
      The emulator raises an interrupt as each packet leaves, so it could not show this (relay below).
    - **Recording babbles**: 97.7 % of EP 0x83 IN packets end in BABBLE_DETECTED, two good packets every 134
      frames, though the samples in the good ones are right and continuous (the packets carry 48 frames each).
      Not explained. Diagnostics: STATUS LEFT / RIGHT choose `usb_audio_in_mode` (0 double-buffered with both halves
      loaded and ISOUPDATE, as before; 1 one packet a frame with ISOUPDATE; 2 the same without; 3 single-buffered
      without), shown as `M` on the UA line and taken when a stream starts; the stream's end logs
      `IN M<mode> L<underruns> U<ring gaps>`. UNDERRUN is now cleared without writing TXPKTRDY back (the
      read-modify-write committed a packet whenever both halves were full).
    - For the emulator (via the user): with double packet buffering, a TX endpoint interrupts when TXPKTRDY
      clears (at the load, if the other half is free), not again when the packet is sent.
  - Round 4 (build `9a4d6133`, 2026-11-23): **USB DRIVE works**: a 102 MB file copied to it and back is identical,
    the drive mounts, ejects and keeps the apps. Writes ran at about 155 KiB/s (11 minutes), about the unit's
    single-sector write speed, though the drive writes 16 sectors a command; the limit is not yet known (see the
    next item). **USB audio works** in IN modes 1-3 (`L0 U0`) and fails in mode 0 (`U4006`: the ring emptied twice
    as fast, so with two packets pending the core sent more than one a frame, which is the babble). Playback no
    longer hangs the player; the meters follow it. Now fixed at one packet a frame, double-buffered, with
    ISOUPDATE; the selector is gone, and the end of a recording logs `USB AUDIO IN L<underruns> U<ring gaps>`.
    One packet a frame needs the SOF handled within a frame, which an app callback longer than 1 ms prevented
    (`test_a_heavy_callback_does_not_cost_packets`: a quarter of the packets empty, samples lost), so the USB
    interrupt now nests in the app's callback, as the watchdog, UARTs and SYNC IN already did.
- USB DRIVE speed: about 155 KiB/s writing on the unit (round 4). With the SD card's multi-block writes at about
  2,000 KiB/s, the time goes on USB: about 380 us a 64-byte packet, single-buffered (a NAK while the CPU empties
  the FIFO; how soon the computer retries is the computer's). To measure before changing anything: the read
  speed, and whether double buffering the OUT endpoint helps (the TRM is not clear that a second packet waiting
  raises its own interrupt, the trap the IN side fell into).
  - Earlier plan: the user started a Claude session on the Windows PC with a read-only brief: raw sectors 0-63 and
    the partition start over USB vs a card reader, a USBPcap capture of drive mount / playback / recording, and
    scripted play / record tests. Debug from its evidence. Also ask whether STATUS showed a crash row.

- USB IDs: the device uses TinyUSB's test vendor and product IDs (0xCAFE / 0x4011, `firmware/usb/usb.c`) until
  the pid.codes product ID (vendor 0x1209) is granted. The request is in, and its timing is pid.codes'; releases
  ship with the test IDs until then (0.2.0 did, and its notes say so).

- USB audio: clock drift is handled by resampling (OUT) and packet sizes (IN), not the stock PLL trim; the
  emulator's clocks agree exactly, so drift was only simulated (packets of 47 / 49 frames). Whether a real
  computer's rate against the codec's crystal stays inside the 2000 ppm the resampler allows, and whether it
  clicks, is for the unit (checklist step 21). An app whose callback holds off the USB interrupt for more than
  about 2 ms would still cost packets.
- USB DRIVE: the card's size from its CSD is checked only on the emulator's SDSC card (CSD 1.0); an SDHC or SDXC
  card (CSD 2.0) is first read on the unit. Whether each computer's eject sends START STOP UNIT (which closes the
  page by itself) is to see on Windows, macOS and Linux; OK ends it either way, after 2 s with no writes. That
  covers a cache flushed within a second or so of the copy (macOS; Linux desktops mount removable FAT drives with
  `flush`); a computer that holds writes longer would still lose them, so ejecting stays the safe way. Speed over full-speed USB is
  unmeasured (checklist step 20).
- SYNC polarity: POL IN RISE arms the pin's falling edge, as the stock RISE does, on the reading that the board
  inverts SYNC IN; nothing has shown that on a unit. Only the phase depends on it (by the other device's pulse
  width), not the tempo. A scope on the jack settles it (checklist step 17).
- Battery: the raw thresholds are the original firmware's; their voltages, and whether 30 s at level 0 is a
  sensible time before switching off, want readings on the unit with each battery type (checklist step 19).
- STEP 4 crash (unit session 5): pressing STEP 4 on CHECK's MIDI page once crashed, with USB connected to a
  Windows PC; not repeated on the unit, nor on the emulator (60 s of STEP 4's clock with a host that never reads,
  then 40 random STEP 1-5 presses). The reason was not noted. If it recurs, photograph STATUS before switching off.
- Input routes HI-Z, LINE ST and LINE MONO are unconfirmed on the unit: session 5's source was too quiet (only DYN
  MIC at HIGH moved, to 2-3 per mille). Retest with a line-level signal.
- Lofi-12 XT and CyDrums: the build makes their update files (docs/hardware.md "Installing and recovery"), but
  Enaga has never been installed on either. A first session on one would follow "Checks on a unit" steps 1-11,
  ready to restore it with its official update.
- AUTO OFF: session 3 saw 5 MIN revert to OFF; session 4 could not reproduce it (kept, saved, and switched the
  unit off after 5 minutes), with no change to the settings code between the two images but the save log lines.
  If it comes back, note the status page's `SETTINGS SAVED` / `NOT SAVED` line and `/ENAGA/SETTINGS.TXT`.
- Whether AUTO OFF should apply on USB power too (it does now; `power_on_batteries` could limit it).
- On the unit: power-key bounce; bounce of keys other than STEP 1 (STEP 1 settles within 784 µs,
  smpltrek-re session M1, well inside the 6 ms sampling and two-sample release).
- LED map: 53 addresses known only from the code, 24 seen on another channel of their LED (docs/hardware.md
  "LED map"; smpltrek-re session L1); the level-to-brightness steps. Left/right on the line outputs (headphones confirmed in M1, the inputs in session 1).
- The SDK should give apps unaligned-safe load/store helpers for packed formats (misaligned LDW/STW/LDDW round
  the address down without a fault) and say that a null read returns garbage rather than trapping (smpltrek-re
  session M1; docs/hardware.md "Processor and memory"). A debug-build null check is worth considering.
- Pad velocity (session 1 found none): now the higher of the first two reports over the PADS threshold, 127 at 3,300 (light
  press about 1,500, hard about 3,400 on the unit). To check by playing on a unit; recorded press curves would
  show whether two reports catch the peak of a hard, fast hit.
- Pad crosstalk (session 1): pressing a pad raises some other pad's reading slightly (0 -> 4, 1 -> 5, 4 -> 2; 2 and 3
  none), never to a press in the synth. Size not measured.
- Pad events: Enaga passes every MCU message on, about 3,930 raw pad events a second at rest, and press / release
  with velocity (press at the PADS threshold, 1,000 at MEDIUM, release under 3/4 of it, velocity as above). Still open: the release threshold, per-pad
  calibration.
- Synth on the unit: measured 8 % idle, 10 % one note, 24 % eight notes (INIT), 25 % SUPERSAW two notes (eight
  voices), no dropouts; the worst case (30 % on the emulator) not yet measured there.
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

## Development log

Entries name the SmplTrek reverse-engineering project, now published as [smpltrek-re](https://github.com/seawaffle/smpltrek-re), as "smpltrek-re".
Its commits and working-tree states cited here (bcce9b3, cdd968f, "not yet committed") are from its history
before publication, which the public repository does not contain.

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

- `hal/keys.c` `keys_poll` follows the original firmware's timing (smpltrek-re claim HW-51, "Key timing and
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

- The LED map from the smpltrek-re notes (`docs/04-hardware-access.md` "Panel LEDs", `analysis/led_table.py`), restated in
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
  with a test (smpltrek-re working tree, not yet committed). The C++20 header needed `int(ENAGA_LED_KEY)`, since
  arithmetic between two enum types is deprecated.

### E1 cache configuration (2026-11-15)

- Enaga had relied on the bootloader having configured the caches. smpltrek-re's trace and simulation of BOOT 1.0.17
  (docs/hardware.md "Boot") show otherwise: it leaves L2 all RAM, L1D half cache, MAR 196-198 (48 MiB of the app
  heap) uncached, and does no cache maintenance before branching. The original firmware's configuration comes
  from its own SYS/BIOS start-up.
- The first instructions of `start.s` record the cache registers (into `.data`, since bss is cleared later), write
  back and invalidate L1D and invalidate L1P, so nothing cached from before the jump is relied on. The image
  itself is in DDR at entry: smpltrek-re's code reading shows the bootloader reads flash by EDMA, behind the caches,
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

Two bugs reported from smpltrek-re's bring-up firmware (commit cdd968f) had the same pattern in Enaga.
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
  The smpltrek-re note about reading EDMA registers before its power module is on does not apply: the interrupt
  is enabled only after audio set-up.

### Session M1 results (2026-11-16)

smpltrek-re's bench session M1 measured on the unit what Enaga had assumed (docs/hardware.md now marks each
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
false (as BNOP's do). The emulator (smpltrek-re bcce9b3, `core/core.cpp`) drops them for a false predicate, so with
both operands non-negative divi falls into the next packet's `b b5` and returns twice; the caller's loop then
runs with two branches in flight. smpltrek-re's emulator now has the two-line fix (in its working tree after
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

From smpltrek-re's emulator work: GCC 13.3 for tic6x divides floats by a reciprocal estimate (RCPSP, refined
twice), not exactly, so its quotients can be a unit in the last place off the host's, and cl6x's exact
`__c6xabi_divf`; the estimate's bits are not yet checked on hardware either. `-fno-reciprocal-math` would make
GCC exact at a cost. The library's C code (built by either compiler) no longer divides: the biquad design, the
one-pole coefficient and the reverb's gains use `enaga_reciprocal`, the effects' rate scale a constant
reciprocal, so cl6x and GCC builds give the same coefficients by construction and the cl6x library calls no
division helper (only `__c6xabi_fixfu`, an exact conversion). Apps' own C++ still divides the GCC way, the same in
both builds. The same note: `_smpyh`, `_smpyhl` and `_smpylh` from GCC's `c6x_intrinsics.h` emit plain SMPY
(wrong halves); Enaga uses none, and should write half selection in C, `(x >> 16) * (y >> 16)`. And the emulator
gains about 30 instructions GCC can emit (MPYH and family, saturating arithmetic, the packed ADD2 / SUB2 / ADD4 /
MAX2 / SWAP4) in smpltrek-re's working tree, not yet committed: rebuild the emulator and rerun the tests after that
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
does. The bootloader leaves it on (smpltrek-re's own firmware, which never enables it, drove the power hold and
read keys on a unit in session M1), so on a unit nothing changes; it lets Enaga start from the power-on state the
emulator's panel (`stock_ui.py`) uses, so the panel runs Enaga with no wrapper:
`stock_ui.py --firmware build/firmware/ENAGA_0_1_0.bin --card <card with /ENAGA/APPS/...>`. It runs at about
0.5-0.6x real time on the development host, so its sound breaks up.

The smpltrek-re emulator work found that tic6x GCC 13.3 at -O2/-O3/-Os can raise SP (release the frame) a few
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
them to a fresh 64 MiB card image and starts the smpltrek-re emulator's web panel (`stock_ui.py`) on Enaga's update
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

### Pad velocity from two reports (2026-11-18)

Session 1 found no velocity response: velocity was the first report over the 800 threshold, and a pad's reading
rises over several 3.8 ms reports, so that report was nearly always just over 800 (velocity 1-10); full scale was
also 12,000 where a hard press reads about 3,400. Now the press is sent at the pad's next report, with velocity
from the higher of the two, 1 + 126 (peak - 800) / 2,500, 127 from 3,300: at most 3.8 ms later. A tap released by
that next report still gives its press, then its release. Tests send each press as two reports, as the MCU's
stream does. 36 tests, dry run 141/141. The synth's VELO (VOICE page, 50 % by default) sets how much velocity
changes its volume.

### The file service, step 1 (2026-11-18)

As designed in "Files": `firmware/kernel/files.c`, the app's side in `abi.h` (14 host functions, error codes,
`ENAGA_EVENT_CARD`) and `<enaga/files.hpp>` (free functions, a `File` that closes itself, callbacks as member
functions, `App::on_card`).

- FatFs: long names with UTF-8 paths (`FF_LFN_UNICODE 2`), exFAT, `FF_FS_LOCK 12` (a file open for writing cannot
  be opened again or replaced). `firmware/hal/ff_unicode.c` replaces FatFs's ffunicode.c: code page 437 from
  Python's codec, upper-casing for ASCII, Latin-1, Greek and Cyrillic. exFAT's 64-bit division brought libgcc's
  unwinder into the link through its unwind tables; their personality routines are stubbed (`abi_stubs.c`).
  The firmware grew from 60 KiB to 95 KiB.
- The queue runs in the main loop after `update` (crash-screen stage 8), 2 ms a pass, 8 KiB per FatFs call; the
  loop does not idle while requests remain. A 1 MiB load spans about 40 passes on the emulator.
- When the app stops, its handles are closed after `stop()` (which may still save) and its requests dropped
  without callbacks. When it is dropped (possibly from an interrupt, possibly mid-FatFs-call), the next pass
  discards everything and remounts.
- `tools/card.py` (with `tools/cardtool.c`) builds FAT and exFAT images and reads files back through Enaga's own
  FatFs; the firmware starts apps from both.
- `tests/test_files.py` runs `tests/apps/files` four ways: a FAT card with a removal and reinsertion on the
  card-detect pin; exFAT; a nearly full card (writes report FULL, removing the partial file frees room); an app
  dropped with a load pending (no callback afterwards, card intact). About 60 checks a run: safe saves replacing
  a file without leaving .TMP, the .TMP fallback, the write areas and refused paths, eight open files and a
  ninth refused, case-insensitive names, a close cancelling queued reads, calls from audio() refused.
- Not covered on the emulator: a card that stops answering mid-transfer (the emulator only drives the
  card-detect pin), and the SD driver's real speed. 40 tests pass; the unit dry run passes 141/141.

### Synth patches on the card, file service step 2 (2026-11-18)

The synth's PATCH page (F1 + F2) lists the twelve factory patches, then sixteen user slots, U1-U16. OK loads the
chosen entry; F1 + OK saves the sound into a user slot (on a factory patch it says USER SLOTS ONLY), under the
name of the patch it was edited from, so U3 shows "U3 BRASS". The sound when the app stops, switching off
included, goes to LAST.SYN with the patch it came from, and the next start restores both (knob pushes still go
back to that patch's values). Loading a patch now also silences held notes, since the voice mode may change.

- Files in `/ENAGA/DATA/SYNTH/`: "USER 01.SYN" to "USER 16.SYN" and LAST.SYN, written with `files::save` and read
  with `files::read_all` (the blocking calls: a few hundred bytes each). The format is in `patches.hpp`: a
  26-byte header (magic, version, value sets, parameter count, list entry, name) and int16 values by parameter
  number. Parameters keep their numbers, so an older file with fewer loads with INIT for the rest; values are
  clamped to their ranges. The slots are scanned at start and on each card event.
- `test_synth.py` saves into U1, loads an older ten-parameter file from U2, BRASS and U1 again, switches off by
  the power key and checks the card's files byte for byte, then boots from that card and finds the sound and
  its patch restored. The card is made by `tools/card.py`, since the names are long. 41 tests pass.
- Not done: naming a patch on the unit (there is no text entry yet; E4's UI toolkit), a confirmation before
  saving over a full slot, deleting a slot.

### Card speed page, file service step 3 (2026-11-18)

CHECK's fourth page, CARD: STEP 1 writes a 4 MiB file to `/RECORD/CHECK.BIN` in 32 KiB requests (four queued),
reads it back the same way, then makes 256 reads of 4 KiB at random offsets (seek, read, one at a time), and
removes the file. It shows KiB/s for each, scattered reads a second, and the 48 kHz stereo streams READ's rate
would feed; results go to the log too. It measures what an app gets through the queue (2 ms a pass in the main
loop), not the bare driver. On the emulator, which models no card timing, it runs to the end (about 8-9 MiB/s;
meaningless for the unit). Step 11 of docs/hardware.md's checklist.

- **Bug found and fixed:** `sd_read` and `sd_write` take only word-aligned buffers, and FatFs transfers whole
  sectors straight in the caller's buffer, so any read or write whose whole sectors fell at an unaligned address
  (a read from offset 314, a save from an odd pointer) failed with CARD ERROR. Step 1's tests read from offset 0
  into `new` memory, so they never hit it. `hal/diskio.c` now passes such sectors through an aligned 512-byte
  buffer, one at a time (aligned transfers are unchanged). `tests/apps/files` checks a save from an odd pointer
  and a read into one (`unaligned`; it fails with -7 without the fix).
- Not in the unit dry run: its staged card is 4 MiB, too small for the test file. 42 tests pass; the dry run
  passes 141/141.

### Unit session 2: the card test stopped CHECK (2026-11-18)

STEP 1 on CHECK's CARD page dropped the app as APP NOT RESPONDING (the main-loop watchdog, 1 s), before any
result: the first time Enaga wrote to a real card. The SD driver writes one sector per command and waits for each to
program (CMD13 until tran, up to 1 s), and a FatFs call writes up to 16 sectors, so a card slow to program, or
one never reported back in tran, holds the main loop past the watchdog; the watchdog blames the app, though the
time is Enaga's. On the emulator a sector programs at once.

- The driver now calls `sd_wait_hook` while it waits, and the file service sets it to `watchdog_feed`: every
  wait stays bounded by the driver's own time-outs, so a slow card gives a slow pass and a failing one an error.
- The driver keeps its last read / write failure (`sd_io_error`, and MMCST0 or, for -6, the card status) and the
  slowest one-sector write; the file service logs `SD ERR <code> <word>` on a disk error and `SD SLOW <n> MS` for
  each new slowest write over 50 ms.
- CHECK stops itself (`fatal`) when the card test fails, so the status page shows those lines.
  `test_check.py` covers that path with a card too small for the test file. 43 tests; dry run 141/141.
- Which it was (slow programming or a status the driver misreads) is for the rerun to show. If writes are slow,
  multi-block writes (CMD25), which cards are built for, are the next step for the driver.

Second try (same day): CHECK stopped with CARD TEST FAILED, `CARD CARD ERROR` and no `SD ERR` line, so FatFs
failed with something other than a disk error (an inconsistent FAT gives FR_INT_ERR); after it every boot showed
CARD ERROR, while a PC still reads the card. The write path had never run on hardware, and how it orders the
data and the command is in doubt: TI's CPU write sequence, as read here, fills the FIFO before the command; the
driver sends the command first, and the emulator models the latter (a write to the FIFO before the command
faults there once DCLR has raised DXRDY). Neither is confirmed, so the next build measures instead of guessing:

- `sd_write_mode` chooses the sequence (0, the old one, stays the default; 1 fills the first 64 bytes before
  the command, then waits for MMCST1 FIFOEMP before each further 64).
- With `/ENAGA/SDTEST.BIN` on the card (`tools/unit_card.py` stages 4 KiB of zeros), each boot writes a pattern
  into the file's first sector by sequence 0 and its second by sequence 1, reads both back, and logs `SDT <mode>
  OK <ms>`, or `BAD <word> <value>` and `SHIFT <words>`. Only that file's data is written, never the FAT or a
  folder. `test_files.py` checks it on the emulator (both OK, the patterns in place, the rest untouched).
- A failed mount logs `MOUNT <FRESULT> <sd_init code> <MMCST0>`, and every file failure reported as CARD
  ERROR logs `FS ERR <FRESULT>` (with `SD ERR` for a disk error). 44 tests; dry run 141/141.

Third try: STEP 1 failed with `FS ERR 1` (disk error) and `SD ERR -2 0 00000010`: a read or write command got no
response at all (MMCST0 TOUTRS). The remount after CHECK stopped failed the same way (`MOUNT 3 -2 00000010`:
sd_init's ACMD41 step, after CMD8 had answered). So partway through the writes the card stops answering
commands and stays that way across switching the unit off (the boots after the second try), until it is taken
out (a PC then reads it, and Enaga mounts it again). The stock firmware runs the card at the same 23.25 MHz with
the same open-drain command line, but writes by EDMA with multi-block CMD25 and CMD12; Enaga writes one sector
per CMD24 from the CPU. GP4[1], which the stock drives low, is of unknown purpose (a card power switch would
explain why switching off does not clear it); not touched without the owner.

- The status page gains an SD line: sectors read and written since start-up (`sd_reads`, `sd_writes`), and on a
  failure its code, the command in flight (`sd_io_command`) and MMCST0's low byte. The log shows five lines.
- With the owner's agreement, GP4[1] is tried as the card's power switch: when a mount fails because the card
  does not answer (FR_NOT_READY or a disk error), `sd_power_cycle` holds the controller in reset with its clock
  off, drives GP4[1] high for 250 ms and low again, waits 50 ms, and the mount is tried once more; the log says
  `SD CYCLED OK 0` or `SD CYCLED MOUNT <FRESULT>`. This covers start-up, the remount after an app is dropped, and
  a card put back in. Not exercised on the emulator (its card always answers).

Fourth try, the probe on a freshly formatted card: `SD R7 W0`, `SDT 0 ERR -2 -2` and `SDT 1 ERR -2 -2`, last
failure `E-2 C17 0010`. Sequence 0's first CMD24 got no response, and after it the card answered nothing (the
read-back's CMD17 timed out too). So it is the first write command, not an accumulation of writes, that stops the
card. Sequence 1 ran second, on a card already silent, so its result says nothing.

- The probe now runs sequence 1 first, and after a failed write tries to revive the card: `sd_init` alone, then,
  if that fails, the GP4[1] power cycle and `sd_init` again. One line per sequence, at most 21 characters (the
  status page's width; the old lines ran off it): `T1 OK 3MS`, `T1 BAD 5 1234ABCD` (+ `T1 SH -16`), `T1 ERR-2
  0010 R-2 P0`. FatFs is remounted afterwards.
- A failed write resets the controller's command and data logic, so it is not left mid-transfer.
- Sequence 1 is now the default for every write (sequence 0 is known to stop the card); all 44 tests pass with
  it, the synth's patches and the CHECK card test included.

Fifth try: `T1 ERR-4 0210`: sequence 1's CMD24 was answered and all 512 bytes left the FIFO (each FIFOEMP wait
succeeded), but DATDNE (the card's CRC status and programming) did not come within 500 ms (0x0010 is the driver's
own time-out flag, 0x0200 DXRDY). Neither `sd_init` alone nor the GP4[1] cycle revived the card afterwards (`R-2
P-2`): GP4[1] is not the card's power, and a failed write still leaves the card silent until it is taken out.

- The probe is now a bus-clock ladder: one sector each at 1.5, 5.8 and 23 MHz (CLKRT 61, 15, 3), read back,
  stopping at the first failure, so one boot (one stuck card) answers whether the CPU keeping the FIFO fed is the
  problem (the stock writes by EDMA). Lines `W1 OK 3MS` ... `W3 ERR-4 0210 S 21` (MMCST1 at the failure).
- The write's DATDNE wait is 2 s (the SD limit for programming is 250-500 ms).

Sixth try, the clock ladder: `W1 ERR-4 0210 S 26` at 1.5 MHz, so not a matter of feeding the FIFO fast enough.
MMCST1 0x26 is FIFOEMP | DXEMP | CLKSTP without BUSY: the controller had stopped the card's clock with its FIFO
empty, which it does during a write when it still wants data. So it never got all 512 bytes, though 128 words
were written to MMCDXR. The likeliest loss is the 64 bytes put in the FIFO before the command, if the command's
DCLR empties the FIFO on the chip (the emulator only sets DXRDY for it); and if the controller holds a write
command until the FIFO has data, sequence 0's unanswered CMD24 is explained too.

- Write sequences 2 (FIFO first, command without DCLR) and 3 (command with DCLR, then the first 64 bytes at once)
  join 0 and 1 (`sd_write_mode`).
- The probe writes sequences 1, 2 and 3 into SDTEST.BIN's first three sectors. While it waits for DATDNE, a
  controller stalled wanting data (CLKSTP | FIFOEMP, not BUSY, for 20 ms) gets 64 bytes of filler, up to 8 times
  (`sd_write_topup`), which should also complete the transfer rather than leave the card stuck; the read-back
  then shows what the card received. Lines: `W1 OK T0 3MS`, `W1 BAD SH-16 T1`, `W1 ERR-4 S26 T8`. The filler
  path does not run on the emulator (its controller never stalls).
- Test runs met the emulator core being rebuilt mid-edit (`C++ core build failed`); after it settled, 44 tests
  pass; dry run 141/141.

Seventh try, the filler: `W1 ERR-4 S10 T1`. One filler went in, then the FIFO held it (MMCST1 0x10: not empty,
clock not stopped) and nothing moved for 2 s, with no error bit and no DATDNE. A controller still wanting data
would have taken it, so 0x26 last time was the controller idle, having already left the transfer; the lost-DCLR
theory does not hold for sequence 1. Sequences 2 and 3 never ran: the probe stops at its first failure.

- Sequence 4 is Linux's davinci_mmc PIO write: 64 bytes in the FIFO, the command without DCLR, then 64 bytes at
  each DXRDY (sequence 2 waits for FIFOEMP instead).
- The probe now runs 4, 2, 3, the likeliest first, and lines are named by sequence (`W4 OK T0 3MS`). 44 tests pass.

Eighth try: `W4 OK T0 3MS`, `W2 OK T0 1MS`, `W3 ERR-2 S26 T0`, status `SD R12 W2`. **Writes work on a unit.**
Both FIFO-first sequences without DCLR read back exact; sequence 3 (command first) got no response to CMD24,
like sequence 0, so the controller holds a write command until its FIFO has data.

- `sd_write` is now sequence 4 only (Linux's); the other sequences, `sd_write_mode` and the filler are gone.
- The probe writes SDTEST.BIN's first sector once per boot and reads it back (`W OK 3MS`), as a check per card.
- hardware.md "SD card" records the write rule. Next on the unit: the CARD page (step 11) and synth patch saving
  (step 12). 44 tests pass; dry run 141/141.

Ninth try, the CARD page twice on a Toshiba microSD (about 3.6 GB, FAT32) in a PNY adapter: WRITE 173 / 170 KiB/s,
READ 2856 / 2878 KiB/s, SCATTER 1787 / 1793 KiB/s (446 / 448 reads/s), STREAMS 15. No failures. Synth patch
saving works: U1 saved and came back after switching off and on. Writes are about 3 ms a sector, one CMD24 each
with its programming wait, so recording needs multi-block writes (CMD25) before it streams; reading is enough for
15 streams as it is.

Synth on the unit: one note V1 about 11 %, eight notes V8 about 24 %, SUPERSAW two-note chord V8 about 25 %.
SUPERSAW (four unison voices a note) holds only two notes and steals the oldest; a little crackle on pad presses
there. Cause, from the code: a stolen voice still sounding (held, or in SUPERSAW's long release) restarts with
reset oscillator phases and filters at its current level, a step in the output. Velocity is noticeable on the bells
(VELO 80) but not usually: most patches keep the default VELO 50, a 6 dB span from softest to hardest.

- The click fixed: a voice taken while still sounding keeps its oscillator phases and filter state (the envelope
  already went on from its level, and the output gains are ramped). A test holds eight sines and takes one with a
  ninth note: the largest sample-to-sample step was 6.7 times the sines' own before, and stays under twice now.
  The synth is a demo for app writers, so voice count and velocity stay as they are. 45 tests pass.

### Streams, WAV and multi-block SD transfers, file service step 4 (2026-11-18)

- `hal/mmcsd.c` moves up to 128 sectors a command: CMD18 and CMD25, each ended by CMD12 (a failed multi-block
  transfer is stopped too). Writes keep the unit's rule (FIFO primed, no DCLR). The emulator's card speed hardly
  changes (it models no per-command programming time); the unit's single-sector writes were 170 KiB/s.
- The boot probe writes SDTEST.BIN's first sector (CMD24) and then sectors 1-7 in one CMD25, reading each back:
  `W1 OK 3MS`, `W7 OK 4MS` (or `W7 NOT IN A ROW` when the file's sectors are not consecutive).
- Streams (abi.h, `enaga::files::Stream`; "Streaming" above) and `<enaga/wav.hpp>`.
- `tests/test_streams.py`: four 64 KiB-ring playback streams from one WAV file (after a LIST chunk) and one from a
  short file played to its end, every word checked in `audio()`; a recording from `audio()` for 1.5 s closed
  with its WAV header and read back with Python's `wave`; no underruns, no audio overruns. With the stream
  service switched off the streams never fill and the test fails. 46 tests pass.
- Not tested: a card removed while streaming (the path is the handles' one), underruns on a slow card.

On the unit (same Toshiba 3.6 GB FAT32 card): probe `W1 OK 3MS`, `W7 OK 6MS`, so CMD25 works. The CARD page, three
runs: WRITE 2382 / 2367 / 1837 KiB/s (was 170), READ 7613 / 7585 / 7585 KiB/s (was 2,860), SCATTER 2085 / 2077 /
2115 KiB/s (521 / 519 / 528 reads/s; was 447). That is room for about 39 playback streams of 16-bit stereo at
48 kHz by reading, or about 9 recordings at the slowest write run, on this card.

### The Enaga menu and app switching, E3 step 1 (2026-11-18)

- `kernel/menu.c`: the app list and STATUS ("Boot and app switching" has the behaviour). `main.c` routes every
  input through `deliver`, which gives it to the menu while open, and otherwise to the app, keeping a record of
  the keys and pads the app saw pressed: opening the menu sends their releases, and a release the app never saw
  pressed is not sent. F1 + POWER opens and closes the menu on the POWER press; holding POWER after the chord does
  not switch off.
- `apps.c`: `apps_scan`, `apps_start(folder)` (stops the app running, then loads the new one into the same window:
  its memory is simply overwritten), `apps_boot` (decision 2 and the start-up record `/ENAGA/LAST.TXT`) and
  `apps_service` (marks the record RUNNING after 2 s). `apps_load_first` is gone.
- A bug found on the way: after an app hung while handling a key press (here, OK choosing an app that hangs in
  `init`), the debouncer still had the key down when the kernel loop restarted, so the press was delivered again
  (opening the menu over the crash screen). The loop now resets the key state after a drop.
- `tests/test_menu.py`: TONE and CXXCHECK switched ten times from the menu; STEP 1 pressed in the menu does not
  reach the paused TONE, and does once it resumes; the record reads `TONE RUNNING` and the next boot starts TONE;
  F1 held at power-on opens the menu; a record of a removed app is ignored. An app that hangs in `init` (HANG)
  leaves the status page with APP NOT RESPONDING, OK opens the menu, and the next two boots open the menu with
  `HANG DID NOT START`. 48 tests pass; dry run 141/141.

### E3 completed on the emulator: fade, stop() limit, open_menu, names, settings (2026-11-19)

- `audio.c`: the output is scaled by `audio_volume` and a fade, ramped across each block by at most 1/8 a block;
  at full level the samples are not touched. `audio_set_callback` starts a new app from silence; `audio_fade_out`
  ramps down and waits (50 ms at most).
- `apps.c`: `apps_unload` fades, then runs `stop()` with `app_stopping` set; `watchdog.c` drops an app whose
  `stop()` has run 3 s (`APP STOP TOO LONG`) even when the main loop is being fed. `apps_start` keeps the folder it
  is switching to in `apps_next`, so after such a drop `apps_service` finishes the switch; `main.c`'s
  `powering_off` does the same for a power-off. Host table: `open_menu` (a flag the main loop acts on after the
  app's callback) and `set_busy` (holds off AUTO OFF; cleared when the app stops). C++:
  `enaga::system::open_menu()` and `set_busy(bool)`.
- `menu.c`: display names from each package header (`apps_title`); the lines are now apps, SETTINGS, STATUS.
  `settings.c`: the settings table, `/ENAGA/SETTINGS.TXT` and the page ("Settings"). `spi_oled.c`: `oled_contrast`,
  sent after any frame in flight. The pad thresholds are variables (`pad_press_level`, `pad_release_level`).
- `<enaga/settings.hpp>`: `enaga::Settings` with `load`, `get` / `text`, `set` (number or text) and `save` (only if
  changed), up to 32 keys.
- `tests/apps/menu` and a third test in `tests/test_menu.py`: names (MENU-TEST in folder MENUAPP), `open_menu` from
  STEP 1, `set_busy` holding off AUTO OFF, a `stop()` that saves forever dropped after 3 s with the switch to TONE
  completed, no step in the output across the switch (the test fails with the fade removed: a jump of 0.25 full
  scale), the SETTINGS page changing all four settings and writing the file, the app's own `SETTINGS.TXT`, the
  settings read back at the next boot, and AUTO OFF switching off with no app busy.

### The speaker (2026-11-19)

Enaga had kept the speaker muted since bring-up (GP2[8] high), which the owner noticed on the unit. Now
`audio_speaker_apply` releases it when the SPEAKER setting is ON and the app has not muted it, with a fade around
the change; power-off mutes it first. Host table: `speaker(on)` (reset when the app stops, or is dropped) and
`jack()` (GP3[8] as read, provisional); C++ `enaga::system::speaker(bool)` and `jack()`. The status page shows `J`
and the jack reading. `tests/test_menu.py` reads GP2[8] back from the GPIO registers: released by default, set by
the app's mute and by SPEAKER OFF, released when the muting app is dropped, and SPEAKER OFF kept across a reboot.

### Unit session 3: E3 on the unit (2026-11-19)

Image ba465148. Switching, the menu, F1 at power-on, the fade, volume, brightness and SPEAKER worked as expected;
the speaker plays and headphones cut it. Headphone detect: GP3[8] reads 0 with nothing plugged in, 1 with
headphones, 0 with a plug in any other jack (inputs or outputs), so `jack()` became `headphones()` (host table
`headphones`, status page `H`). PADS made only a small difference. AUTO OFF set to 5 MIN read OFF again after
leaving the SETTINGS page and after a restart, while the other settings were kept; the emulator does not show
it (open items).

### Update files for the Lofi-12 XT and CyDrums (2026-11-19)

The three devices are one board, so one firmware image serves all of them; only the update container differs
(family word, two CRC seeds, the file name on the card). `tools/sysimage.py` gained a device table (values from
smpltrek-re's `tools/palette_swap.py`), and `firmware/build.py` writes `build/firmware/<device>/<card name>` for
each, plus `ENAGA_<version>.bin` (the SmplTrek's, byte-identical to before: image 63a110a1 either way).
The family words and both CRCs were checked against the official Lofi-12 XT 1.2.179 / 1.5.205 and CyDrums 1.1.84
files. `tests/test_sysimage.py` (host) pins the SmplTrek bytes and checks each file verifies only as its own
device. Untested on either device.

Enaga also tells which device it runs on: each bootloader contains its update file's name, and the SmplTrek's
is still in DDR at the hand-over (`SmplTrek.bin` at 0xC70A8BA0). `firmware/kernel/device.c` searches
0xC7000000-0xC7200000 at power-on, before an app can reuse that memory; the result is the third log line, the
host table's `device()` (`enaga_device`) and `enaga::system::device()`. The family word was the other candidate,
but the bootloader compares it as an instruction immediate, not a stored word. A search of the top 16 MiB took
0.4 s on the emulator when nothing was found, so it covers 2 MiB (about 30 M cycles). The emulator writes the name
where BOOT 1.0.17 has it (`tools/emulate.py`, `bootloader_file`); `tests/test_e1.py` covers each name, none, and a
near miss.

### Unit session 4: device detection, AUTO OFF, PADS (2026-11-20)

Image 9bcbb5be (branch `devices`). The status page's third line read `SMPLTREK`; app switching still worked.
AUTO OFF set to 5 MIN was kept, logged `SETTINGS SAVED`, was in `/ENAGA/SETTINGS.TXT`, and switched the unit off
after 5 minutes. Session 3's revert did not recur; the settings code is the same in both images apart from the save
log lines, so its cause is unknown (open items). PADS: the user chose 1,600 / 1,000 / 600 (LOW / MEDIUM / HIGH)
from 1,100 / 800 / 500, which made little difference; MEDIUM, the default, now presses at 1,000 rather than the
original firmware's 800.

### Documentation pass (2026-11-20)

README, design.md and hardware.md checked against the code and the unit sessions. E2 and E3 marked complete in
Phases and Status; open items settled in sessions 1 and 3 removed (key names, inputs L/R, headroom with caches,
F1 at power-on, brightness, patch saving); Enaga's size corrected to about 190 KB; the Files plan notes the API
as built; per-device build output and device detection noted where the build and memory are described.

### API reference (2026-11-20)

docs/api.md documents the app interface header by header: the app's callbacks and contexts, audio, input,
display, LEDs, system calls, files, streams, WAV, app settings, the DSP library, the C interface and the language
runtime. `tests/test_api_docs.py` (host-only) gathers the public names from `include/enaga/` with regular
expressions (C names, host-table functions, C++ classes, enums, functions, members and constants, without
`namespace detail` and `private:` sections) and fails when one is not mentioned in api.md: 395 names now.
`<enaga/dsp.hpp>`'s summary gained what it had left out (biquads, FIR, wavetables, sync, chorus, Memory).

### Release builds (2026-11-20)

`.github/workflows/release.yml` runs on a pushed `v*` tag: it checks the tag against `VERSION` in
`firmware/build.py`, restores the tic6x toolchain from the Actions cache (built by `tools/build_tic6x_*.sh` on a
miss, 20-30 minutes; the key is the hash of those scripts and the patch), builds the firmware and creates a GitHub
release with `enaga-<tag>-<device>.zip` for each device and `SHA256SUMS`. The zips keep the card names, spaces
included, which GitHub would rewrite in a bare asset name. The firmware needs only GCC, so TI's tools are not
involved; apps are not part of the release.

### Pad pressure, E4 step 1 (2026-11-20)

`ENAGA_EVENT_PAD_PRESSURE` (7) and `App::on_pad_pressure(pad, pressure)`, 0-127, between a pad's press and release
(`firmware/kernel/main.c`). The algorithm follows Lofi-12 XT's (XF-23):
- **Settle:** a press starts a 40-report countdown, about 150 ms. The reading at the end is the baseline.
- **Start:** pressure turns on once a reading is more than 200 above the baseline.
- **Value:** the mean of the last 9 readings on a curve, sent when it moves by more than 1.

The curve is our own formula: 127 u³, with u running from the PADS threshold to 3,300, the same span velocity
uses. Lofi's table, read from its image, has thresholds 800, 1277, 1401 ... 3200. Read as raw = 800 + 2400 (k /
127)^a, the exponent comes out 0.333-0.346 along the table, so its inverse is a cube. 127 u³ reaches 1 at 1,277
from Lofi's 800, as the table does. It follows the user's PADS setting rather than a fixed 800.

The emulator test (`test_pad_pressure_after_the_hit_settles`) checks:
- the settle;
- a rise too small to start it;
- the 9-reading ramp;
- the change filter;
- nothing after the release.

The CHECK app's PANEL page shows the last pad's VEL and PRS for the unit; checklist step 10 asks how it feels.

### Input selection, E4 step 2 (2026-11-20)

`enaga::system::input(source, level)` (host table `input`, appended): sources `ENAGA_INPUT_MIC`, `_HIZ`,
`_DYNAMIC_MIC`, `_LINE_STEREO` and `_LINE_MONO`, levels `ENAGA_INPUT_LOW`, `_NORMAL` and `_HIGH`.
- **The routes:** each pair writes the codec's ADC routing and PGAs and sets the front-end pins to the stock REC
  SRC end state (docs/hardware.md "Input routes"). The ISR feeds a one-ADC source to both channels.
- **Levels:** our own uniform scheme rather than Lofi's. LOW / NORMAL / HIGH are the stock three gain steps for
  Hi-Z and the dynamic mic. For the lines, LOW is the PAD and NORMAL and HIGH are the same.
- **Mono with the PAD:** stays mono (Lofi's behaviour), not the stock's both-slots artefact.
- **Built-in mic rule:** applied in the ISR. The mic's input is zeroed while GP3[8] reads low.
- **Switching:** the input is silenced while the codec is rewritten and for 8 blocks after.
- **Between apps:** `apps_unload` puts LINE_STEREO NORMAL back, the start-up state, writing the codec only if
  the input had changed.

Tests:
- `tests/test_input.py` checks every pair's codec registers, pins and what the app hears, with distinct levels
  on the two ADC slots. It also checks the headphone rule and the quiet stretch.
- `tests/test_menu.py` checks the reset on an app switch.

On the emulator, the input stream's second word and the output capture's second word are the left channel
(slot 1), as the ring's word order implies.

CHECK's AUDIO page chooses the input (STEP 9-13, B for the level) for the route-by-route unit check
(checklist step 7). The routes copy stock code and have not run on a unit; GP1[2]'s role in particular is
inference.

### MIDI on the DIN jacks, E4 step 3 (2026-11-20)

UART0 driver `firmware/hal/uart0.c`, parser and sending `firmware/kernel/midi.c`; apps use `on_midi(MidiMessage)`,
`on_midi_sysex` and `<enaga/midi.hpp>`. ABI additions:
- `ENAGA_EVENT_MIDI`: id = port bit, value = the three bytes;
- `enaga_midi_port`, `enaga_sysex_flags` and `ENAGA_SYSEX_CHUNK`;
- host `midi_send` and `midi_channel`;
- the app table's `midi_sysex`, called only when the app's `size` covers it.

Channels: apps get and send all 16; MIDI CH is only a default (docs/api.md "MIDI").

Choices:
- **Transmit:** one byte per THRE interrupt, so a real-time byte from its own queue waits behind at most one
  byte. A message queue of 2 KiB, all or nothing per call.
- **Contexts:** the main loop and the audio callback may both send. Queuing turns interrupts off briefly.
- **Delivery:** MIDI goes to the app while the menu is open, so notes from outside keep playing. It also counts as
  activity for AUTO OFF.
- **Normalising:** note on with velocity 0 arrives as note off. Active sensing is consumed: a gap of over 300 ms
  gives CC 123 on every channel.
- **THRU:** at the message level, through the same queue, so it never interleaves with the app's output. SysEx is
  not forwarded, since a long one would block the app's messages while it passed.
- **New settings:** MIDI CH and MIDI THRU. IN/OUT routing waits for USB.

Tests:
- `tests/test_midi.py` covers the parser cases, SysEx chunking and cutting, the sensing timeout, every send helper,
  a clock byte going ahead of a 300-byte SysEx, and THRU.
- Short cases use the emulator's unpaced UART0 path in bursts of at most 16 bytes.
- With the emulator's MIDI additions of the same day (a paced MIDI IN sender, transmit timestamps, an OUT-to-IN
  cable), the tests also cover:
  - a 1,002-byte SysEx dump and 350 dense messages at the full 31,250 baud, with no byte lost;
  - sent bytes going back to back, one character time (119,040 cycles) apart;
  - a round trip through the cable;
  - CHECK's 120 BPM clock (`tests/test_check.py`): 48 a second, each gap within 19-23 ms, no drift.

CHECK has a fifth page, MIDI, for checklist step 16.

### Clock, transport and SYNC, E4 step 4 (2026-11-20)

`firmware/kernel/clock.c`, `<enaga/clock.hpp>`. One clock, 96 ticks to the quarter note (Lofi-12 XT's
resolution).
- **Where it runs:** it advances in the audio interrupt before the app's callback. `clock_ticks` gives the
  block's ticks with their frames, so apps sequence on the sample.
- **Sources** (the CLOCK SRC setting):
  - INT: the app's start / stop / continue / locate / tempo.
  - MIDI: clock and transport taken in UART0's interrupt, in byte order with Song Position Pointer, so a CONTINUE
    after an SPP resumes at it.
  - SYNC: rising edges on the GPIO bank 7 interrupt.
- **Following** works as stock HW-41. The clock runs at the measured tempo, capped at the next clock's or pulse's
  tick. Over one clock or pulse behind, it catches up at once.
- **Outputs** (CLOCK OUT for MIDI clock and transport, SYNC OUT for pulses) follow the clock from any source.
  They are sent from the audio interrupt, so they are exact on average and within one block (1.33 ms).
- **Transport events:** `ENAGA_EVENT_TRANSPORT` to the app from the main loop.
- **ABI:** the event, the clock enums and structs, and host `clock_control`, `clock_state`, `clock_ticks`.

Found while testing:
- **Next tick:** working out the next tick from the float fraction lost a tick now and then at the cap, when
  rounding put the position a hair past it. An explicit next-tick counter, and pinning the position exactly at
  the cap, fixed it.
- **No interval yet:** before an outside source's first interval the clock stalled, then caught up dozens of
  ticks in one block. It now runs at the last known tempo until the interval arrives.

`tests/test_clock.py` uses the emulator's paced MIDI IN, `sync_in` pulse trains, transmit timestamps and the
SYNC OUT pin trace:
- **Internal at 120 BPM:** MIDI clock exact on average, every gap within a block. SYNC OUT 250 ms apart and
  15 ms wide. Every tick reaches the app in order.
- **Song Position Pointer** before CONTINUE, and on a locate while stopped.
- **Following MIDI clock** at 100 BPM: the tempo to within 0.5, START, STOP, then SPP plus CONTINUE resuming at
  tick 192.
- **Following SYNC** at 90 BPM, stopping when the pulses stop, and bridged to MIDI clock.

CHECK's MIDI page shows the clock and starts it (checklist step 17).

Not built: the original's SYNC polarity settings (falling edges in, active-low out). Add them if a unit or a
user's gear needs them.

### USB MIDI, E4 step 5 (2026-11-20)

**The stack.** TinyUSB 0.21.0 (MIT), vendored as a subset in `firmware/third_party/tinyusb`, with its Mentor MUSB
device driver.
- **StarterWare's usblib**, which the original firmware uses, carries a TI-only licence forbidding combination
  with "viral" open-source code, so it cannot ship in an MIT project.
- **TI's Processor SDK USB driver** for the OMAP-L138 (BSD-3) was the reference for the chip-specific parts and is
  the fallback.

**The C6748 port** (`firmware/usb/musb_enaga.h`, selected by `TUP_USBIP_MUSB_ENAGA` in `firmware/usb/tusb_config.h`
since the C6748 is not a TinyUSB MCU) copies the original firmware's bring-up and interrupt handling. Three
patches to TinyUSB, each marked `Enaga:` and listed in its README:
1. **Interrupt status from the TI wrapper**, for the first read and for the handler's re-check. With CTRLR.UINT =
   0 the core's own interrupt registers must not be read (SPRUH79C), and TinyUSB read them.
2. **No double-packet-disable registers:** the C6748's core lacks them at 0x340 / 0x342. The emulator stopped on
   the access during enumeration.
3. **Real-time MIDI as CIN 0xF:** TinyUSB sent CIN 0x5, contrary to USB MIDI 1.0 table 4-1.

**Code checks.**
- The FIFO registers get 32-bit accesses with 16- and 8-bit tails. The main register struct is not packed.
- User buffers are read with byte loads: TinyUSB treats non-ARM targets as strict-alignment.
- The firmware grew by about 37 KB.

**Glue** (`firmware/usb/usb.c`).
- **Descriptors:** one configuration with the MIDI interfaces (audio control and MIDI streaming), bulk 0x01 /
  0x81, 64 bytes, self-powered. The product string names the device ("Enaga SmplTrek", "Enaga Lofi-12 XT",
  "Enaga CyDrums").
- **Ports and settings:** MIDI from USB goes through midi.c's parser as port `ENAGA_MIDI_USB`. Sending goes through
  a 2 KiB queue (any context, all or nothing, nothing while unconfigured) drained in the main loop. New settings
  MIDI IN and MIDI OUT (DIN, USB, BOTH); the clock's MIDI out follows MIDI OUT.
- **MIDI clock from USB** reaches the clock from the main loop, so it jitters by about a pass. The tempo is the
  mean of 24 intervals, so it holds to within about 2 BPM on the emulator.

**Tests** (`tests/test_usb.py`, on the emulator's USB0 model and scripted host, which the emulator agent built to
the brief of 2026-11-20):
- enumeration, the descriptors and the per-device product string;
- MIDI in and out, SysEx both ways;
- the routing settings;
- MIDI clock in and out over USB;
- unplugging and plugging back in.

CHECK's MIDI page marks each message D or U (checklist step 18).

Not done: mass storage and USB audio (E5), the real USB IDs.

### The UI toolkit, E4 step 6 (2026-11-20)

`<enaga/ui.hpp>`: header-only, app side, nothing in the firmware or the ABI. Four widgets, each fed the app's key
and knob callbacks and returning an `Action`:
- **`List`:** scrolls with UP / DOWN / VALUE; OK chooses, CLR goes back.
- **`ParamPage`:** the synth's pattern made general. Parameters in groups of four on knobs A-D, the selection
  buttons flip groups, a knob push resets, values shown by labels, a format function or number and unit.
- **`Dialog`:** OK / CLR in a box over the page.
- **`TextEntry`:** up to 16 characters from the font's file-name-safe set; knob A or LEFT / RIGHT move, knob B,
  VALUE or UP / DOWN change.

The look follows the Enaga menu and the synth: title row, dim cursor bar, right-aligned values, value bars. The
controls follow the owner's conventions: four-knob groups, selection buttons for groups, F1 left to the app.

Tests:
- `tests/test_ui.py` compiles the header for the host against a stand-in host table that records the text drawn.
  It checks each widget's logic and what it draws.
- A mutation check (resetting to the wrong value, not trimming text) failed the expected checks.
- `tests/test_widgets.py` drives `examples/widgets` on the emulator through every widget by keys and encoder
  pins.
- Screenshots of the four pages were looked at.

The synth keeps its own drawing: it is a demo and gets bug fixes only. Naming a patch on the unit would use
`TextEntry`, but that is a feature.

Not done here: app icons in the Enaga menu, which came next (below).

### App icons in the menu, E4 step 6 (2026-11-20)

The second half of step 6, with the owner's choices: a 16 x 16 icon in the screen's 16 greys, optional, in the
package; a list with the icon at the left of each line; a generic icon for apps without one.
- **Package:** `ENAGA_PACKAGE_ICON` (flags bit 0) says 128 bytes of icon follow the payload, two pixels a byte. The
  loader accepts that flag only (any other bit is still `BAD_HEADER`) and calls a package whose icon is cut short
  `TRUNCATED`. The header is unchanged, so packages without an icon load as before.
- **Build:** `icon.txt` in the app's folder, 16 lines of 16 hex digits (`.` for 0), read by `tools/build_app.py`;
  a malformed file stops the build. `examples/tone` (a sine) and `examples/widgets` (a knob) have one. The synth
  does not (it is a demo).
- **Menu:** lines 20 pixels apart from y = 24, five on screen. Each has a 16 x 16 icon at the left: the package's,
  else a generic one (four tiles). SETTINGS (three faders) and STATUS (an i in a circle) have their own. The name
  follows at x = 22 (15 characters shown, was 18), ON at the right for the running app. The selected line is a
  dim grey bar, as in the toolkit's `List`; grey 0 in an icon shows the bar. Marks at the right edge show more
  lines above or below. The icon is read with the name, when the menu opens.

Tests:
- `tests/test_e0.py`: the loader takes a package with an icon, rejects unknown flags and a short icon. `icon.txt`
  lands after the payload pixel for pixel.
- `tests/test_menu.py`: on the emulator, the menu's canvas holds TONE's icon, the generic icon for the menu test
  app, and the SETTINGS and STATUS icons, each over the right background. With package icons switched off in
  the menu code, the test failed.
- Screenshots of the menu, at the top and scrolled to SETTINGS, were looked at.

### The SDK, E4 step 7 (2026-11-20)

The owner's choices: a prebuilt Linux x86-64 toolchain published with each release, its source alongside; the
DSP library stays source in the SDK, built with each app by cl6x when the developer has it, GCC otherwise.
- **`tools/make_sdk.py <tag>`** writes `enaga-sdk-<tag>.zip`: `include/`, `runtime/` (DSP sources included),
  `tools/build_app.py` and `tools/paths.py`, `template/`, the tone, widgets and synth examples,
  `docs/getting-started.md`, `docs/api.md`, the licence and a README. 0.1 MB. The SDK root plays the repository's
  part for `paths.py`, so nothing in the build tool changed for it.
- **`build_app.py`**: an app folder outside the repository (or SDK) now builds into its own `build/APP.ENA`,
  not into the SDK's folder. Inside, as before.
- **`tools/package_toolchain.py <tag>`** writes `enaga-toolchain-<tag>-linux-x86_64.tar.xz`, `tic6x/` to unpack
  into the SDK's `tools/`. Host programs are stripped, target libraries have no debug information, and LTO, gcov
  and the manuals are left out. 27.6 MB from 1.1 GB. It needs glibc 2.38 or newer and libzstd (built on Ubuntu
  24.04). The same script writes `enaga-toolchain-<tag>-source.tar`: the six upstream tarballs (GCC, binutils,
  GMP, MPFR, MPC, ISL), checked against pinned SHA-256s and downloaded when the build directories lack them (a
  CI toolchain cache hit), with the patch and both build scripts. That meets GPLv3's corresponding-source duty for
  the binaries. Apps are not bound by it: libgcc and the libstdc++ headers fall under GCC's runtime library
  exception.
- **The release workflow** adds the three archives to each release, covered by `SHA256SUMS`, with a note on them.
- **`template/`**: a one-voice synth on a `ParamPage` (wave, attack, release, level), played by the pads and MIDI
  notes, with a note icon. Each value it shares with `audio()` is one word written by one side; `audio()` builds
  the envelope shape itself when the times change, so it never reads a half-written struct.
- **`<enaga/bytes.hpp>`**: little- and big-endian loads and stores at any address, byte by byte.
  The null-read note was already in api.md ("Language and runtime"); the getting-started guide repeats both.
- **`docs/getting-started.md`**: from the release downloads to the app on the card, then what to change first
  and the rules that matter most.
- Not done: the prebuilt DSP library (the owner chose source); macOS and Docker toolchains.

Tests:
- `tests/test_sdk.py` unpacks the SDK zip on its own, copies the template outside it, and builds it and the
  three examples with the SDK's build tool. Each is byte-identical to the repository's build apart from the
  display name, which comes from the folder.
- The packaged toolchain is unpacked and builds the template and the synth to the same bytes as the full one.
  Its source archive holds every tarball.
- The template plays a MIDI note on the emulator, and the release dies away after note off.
- `tests/test_bytes.py` checks every load and store at eight offsets on the host, and that the bytes around
  them are untouched. A swapped byte order in `store_le32` failed it.
- The source download and its hash check were run against gcc.gnu.org for one tarball.
- The release workflow itself runs only on a tag, so it has not run yet.

### The exit test, simulated, E4 step 8 (2026-11-20)

A fresh agent with no knowledge of Enaga got only `enaga-sdk-v0.2.0.zip` and the toolchain archive, as a release
would publish them. It also got an empty home folder and an empty folder for the card, and was barred from the
repository and the network. It followed `docs/getting-started.md` literally:
- Unpacking, copying the template, the build and the copy to the card each worked first time, in about five
  minutes. It ran with its own HOME, so it had no cl6x: the GCC-only path, as a newcomer would have.
- It then made the guide's first two changes from `docs/api.md` alone: the pads an octave down, and a fifth
  parameter, CUTOFF, through `Svf`. Both compiled first time under `-Werror`.
- Its card, booted in the emulator: the app started (the only one installed), played a MIDI note and fell silent
  after the release. Its screen was the one it had predicted from the docs (MY APP, 1/2, PLAYING).

It found no blockers. Fixed from its notes:
- **The guide:**
  - Installing Enaga now links to the project README on GitHub, since the SDK has no README section for it.
  - The three names are explained: the project folder, the card's package folder, and the menu's upper-case name.
  - It says which folder the commands run in.
  - It covers `*.ti.c` files, `ENAGA_TI_CGT`, and that the DSP library is built into the SDK's own folder, so
    the SDK must be writable.
  - "Where next" says plainly that the emulator is in the repository, not the SDK.
- **api.md:**
  - The opening points to the guide instead of the repository README.
  - `Param`'s field order and `format`'s signature are given, with examples.
  - The package-name rule is the same as in the guide.
- **build_app.py:**
  - Its own errors (no sources, a bad name or icon, a failed compile or link) are one line, not a traceback.
  - A folder without sources no longer gets a `build/`.
  - The missing-compiler and no-cl6x messages point to the guide, not to repository scripts or `paths.py`.

Left as they are: a number-to-text helper for `format` functions (it wanted Hz, and the SDK has no libc), and a
way to try apps without a unit or the repository.

### Version 0.2.0

`VERSION` in `firmware/build.py`, `ENAGA_VERSION`, the USB device release (`bcdDevice` 0x0020) and the README go
to 0.2.0 for the first release with E4's work: MIDI (DIN and USB), the UI toolkit, app icons and the SDK. ABI_VERSION
stays 0; the interface is still provisional, and an app needs firmware of its own version.

### Unit session 5: the 0.2.0 release (2026-11-21)

The release's `SmplTrek.bin` (sha256 a93f1cb1..., the same bytes as the local build), CHECK, SYNTH and MYSYNTH (the
SDK template built from the release downloads). Checklist steps:
- 1-3: as expected; the log starts `ENAGA 0.2.0`.
- 7, input routes: no ERR on any route. MIC follows the built-in microphone, and LOW / NORM / HIGH change the level.
  The source was too quiet to judge the others (open items).
- 10, pad pressure: PRS stays at 0 resting after a hit and follows harder presses smoothly; the feel is good.
- 13: the menu icons read clearly; switching works. MYSYNTH, built from the SDK, plays.
- 14: the settings work; PADS is fine.
- 16-17, DIN MIDI and clock: not run (no equipment at hand).
- 18, USB MIDI: passes (docs/hardware.md "USB"). A first reading of SYSEX after a USB SysEx was not repeated in
  five later tries, the same bytes included.
- One crash on CHECK's MIDI page after STEP 4, not reproduced (open items). An "SD issue" remembered on the status
  page (not photographed) was probably the
  `W1` / `W7` write probe that `ENAGA/SDTEST.BIN` on the staged card starts at every boot.

### MIDI out: a stalled USB host no longer holds up DIN (2026-11-21)

`midi_out_bytes` is all or nothing across the ports, so a sender can wait for room. With a host that had stopped
reading USB (Windows lists the device but no program has the port open), USB's 2 KiB queue filled and then every
message was refused for DIN too: a new test (`test_usb`, a host that stops reading) sent twelve 301-byte messages
and DIN got 8. Sending to each port separately would have broken that waiting: USB taking a message DIN had no room
for would return true and lose it on DIN. Instead USB counts as stalled after 100 ms with bytes queued and none
taken (`usb_midi_stalled`), and a stalled USB no longer holds a message back: it gets what fits and drops the rest
(`usb_tx_lost`). DIN always drains (31,250 baud), so its queue being full is only ever a wait. When the host reads
again, USB carries on.

### Battery and power, E5 step 1 (2026-11-21)

Enaga reads the battery, as the original firmware does (`firmware/kernel/power.c`, docs/hardware.md "Power"). The
level rules were not fully known: smpltrek-re had the threshold tables but had not read the loop that steps the
level, so the stock `battery_sample` and `battery_set_type` were run in its c6sim with every raw value from 3700 down
to 2400 and back, for the three types. Result: a step down below the first table, a step up at or above the
second (so the second is hysteresis, as smpltrek-re supposed), the first average setting the level directly, and
readings under 2000 ignored.
- `enaga::system::battery()`: 0-4, or -1 off batteries (GP3[7] low). A new `battery` entry at the end of
  `enaga_host_t`.
- BATTERY setting (ALKALINE, NiMH, LITHIUM) on the SETTINGS page and in `SETTINGS.TXT`. The file's write buffer
  grew from 160 to 320 bytes: with the new line the longest file is about 180.
- A battery mark at the top right of the menu (one bar per level) and `B` on STATUS; level changes are logged
  with the raw average, for tying the raw values to volts on the unit.
- At level 0 on batteries: a bar at the bottom of the screen counts down 30 s, then the unit switches off as POWER
  does, showing BATTERY EMPTY. The app's `stop()` runs and its files are closed first. If the level recovers in
  time, nothing happens. The original firmware only redraws every 30 s at levels 0-1; the switch-off is Enaga's
  own, to keep a fading supply from cutting a card write.
- `tests/test_battery.py` (an app that logs the level and saves a file in `stop()`): thresholds and hysteresis,
  readings under 2000, the power source, the BATTERY setting, the menu mark, the switch-off with the file saved,
  and a recovery that cancels it.

### SYNC polarity, E5 step 2 (2026-11-21)

POL IN and POL OUT settings (RISE, FALL; both RISE by default), as the original firmware's POLARITY IN and OUT
(smpltrek-re HW-27; docs/hardware.md "SYNC IN / SYNC OUT").
- Which label is which value was not written down in smpltrek-re's docs beyond a note in its emulator README, so
  the stock settings row (0xC15B9E80) was read: value 0 shows RISE, 1 FALL. Stock value 0 arms GP7[8]'s *falling*
  edge, so RISE on the stock unit is the pin's falling edge, presumably the jack's rising edge through an inverting
  input. Enaga follows the stock meaning of each label. Before this, Enaga armed the pin's rising edge, so with
  a device whose pulse reaches the pin inverted it counted the end of each pulse: same tempo, late by the pulse
  width. That is the new default's only change in behaviour.
- POL OUT RISE idles SYNC OUT low and pulses high (as before); FALL idles high and pulses low.
- `clock_sync_polarity_changed()` re-arms the edge (clearing any edge pending from the change) and re-drives
  SYNC OUT, at start-up and whenever either setting changes.
- `tests/test_clock.py` `SyncPolarity`: RISE counts the pin's falling edge and not its rising one, FALL the other
  way round, and POL OUT FALL idles high with 15 ms low pulses. The settings come from `SETTINGS.TXT`, through
  the same apply as the SETTINGS page.

### USB DRIVE, E5 step 3 (2026-11-21)

The SD card as a USB drive, from the menu's new USB DRIVE line (between SETTINGS and STATUS), as decided in the plan.
- **USB:** a mass storage function beside MIDI in one configuration (interface 2, bulk endpoints 0x02 / 0x82),
  TinyUSB's MSC class vendored from the same 0.21.0 tarball (hash checked). It is always there but reports no
  medium unless the page is open, so a computer sees an empty card reader; MIDI is never interrupted by
  re-enumerating. Opening the page reports UNIT ATTENTION (medium changed), and the computer mounts the card.
  Reads and writes go straight to the SD driver in the main loop's `usb_poll`, 16 sectors per command. START STOP
  UNIT with START clear (eject or stop) marks the drive ejected. SYNCHRONIZE CACHE passes (nothing is cached);
  other commands TinyUSB does not answer get ILLEGAL REQUEST.
- **The page:** opening it stops the running app as a switch does (`stop()`, files closed), unmounts FatFs
  (`files_release`) and shares the card. It shows whether a cable is in and the computer has read the card, and
  the KB read and written. It ends on the eject, on the cable pulled after a computer had the drive, or on OK /
  CLR; then the card is mounted again (`apps_mount`) and the menu reopens rescanned, with the reason as its
  message. The app is not restarted. AUTO OFF waits while the page is open.
- **Card size:** `sd_init` now reads the CSD (CMD9, between CMD3 and CMD7) into `sd_sectors`, for CSD 1.0 and 2.0;
  the response registers are read as Linux's davinci_mmc does (MMCRSP01 = bits 31-0).
- **Unplugging:** TinyUSB's MUSB driver left the DISCON interrupt empty, so after a cable was pulled the device
  stayed configured in TinyUSB's eyes until the next bus reset (USB MIDI included). DISCON now signals
  `DCD_EVENT_UNPLUGGED` (a marked local change).
- `tests/test_usb_drive.py`: no medium before the page (INQUIRY answers, TEST UNIT READY says medium not present,
  reads fail, the app runs on); the page stops the app (its `stop()` file is on the card), UNIT ATTENTION then
  ready, READ CAPACITY matches the card, sectors read back as the card holds them; an app package copied the way a
  computer would (added to a copy of the image offline, the changed sectors written over USB) lands intact, the
  eject closes the page, the drive is gone again and the menu lists the new app; OK ends it; unplugging ends it.
  `tests/test_usb.py` expects the new interface and endpoints.
- `tests/test_synth.py` pressed F1 and the second key of a chord at the same instant; which one the key scan
  saw first depended on where the firmware was in its scan, and this step's larger kernel moved that, so the synth
  saw F2 alone. The test now holds F1 15 ms first, as a hand does. Nothing in the synth changed.

### USB audio, E5 step 4 (2026-11-21)

USB Audio Class 1.0 beside MIDI and the drive, on the emulator's new isochronous streams (smpltrek-re, at the
user's request: `usb_iso_stream_out` / `_in`, ISOUPDATE, SOF every millisecond).
- **Descriptors:** one AudioControl interface (0) for the whole audio function, listing MIDI streaming (1) and the
  audio streaming interfaces (2 out, 3 in); USB streaming terminal -> line out terminal, line in terminal -> USB
  streaming terminal; no feature unit. Each streaming interface: alternate 0 empty, alternate 1 PCM 2 x 16 bits at
  48 kHz, one isochronous endpoint of 196 bytes (0x03 adaptive, 0x83 asynchronous). The drive moved to interface 4.
- **A class driver of Enaga's own** (`firmware/usb/usb_audio.c`, TinyUSB's app driver hook), not TinyUSB's audio
  class (UAC2-first, and its transfers finish in the main loop, which runs about 850 times a second: too few for a
  packet every millisecond). It claims interfaces 2 and 3, allocates their double-buffered FIFOs, and answers
  SET_INTERFACE. The packets move in the USB interrupt on SOF through the driver's `sof` hook: OUT read from the
  FIFO, IN loaded for the next frame (POWER.ISOUPDATE), the endpoints' own interrupts off. TinyUSB's MUSB
  `dcd_sof_enable` was empty; it now sets the SOF interrupt enable (a marked local change).
- **The app's side:** `ENAGA_INPUT_USB` (abi.h, value 5) makes the computer's audio the callback's `in`; the codec
  route is left alone. The callback's `out` goes to the computer before VOLUME, whatever the input.
- **Clocks:** OUT is resampled (linear interpolation) by a ratio that keeps its ring at 144 frames, the ratio
  following the smoothed level (proportional, at most 2000 ppm); IN packets are 47, 48 or 49 frames to keep its ring
  at 128, on a level averaged over about 32 packets. Both levels are taken at a steady phase: the frames the other
  side made or used since its last packet or block are counted from the TSC, since 1 ms packets against 1.33 ms
  blocks otherwise saw-tooth by up to a block and look like drift. The IN ring starts exactly at its target. Both
  halves of the IN FIFO are kept loaded, so a SOF served up to a block late (the USB interrupt waits for the app's
  callback) costs no packet: checked with an app whose callback takes 86% of the block, which loses packets when
  only one half is loaded.
- **A harness lesson:** asking the emulator for a set number of IN frames and stopping leaves one frame with no
  packet and no token at each stop, 2000 ppm at 500-frame chunks, which looked like drift and pinned the
  resampler. The tests stream as a computer does: tokens asked for well ahead, OUT packets kept in hand.
- `tests/test_usb_audio.py` (with `tests/apps/input`, STEP 10 now the USB input, STEP 11 a heavy callback): the
  descriptors; a 1 kHz sine and a counter sent to the unit come back through the app at their level, in order,
  48 frames a packet, ratio 1.0; a computer 1000 ppm fast and then slow (a 49- or 47-frame packet in 20) is
  followed (ratio 1 +- 1/960) with nothing lost; the output reaches the computer without the USB input; stopping
  the OUT stream silences `in` and stopping IN stops loading; unplugging and replugging while streaming; the heavy
  callback. `tests/test_usb.py` expects the new endpoints. CHECK's AUDIO page: STEP 14 chooses USB.
- `tests/test_e1.py` `test_one_sample_opening_while_held_is_ignored` failed on this step's build: its 3 ms
  openings 9 ms apart assumed key samples exactly 6 ms apart, but samples ride the main-loop passes (on the
  emulator, gaps alternating 5.25 / 6.75 ms in a 12 ms pattern), and a 6.75 ms gap could catch two openings in a
  row, a genuine release. Twelve openings 10 ms apart now: never two in a row, and several sampled; the test fails
  again if the debounce is removed (checked). The firmware is unchanged.

### USB DRIVE: OK waits for the writes (2026-11-22)

macOS and Linux keep a copy's writes in memory and send them a moment after the copy looks finished, so OK on the
USB DRIVE page could end the drive in the middle of them and cut the file short. OK now starts a wait: the page
shows WAITING FOR THE WRITES TO FINISH and ends once the computer has written nothing for 2 s (at once if it wrote
nothing at all); OK or CLR again ends it at once. The quiet time is measured in `menu_service` from the write
count, so `usb.c` is unchanged. *Decided:* no automatic restart of the app afterwards; the menu stays, and choosing
the app loads the new package, as leaving the menu always works. `tests/test_usb_drive.py`: OK waits while a write
arrives late, restarts the wait on another, ends 2 s after the last; OK twice ends it at once. Checklist step 20
gains copying over the running app and pressing OK.

### UI toolkit, second part, E5 step 5 (2026-11-22)

The pieces around the widgets that every screen ended up needing, in `<enaga/ui.hpp>` as planned: `Line` (text,
padded numbers, fixed point, hex, note names, outside text made drawable, `add_if_fits`, `column`, `copy_to`),
`drawable`, `status_line` / `hint_line` with `body_top` / `body_bottom`, `Notice`, `bar`, `bar_centred`, `meter`
and `meter_db`. `ParamPage` draws its bars with `bar` (the same pixels). api.md now says LED writes go out only
when a level changes.
- `bar` placed values with a 64-bit division at first; the C674x has none, and libgcc's helper drags in its
  unwinder, which an app cannot link (`__exidx_start` undefined). The arithmetic is 32-bit now, halving a range
  wider than 2^22 first.
- `examples/widgets` uses all of it: the status line and hints on every page, a notice for each action, `Line`
  in place of its hand-written formatting, and a LEVELS page (F6) metering the input with peak marks and a
  centred PAN bar.
- Tests: tests/ui/host.cpp checks each piece's text and pixels (including a notice across the millisecond
  counter's wrap); `tests/test_widgets.py` adds the notice on the screen and gone after 1.5 s, and the meters
  dim in silence and lit with input on the emulator.

### UI toolkit, third part, E5 step 6 (2026-11-22)

`Grid`, `PadMap`, `FileBrowser`, and tabs and columns for `ParamPage`, in `<enaga/ui.hpp>`.
- **The pads are not a grid.** The plan said a 4 x 4 grid mirroring the pads; the panel has 15 pads in two
  staggered rows (hardware.md "Panel MCU"), so `PadMap` draws them that way, 16 px cells with the upper row offset
  by half a cell, and the general case, cells moved with the arrows, is its own widget, `Grid`.
- `Grid` keeps its lights in a 64-bit mask, hence `max_cells` 64. Right and left stop at a row's ends; VALUE runs
  through all cells in order.
- `FileBrowser` sorts as entries arrive (insertion into an index array; the file service lists in directory
  order). A listing still running when the browser moves on (OK into a folder, CLR, a new `open`) would deliver
  its entries into the new folder's list; requests run in order, so the browser counts the listings outstanding
  at the move and drops entries until that many have finished. Names of 64 bytes or more are left out rather
  than kept cut short, since a cut name gives a path that does not open.
- `ParamPage` tabs fall back to "2/3" when the names do not fit beside the title. Columns show five characters of
  name and value; `examples/widgets` now shows CUTOFF above 1 kHz as `4.5K` so it fits.
- `examples/widgets`: F7 PADS (the map labelled as a keyboard, lit by velocity while held), F8 FILES (from the
  root), F1 switching SOUNDS between list and grid and EDIT between rows and columns, and EDIT's groups as tabs.
- Tests: tests/ui/host.cpp (also run under the address and undefined-behaviour sanitizers while writing) checks
  grid movement, scrolling and drawing, the pads' places, tabs and columns, and the browser against a hand-driven
  file service: sorting, filtering, going in and up, errors, late listings and over-long names.
  `tests/test_widgets.py` adds the grid, the tabs and columns, pads lighting the map, and browsing a card with
  `/SAMPLES/Drums/kick_01.wav` on the emulator.

### What the first outside apps ran into, E5 step 7 (2026-11-22)

A list from a developer who wrote an app on 0.2.0, worked through in order.
- **64-bit division.** libgcc's `_divdi3.o` and its relatives carry unwind tables naming
  `__c6xabi_unwind_cpp_pr3`, which pulls in libgcc's unwinder, whose `__exidx_start` / `__exidx_end` the app
  linker script never defines (it discards `.c6xabi.exidx`): `undefined reference to __exidx_start`, far from the
  cause. Defining the symbols would link the unwinder into every app that divides; instead the runtime defines
  `__c6xabi_divlli`, `__c6xabi_divull`, `__c6xabi_remlli` and `__c6xabi_remull` (shift and subtract, a zero
  divisor is `fatal`), which come before libgcc, so its objects are never linked. libgcc's other helpers
  (shifts, multiplies, float) carry no unwind references. cxxcheck had an `int64` division check all along, but
  on constants the compiler folded; it now divides `volatile` values, signed and unsigned, and the old runtime
  fails to link it exactly as reported.
- **Strings.** `<enaga/string.h>` declares the runtime's `memcpy`, `memmove`, `memset`, `memcmp`, `strlen`, and
  the new `strcmp`, `strncmp`, `strchr`, `strrchr`. Not a plain `<string.h>` in Enaga's include folder: that
  would also shadow the system's in host builds of Enaga's headers (tests/ui). cxxcheck checks each.
- **`ParamPage` speed.** `Param::step` (per detent), and acceleration for parameters of more than 100 steps:
  turning the same parameter the same way within 60 ms counts three, within 30 ms eight. Encoder events come
  once a main-loop pass with the detents since, so the page times them itself (`enaga::ms`). It follows the
  parameter, not the knob, so flipping groups starts afresh. The times are guesses for the unit to settle; the
  widget tests now turn at 80 ms a detent for exact values, and check a 12 ms spin separately.
  `set_values` swaps the value array and keeps the group.
- **Font.** `< ( ) , _ ' * [ ]` added (drawn for Enaga, in the font's 5 x 7 style). Lower case always drew as
  upper case, so `ui::drawable` now says so; api.md had said other characters are left blank, when they draw as
  `?`. `Line::shown` keeps `_` now.
- **Emulator script.** `Emulation(card_mib=...)`, `turn()` (moved from the dry run, which still exports it),
  `power_off()` and `card_file()`; the command line's `--card-mib`, `--file`, `--turn`, `--power-off` and `--get`,
  and `--help` shows the whole description, `audio.wav`'s format included. `Emulation` now sets the encoders'
  pins at rest itself.

### Enaga's screens and the synth on the newer work, E5 step 8 (2026-11-22)

What steps 5-7 offer, put to use where Enaga and its example drew things by hand.
- **The synth** (an exception to "bug fixes only", as its job is to show how an app is written): the status line
  (patch, voices, load), the PATCH list as a `ui::List` (UP / DOWN now move it as well as VALUE, and it stops at
  the ends rather than wrapping; the loaded patch is marked `>` rather than with a block, and empty slots are no
  longer dimmed), `ui::bar`, a `Notice` for its messages, and `Line` in place of its own number and text helpers,
  in `params.cpp`'s formatting too. Paths stay hand-built: a `Line` is a screen's line, shorter than a path.
  Pressure now comes from `on_pad_pressure` (Enaga's aftertouch, from the pad's resting reading after the hit)
  rather than the synth's own curve over raw readings, which began at the press threshold, so a hard hit no
  longer read as pressure.
- **Not** `ParamPage`: the synth's groups hold two to four parameters picked from one table, and their values are
  `int16_t` in the engine, written with single stores the audio context reads. `ParamPage` wants four to a group
  in order and an `int` array; the synth draws its rows the same way instead.
- **The menu, SETTINGS and USB DRIVE pages** (C, in the firmware, so not the C++ toolkit): `ui_title`,
  `ui_hint` and `ui_cursor_bar` in `menu.c` draw the toolkit's frame: a rule under the title, hints on the bottom
  rows (120 and 110), and the lit bar for the current line. SETTINGS now marks its line with the bar instead of
  `>`, at 11 px a row like a `List` (eight on screen, with the same marks when there is more).
- **The template** shows the frame too: a status line with the note playing (`Line::note`), the parameter page
  under it and a key hint at the bottom; its ATTACK and RELEASE (0-2000 and 0-4000 ms) now speed up when turned
  quickly. getting-started.md's rules point at `Line` and `<enaga/string.h>`.

### USB audio on a computer: refused requests hung it (2026-11-22, unit session)

On the unit the computer listed the audio device, but playing to it hung the player and Audacity could not record.
On the emulator, the request computers send as a stream starts, SET_CUR of the endpoint's sampling frequency
(UAC1 5.2.3.2.3.1, sent although the descriptor offers no controls), timed out rather than stalled. Two faults:
- **Refused requests never stalled.** TinyUSB's MUSB port leaves the SETUP's RXRDY parked when the request has no
  data stage or an OUT one, and its EP0 stall wrote SendStall without servicing it; the core went on NAKing, and
  the computer's control transfer timed out (seconds, then a hang). The stall now services RXRDY too (vendored
  README); `tests/test_usb.py` refuses a request of each shape and expects a stall and a working device after,
  and fails on the old code. The emulator's host had only ever sent requests the device knew.
- **The sampling frequency was refused.** `usb_audio.c` now accepts SET_CUR (a rate other than 48,000 is logged,
  not refused mid-transfer) and answers GET_CUR / MIN / MAX / RES with 48,000. Anything else for the audio
  interfaces or endpoints is refused and logged on the status page as `USB REQ <type> <request> <value> <index>`
  (the first eight), so the next surprise shows itself. `tests/test_usb_audio.py` covers both.

### Version 0.3.0

`VERSION` in `firmware/build.py`, `ENAGA_VERSION`, the USB device release (`bcdDevice` 0x0030), the README and the
getting-started version go to 0.3.0 for E5's work so far: battery and power, SYNC polarity, USB DRIVE, USB audio,
the UI toolkit's additions, runtime 64-bit division and `<enaga/string.h>`, and the USB fixes from unit session 6.
ABI_VERSION stays 0: apps built for 0.2.0 must be rebuilt.

### Long names on the emulator tools' cards (2026-11-24)

An app with a file browser needed samples on the panel's card. `tools/panel.py` now takes `--file CARD_PATH=LOCAL`
(repeatable) and `--card-mib` (default 64), as `tools/emulate.py` already did. Both tools now build their cards with
`tools/card.py`, through the firmware's own FatFs configuration, instead of the emulator's `media.build_card`. That
builder's FatFs has long names off, so `--file` paths had been limited to 8.3. Now a card holds any name the unit
can read, spaces and Unicode included. `card.py` gains `file_args()` for the options: it expands `~` and stops on a
missing local file. `make()` checks the local files before it starts and raises `CardFull` when a file does not fit;
the tools report that with a hint to use a larger `--card-mib`. A `--file` path that is an app's `APP.ENA` (FAT
names ignore case) is an error. `tests/test_card_files.py` covers the panel's card, the widgets example browsing to
a long name on an `Emulation` card, and the errors.

### Fast memory measured and set aside (2026-11-24)

Part of L2 (or L1D, or the 128 KiB of shared RAM) as RAM that apps could allocate from was drafted for 1.0 and
measured on the unit first, by a MEMORY page added to CHECK on the branch `fast-memory` (not merged; it holds the
page, the draft and the full table). Three loops (a delay line, random interpolated reads, one pass) timed on
buffers in DDR, L2 RAM, L1D RAM and shared RAM across eight cache splits, firmware 0.3.0:
- L2 RAM was 1-4% faster than the same 64 KiB buffer in DDR through the cache. L1D reaches L2 RAM and L2 cache the
  same way, so L2 RAM only saves the misses of a buffer the cache would drop.
- L1D RAM gained nothing; halving L1D cost a 64 KiB delay line 7%.
- Shared RAM was DDR's speed cached and 3-8 times slower uncached.
- A smaller cache cost random reads on a 1 MiB buffer 13-21% and the LOAD page's voices up to 7%.

So Enaga keeps L2 and L1D all cache, and apps get no fast-memory API. The original firmware also makes all of L2
cache (it keeps 16 KiB of L1D as RAM for its audio DMA buffers). Not measured: an app whose code and data together
overflow 256 KiB; if one turns up, the branch's MEMORY page can be rerun.

### Register writes diffed against the original firmware (2026-11-24)

The smpltrek-re side captured the original firmware's register accesses in the emulator, phase by phase (its
`docs/stock-register-writes/`: USB0, the codec and I2C0, McASP0 and EDMA, MMC/SD and the SD commands), and gave the
emulator a capture (`capture_start` / `capture_take`, `codec_log`, `sd_log`). `tools/register_capture.py` runs
Enaga through the same phases with the same ranges and raw format (the input app: boot, USB attach, bus reset,
enumeration, MIDI out, the audio interfaces on, streaming and off, suspend, detach, each input route, power-off;
the battery app: USB DRIVE opened, read, written and ejected, with the remount). The emulator's own
`stock_register_tables.py --raw` lays it out like the stock tables. What the diff shows:
- **Fixed: USB0 touched before it was powered.** TinyUSB's `dcd_init` reads and writes INTRUSBE and clears the
  wrapper's interrupts before its PHY hook, which is where Enaga enabled PSC1 module 1. Accessing a module that is
  still off is undefined on the chip (it worked on the unit, and the core's reset just after undid the writes; the
  original firmware never does it). `usb_init` now starts the PHY before `tusb_init`, and the hook skips a second
  start. `test_usb.test_the_core_is_powered_before_it_is_touched` fails on the old order.
- **The same as the original:** the PHY bring-up (CFGCHIP2 0x0972, PSC, CTRLR reset, the clock-good wait, POWER 0
  then 0x40), the codec's whole start-up (49 writes against Enaga's 42: the same values in the same order; the
  extra seven are the original's first route), every McASP0 register, the SD clock (CLKRT 3) and the card's
  start-up commands with CMD55 at RCA 0 until CMD3. The input routes put the same input into the same converter
  as the original's for every source (byte for byte the original writes 21 and 24 as 0xFC when it changes route,
  Enaga keeps the 0xF8 both write at start-up), with the HIGH levels at +10 and +21 dB.
- **Different by design, kept:**
  - The audio DMA buffers are in DDR (the original's are in L1D RAM), with 128-frame blocks against 256, and a
    transfer-complete interrupt.
  - The SD driver moves data by CPU with multi-block commands; the original uses EDMA, and also sends CMD6 (high
    speed) and ACMD51, at the same 23 MHz clock.
  - USB: the audio IN FIFO is double-buffered and loaded for the next frame (ISOUPDATE), where the original's is
    single-buffered without ISOUPDATE. The original's way of knowing a packet went (TX interrupt, never
    FIFONOTEMPTY) is safe only single-buffered; Enaga's audio class serves IN on SOF and does not depend on it.
    MIDI and the drive's bulk endpoints are single-buffered, 64 bytes, as the original's.
  - USB: SOF interrupts only while a stream is open (the original keeps them on); TinyUSB masks and unmasks the
    wrapper's interrupt around its critical sections (about 2,500 pairs over the run), which the original never
    does; the endpoint interrupt enables follow the open endpoints (the original enables all).
- **The original's card-reader mode runs at high speed:** it resets USB and comes back with HSENAB left on, a
  mass-storage-only configuration and CPPI DMA. Enaga's drive is a function of the full-speed composite device. At
  about 150 KiB/s the drive is far from full speed's limit, so this is not yet the bottleneck.
- **Emulator reset values:** 13 registers the original writes differ from SPRUH79C in the model (the USB interrupt
  enables, McASP clock controls, ICSAR, CFGCHIP2, SUSPSRC, MMCST1). Enaga writes each whole (the endpoint
  interrupt enables at the first bus reset, before any endpoint can interrupt) or only reads status, so none
  affects it.

