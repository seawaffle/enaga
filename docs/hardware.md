# SmplTrek hardware notes for Enaga

What Enaga's drivers rely on, in one place. Register-level detail lives next to the code that uses it (the
comment at the top of each `firmware/hal/*.c` and `firmware/kernel/audio.c`); this document is the map.
Facts marked **unconfirmed** are believed from how the original firmware uses the hardware but have not
been checked directly on a unit. TI documents cited: the C6748 data sheet (SPRS590), the OMAP-L138/C6748
technical reference manual (SPRUH79), the C674x CPU and megamodule guides (SPRUFE8, SPRUFK5) and the
TLV320AIC3106 data sheet (SLAS509).

Most of what is known about the board comes from the SmplTrek reverse-engineering project,
[smpltrek-re](https://github.com/seawaffle/smpltrek-re). References to it name its bench sessions (M1, D1, ...: `experiments/sessions/` there), its
claims (HW-15, XF-21, ...: `claims.json`) and its documents (`docs/01-platform-map.md`,
`docs/04-hardware-access.md`, ...).

## Processor and memory

- **CPU:** TI TMS320C6748 (C674x DSP, floating point) at **372 MHz** (PLL0 SYSCLK1). Peripherals on the
  SYSCLK2 domain run at 186 MHz. The CPU time-stamp counter (TSCL) counts CPU cycles, 372,000 per ms;
  Enaga uses it for every delay and time-out.
- **DDR:** 128 MiB at `0xC0000000`-`0xC7FFFFFF`, cacheable.
- **On-chip:** L1P 32 KiB, L1D 32 KiB, L2 256 KiB. The original firmware configures all of L1P and L2 as
  cache and 16 KiB of L1D as cache (the other 16 KiB as RAM at `0x11F00000`). There is also 128 KiB of
  shared RAM at `0x80000000`.
- **SPI flash:** two 16 MiB NOR chips on SPI0. The first 256 KiB of chip 0 hold the bootloader and the next
  4 MiB the system image; the rest holds the original firmware's sounds and settings. Enaga does not write
  either chip, and does not read them yet. The original firmware and bootloader read them by EDMA (code reading,
  "Boot"); if Enaga does too, it must invalidate the destination range before the CPU reads it, since the DMA
  writes DDR behind the caches. Both chips answer JEDEC ID `C2 20 18` (Macronix, 16 MiB; smpltrek-re session M1, 2026-11-16, measured).
  Chip 0 reads status `0x0C` and configuration `0x0F`: status bits 2-3 set, block protect on Macronix parts
  (inference), so writes to chip 0 are refused until they are cleared, which would be a deliberate, risky step.
  Chip 1 reads status `0x00`, configuration `0x07`.
- **Null and reserved reads (measured, M1).** Reads of `0x0`-`0x14` and `0x00400000` do not fault and do not
  return 0: they return leftover data (most likely the previous load's 64-bit value, bytes chosen by address
  bits 2:0; inference), with EFR, IERR and the event flags clear. A null dereference fails silently with
  plausible garbage; Enaga cannot trap it.
- **Misaligned access (measured, M1, cached DDR).** No exception: LDDW rounds the address down to a multiple of
  8, LDW and STW to 4, so a misaligned 32-bit store overwrites the aligned word below. Packed formats (WAV
  headers, card data) need byte copies or unaligned-safe helpers.
- **EMIFA (measured, M1).** A store into the chip-select-0 window (`0x40000000` and up) with EMIFA not set up
  does not hang the unit: the data is dropped and reads return junk. Nothing is attached there.
- **USB0 (measured, M1).** The original firmware's bring-up works: CFGCHIP2 = `0x0972`, PSC1 module 1, CTRLR =
  1, then CFGCHIP2 bits 15, 10, 9 cleared and bit 6 set; USB0PHYCLKGD (bit 17) comes up within 10 ms and
  CFGCHIP2 then reads `0x00020972`. The vendor driver's reserved registers `0x01E0080C` and `0x01E0084C` read 0,
  and writing 0 to `0x01E00808` and `0x01E00848` is harmless.

## Boot

The bootloader (in SPI flash, never modified by Enaga) copies the system image's sections to their load
addresses and branches to its entry point. At that moment the PLLs and DDR are configured, the
power hold (GP4[11]) is already driven high, and the OLED shows the SONICWARE logo. The bootloader runs
from `0xC7020000`-`0xC70B0000` while loading, so an image must not load there; the original firmware loads
from `0xC0000000`, which is where Enaga loads too. A system image that never takes over leaves the unit on,
showing the logo, with the power key dead: remove the batteries or power to recover, then use BOOT MODE.

**Cache state at entry (measured on a unit, smpltrek-re session M1, 2026-11-16; it matches the earlier static trace
and simulation of BOOT 1.0.17's cache start-up).** At the jump L2CFG = 0 (L2 all RAM, no L2 cache), L1PCFG = 4 (32 KiB
L1P cache), L1DCFG = 3 (16 KiB L1D cache, 16 KiB L1D RAM at `0x11F00000`), and MAR 17, 128, 192-195 and 199
cacheable, MAR 196-198 (`0xC4000000`-`0xC6FFFFFF`) not. The bootloader does not write back or invalidate any
cache before branching, and it uses neither L2 nor L1D as RAM (its code, data and stack are in DDR at
`0xC7xxxxxx`). The original firmware's configuration under "Processor and memory" is set by its own start-up,
not by the bootloader.

**How the image reaches DDR (code reading, smpltrek-re).** The flash is read by EDMA, not by the CPU: receive on
event 14 from SPIBUF, transmit on event 15 into SPIDAT1, SPIINT0 DMAREQEN, completion interrupt on the receive
channel; the SPI driver does no cache maintenance. So the system image is written to DDR without passing through
the CPU's caches, and with no L2 cache at the jump nothing of it is held in L2: Enaga's image is in DDR when it
starts. Lines the caches hold from before (L1P, L1D) are still not to be trusted, which is why `start.s`
invalidates L1P and writes back and invalidates L1D first.

So Enaga trusts none of it. The first instructions of `start.s` record L2CFG, L1PCFG, L1DCFG and MAR 192-199,
write back and invalidate all of L1D and invalidate L1P; `cache_setup` (`hal/cache.c`) then makes L2, L1P and
L1D all cache and writes every MAR from 16 to 255 (0-15 are read-only): cacheable for DDR (192-199), not
cacheable everywhere else. The log's second line shows both states: L2, L1P and L1D modes (4 = all cache,
3 = 16 KiB, 0 = none) and the PC bits of MAR 192-199 in hex (bit n = MAR 192+n), as recorded at entry and
after set-up. The unit's hand-over gives `CACHE 043 8F>444 FF`, and so does the emulator, which hands over
the measured state.

## Installing and recovery

- An update file is a `cmtd` container holding one `mmtd` system image (format and CRC seeds in
  `tools/sysimage.py`); `firmware/build.py` produces `build/firmware/<device>/<card name>` for each device and
  `build/firmware/ENAGA_<version>.bin` (the SmplTrek's).
- **Install:** copy the file to the root of the SD card named `SmplTrek.bin`, hold **F4** (printed HOME on a SmplTrek)
  while switching on to reach BOOT MODE, and choose SYSTEM UPDATE. The bootloader checks the file, writes it to flash,
  verifies it and asks for a restart.
- **Recovery:** the same procedure with an official SmplTrek system update restores the original firmware.
  Because Enaga never touches the bootloader or the sound content in flash, that one step is enough.
- **Lofi-12 XT and CyDrums:** the same board (smpltrek-re README: C6748 mezzanine, panel MCU, key matrix; panels
  physically the same, printed differently). Their update files differ in the family word (SmplTrek 5,
  Lofi-12 XT 12, CyDrums 8), the two CRC seeds and the name on the card (`Lofi-12 XT.bin`, `CyDrums.bin`);
  `tools/sysimage.py` holds them, checked against the official Lofi-12 XT 1.2.179 / 1.5.205 and CyDrums 1.1.84
  files (family word, both CRCs). The update chord is the same key, printed PAD on both (CyDrums manual: PAD +
  power). **Never installed on either.** Unknown until one is: what their bootloaders (not dumped) leave set up
  (PLL and DDR clocks, cache, where the bootloader itself sits; Enaga loads at `0xC0000000`), whether their panel
  MCU firmware scales pads the same way (the stock code drives the same audio pins on all three, below).
- **Hardware use is the same in every product** (smpltrek-re `docs/01-platform-map.md` "Hardware use per product", XF-20 to
  XF-25, all 13 stock releases): no release of any of the three uses a pin, codec register, panel command or
  peripheral beyond what SmplTrek 3.2.174 uses. GP3[8] is the only jack detect, with no input or line-out
  detection anywhere; the headphone / speaker split is mechanical. Differences are product code: the block size
  (Lofi and CyDrums 48 frames against 64), the input-source model, pad pressure (Lofi), and AUTO POWER OFF's
  first step (20 min in Lofi and CyDrums 2.0.97).
- **Which device Enaga runs on:** each bootloader opens its update file by name, so the name is a constant string
  in it. BOOT 1.0.17 is still in DDR at the hand-over, with `SmplTrek.bin` at `0xC70A8BA0` (smpltrek-re flash dump,
  section loaded at `0xC70A6BC0`; in flash at boot-area offset `0x345E0`). Enaga looks for the three names in
  `0xC7000000`-`0xC7200000` before it loads an app (`firmware/kernel/device.c`). The family word is not a stored
  value there: the parser compares it as an immediate (`cmpeq 5,b4,b0` at `0xC7090CEC`), so it can't simply be
  read. The other two bootloaders are assumed to load in the same area; if one doesn't, the device shows as
  unknown, and reading the boot area of the SPI flash (offset 0, where the C6748 ROM boots from on every unit)
  is the fallback.

## Power

| Signal | Use |
|---|---|
| GP4[11] | power hold: high keeps the unit on; low switches it off |
| GP3[0] | power key, active low |
| GP3[7] | power source: high = batteries (**unconfirmed**) |
| panel channel 15 | battery voltage, reported by the panel MCU (below) |

Switching off, in the original firmware's order: `B2` to the panel MCU, DAC mute, `C2`, then GP4[11] low.

**Battery level** (`firmware/kernel/power.c`), as the original firmware computes it (smpltrek-re "Power
management"; its `battery_sample` run in c6sim from Enaga's side, 2026-11-21): the latest channel 15 reading once a
second, readings under 2000 ignored, eight to an average. The level (0-4) falls a step while the average is below
the falling threshold under it and rises a step only at or above the rising one over it; the first average sets
it outright. Raw thresholds per BATTERY setting (the stock BATTERY TYPE):

| Type | Falling (to 0, 1, 2, 3) | Rising (to 1, 2, 3, 4) |
|---|---|---|
| ALKALINE | 2630, 2830, 2980, 3180 | 2730, 2930, 3080, 3280 |
| NiMH | 2840, 2915, 2940, 2990 | 2855, 2930, 2955, 3005 |
| LITHIUM | 3030, 3280, 3360, 3490 | 3080, 3330, 3410, 3540 |

The voltages these raw values stand for are not measured; the original firmware only draws the level. Enaga
also switches off after 30 s at level 0 on batteries (closing the app's files first), which the original
firmware does not do.

## Keys

A 6 x 8 matrix: rows GP2[0..5] are outputs driven one high at a time; columns 0-5 are GP3[1..6], column 6
is GP3[9], column 7 is GP3[11]. One extra key, active low on GP5[10], appears as row 0, column 4. Key
number = row x 8 + column, forming a 48-bit switch word:

| Bit | Key | Bit | Key |
|---|---|---|---|
| 0 | knob B push | 19 | F1 (FUNC) |
| 1 | knob A push | 20 | F3 (PROJECT) |
| 2 | knob C push | 21 | F7 (MIXER) |
| 3 | knob D push | 22 | F5 (PLAY) |
| 4 | VALUE push | 23 | OK |
| 8 | selection button, left | 24 | left |
| 9 | selection button, right | 25 | down |
| 16 | octave left | 26 | F2 (REC) |
| 17 | up | 27 | F4 (HOME) |
| 18 | right | 28 | F8 (EDIT) |
| 29 | F6 (STOP) | 30 | CLR |
| 31 | octave right | 32-47 | STEP 1-16 |

Bits 5-7 and 10-15 have no switch. Enaga names the 2 x 4 function block left of OK / CLR F1-F8 in reading
order, because its printed labels differ between products on the same panel (SmplTrek labels in brackets above;
Lofi-12 XT and CyDrums print PTN, COPY, PAD, REC, Play, MIX, MUTE there, see smpltrek-re
`docs/10-cross-flash-button-map.md`):

| | 1st | 2nd | 3rd | 4th |
|---|---|---|---|---|
| upper row | F1 (19, FUNC) | F2 (26, REC) | F3 (20, PROJECT) | F4 (27, HOME) |
| lower row | F5 (22, PLAY) | F6 (29, STOP) | F7 (21, MIXER) | F8 (28, EDIT) |

F1 is func on all three products, and F4 is the key the SmplTrek bootloader tests for BOOT MODE. The names come from the original firmware's factory test page, which
draws the panel and fills in each button whose bit is set, matched to the manual's layout; they are still
to be confirmed key by key on a unit. The switches are not debounced in hardware. Bounce was measured on STEP 1
only (smpltrek-re session M1: 20 presses sampled every 10 µs): presses gave one edge, releases up to 7 edges over
175 µs, the longest settle time 784 µs, no glitches; a two-sample release filter at 6 ms is ample. Enaga follows the original firmware (`hal/keys.c`; smpltrek-re claim HW-51): each key is sampled every
6 ms, a press is taken on the first closed sample and a release on the second consecutive open one, so a
one-sample opening while a key is held is ignored. The original filters releases so only for bits 0-31; its
STEP keys release on the first open sample, so a one-sample opening gives a release and a new press. Enaga
filters every key. The original's auto-repeat (400 ms, then every 100 ms) is not reproduced.

## Encoders

Five quadrature encoders on GPIO: A GP3[14]/[15], B GP3[12]/[10], C GP5[2]/[0], D GP5[5]/[4], VALUE
GP5[7]/[8]. Both pins high is the rest position between detents; decoding as in `hal/encoders.c` gives one
count per detent.

## Panel MCU (pads, LEDs, battery)

A separate microcontroller scans the 15 pressure pads and the battery voltage and drives the LEDs. It talks
to the DSP over **UART1** at 465,000 baud, 8N1 (186 MHz / 16 / 25). The MCU sends continuously, touched or
not: about 4,192 `Bn` messages a second, all 16 channels at about 262 each, every 3.69-3.97 ms (smpltrek-re session
M1, measured). That is about 12,600 bytes a second, so the 16-byte receive FIFO fills in about 1.3 ms; M1's
image, which polled, lost bytes to FIFO overruns. Enaga takes them by the receive interrupt (CPU interrupt 6 <-
event 46) into a ring that the main loop parses, and the interrupt may nest in the app's audio callback;
`uart1_overruns` counts LSR OE. Bytes during start-up, before the interrupt is on, may still be lost.

| Direction | Bytes | Meaning |
|---|---|---|
| to MCU | `90`, `B1`, `A0` | start-up, 50 ms apart; `A0` asks for the `A1` reply and may need repeating |
| to MCU | `B2`, `C2` | power-off (see Power) |
| to MCU | `C1 a b c v` | set LED (a, b, c) to level v |
| from MCU | `Bn lo hi` | analogue channel n: 14-bit reading `lo \| hi << 7`; n = 0-14 pads, 15 battery |
| from MCU | `A1` + 6 bytes | MCU information; bytes 3-5 are its version |

LED addresses are a = 0-2, b = 0-5, c = 0-7 (144 addresses), written here as n = 48a + 8b + c; levels 0-10 are
usable (from 11 a neighbouring LED lights too). The original firmware treats a pad reading of 800 or more as a
press. Enaga does the same and gives apps press and release events besides the raw readings: a press at 800, a
release below 600, velocity 1-127 from the higher of the first two readings over 800 (127 from 3,300; "Pad readings"). The velocity scale is set
from session 1's readings; the release threshold is still provisional.

The 15 pads sit in two staggered rows, 7 above (TRK 1-7) and 8 below (TRK 8-10, GLOBAL 1-3, USB AUDIO, EXT SRC),
and their channels run left to right alternating lower and upper row: even channels 0-14 are the lower row,
odd channels 1-13 the upper (smpltrek-re session D1, `D1-hello.md`; the positions come from the panel drawings). The rows lie like
a piano's white and black keys, which the synth example uses: lower row C to C, upper row C#, D#, -, F#, G#, A#, -.

### LED map

From the original firmware (smpltrek-re `docs/04-hardware-access.md` "Panel LEDs"; the graded table of
all 144 addresses is printed by `analysis/led_table.py` there). It drives 115 addresses; the other 29 are
never written, by it or by Enaga: (0,0,6-7), (1,0,6-7), (2,0,6-7), (0,1,7), (1,1,7), (2,1,7) and (2,3,0) to
(2,5,3). Confirmation on a unit, per LED: **device** = every channel's address seen lighting it; **partly** =
the LED seen on one channel, the others inferred; **static** = from the code only. smpltrek-re's session L1, planned
to check the rest and the brightness steps, has not been run.

Pads, indexed by MCU pad channel (the id of a pad event); red, green and blue are v1, v2 and v3 of the original
firmware's `pad_rgb_set`:

| Pad | Label | Red | Green | Blue | Confirmed |
|---|---|---|---|---|---|
| 0 | TRK 8 | 2,1,5 | 2,1,4 | 2,1,6 | static |
| 1 | TRK 1 | 2,1,2 | 2,1,1 | 2,1,3 | static |
| 2 | TRK 9 | 0,1,5 | 0,1,4 | 0,1,6 | device |
| 3 | TRK 2 | 0,1,2 | 0,1,1 | 0,1,3 | device |
| 4 | TRK 10 | 1,1,5 | 1,1,4 | 1,1,6 | static |
| 5 | TRK 3 | 1,1,2 | 1,1,1 | 1,1,3 | static |
| 6 | GLOBAL 1 | 2,2,4 | 2,2,3 | 2,2,5 | static |
| 7 | TRK 4 | 0,2,7 | 0,2,6 | 0,1,0 | partly (blue) |
| 8 | GLOBAL 2 | 1,2,4 | 1,2,3 | 1,2,5 | static |
| 9 | TRK 5 | 1,2,7 | 1,2,6 | 1,1,0 | static |
| 10 | GLOBAL 3 | 0,2,4 | 0,2,3 | 0,2,5 | static |
| 11 | TRK 6 | 2,2,7 | 2,2,6 | 2,1,0 | static |
| 12 | USB AUDIO | 0,2,1 | 0,2,0 | 0,2,2 | partly (green) |
| 13 | TRK 7 | 2,2,1 | 2,2,0 | 2,2,2 | static |
| 14 | EXT SRC | 1,2,1 | 1,2,0 | 1,2,2 | static |

STEP 1-16 and ten buttons are two-channel LEDs: green at an even c (below), red at c + 1. The STEP order is the
panel wiring.

| STEP | Green | Confirmed | | STEP | Green | Confirmed |
|---|---|---|---|---|---|---|
| 1 | 1,3,0 | static | | 9 | 2,5,4 | static |
| 2 | 1,3,2 | static | | 10 | 0,3,0 | device |
| 3 | 0,3,2 | device | | 11 | 2,5,6 | static |
| 4 | 0,3,4 | partly (green) | | 12 | 1,3,4 | static |
| 5 | 0,3,6 | partly (green) | | 13 | 1,3,6 | static |
| 6 | 0,4,0 | partly (green) | | 14 | 1,4,0 | static |
| 7 | 1,4,2 | static | | 15 | 0,4,2 | partly (green) |
| 8 | 1,4,4 | static | | 16 | 0,4,4 | static |

| Button | Green | | Button | Green |
|---|---|---|---|---|
| F1 | 0,5,0 | | F2 | 1,5,0 |
| F4 | 0,5,2 | | F3 | 1,5,2 |
| F7 | 0,5,4 | | F8 | 1,5,4 |
| F6 | 0,5,6 | | F5 | 1,5,6 |
| CLR | 0,4,6 | | OK | 1,4,6 |

Every button LED is partly confirmed: its green address was seen on the unit (2026-11-14 sweep), red at c + 1
was not swept. The octave buttons have no LED.

Row 0 (b = 0) holds six one-channel LEDs, each written on all three a-planes with the same value, as the
original firmware does: c = 0 VOL L, 1 VOL R, 2 the microphone LED, 3-5 the VALUE encoder's blue, green and red.
Planes a = 0 and 1 were seen on the unit, a = 2 is inferred. The original firmware flashes VOL L, VOL R and the
microphone LED when the input feeding them peaks above -3 dBFS.

Enaga's API (`enaga_led` in `include/enaga/abi.h`, `enaga::pad_led`, `step_led`, `button_led`, `vol_led`,
`mic_led`, `value_led` in `app.hpp`) drives these by name; `kernel/leds.c` holds the address tables and sends
only changes.

## Display

A 128 x 128, 4-bit grey OLED on **SPI1** (9.3 MHz) with a data/command line on GP2[11]. The controller
behaves like an SSD1327 (part number **unconfirmed**). Initialisation commands (D/C low): `B3 F0`, `81 FF`,
`A0 07`, `AF`.

A frame is 8192 bytes sent with D/C high, about 7 ms. The panel's byte order is not row-major: the frame
is 64 blocks of 128 bytes, block `y >> 1` holding rows 2k and 2k+1; within a block the first 64 bytes are
the even columns and the next 64 the odd columns; the even row is the low nibble and the odd row the high
nibble:

    byte  = fb[128 * (y >> 1) + 64 * (x & 1) + (x >> 1)]
    shift = (y & 1) ? 4 : 0

No address commands are sent: the panel's write position wraps after each full frame, so every frame must
be sent whole. Enaga sends frames by EDMA (events 18 and 19, SPI1 receive and transmit).

## Pad readings (measured, Enaga session 1, 2026-11-18)

At rest every pad reads 1-2; a light press about 1,500 and a hard one about 3,400 (of the 14-bit range). Pressing a
pad raises some other pads' readings slightly (pad 0 -> 4, 1 -> 5, 4 -> 2; 2 and 3 none): not neighbours on the
panel, so probably in the sensing or the MCU's inputs. Readings rise over several 3.8 ms reports, so the first
report over a threshold says little about how hard the pad was hit. Enaga's press thresholds (the PADS setting,
1,600 / 1,000 / 600) were set by feel in session 4.

## Audio load with caches (measured, Enaga session 1)

With Enaga's cache set-up, a computed voice (the LOAD page's SYNTH) costs about 1,870 cycles a 64-frame block, a
voice reading samples from DDR (SAMPLE, GRAIN) about 2,600-2,700; Enaga's own share with no app is about 3 % of
the block. Details in docs/design.md, "First unit session: results".

## Audio

- **Codec:** TI TLV320AIC3106 on **I2C0** (address `0x18`, 100 kHz), configured by 40 register writes
  (`hal/codec.c`): codec is the clock master at 48 kHz, I2S with 32-bit slots, line inputs into both ADCs,
  both DACs to the line outputs.
- **Data:** **McASP0**, AXR13 transmits and AXR14 receives, 2-slot frames of 32-bit words, clocks from the
  codec. **EDMA3 channel 0** (McASP receive) and **channel 1** (transmit) move the samples; their
  completions are signalled in EDMA shadow region 1, which reaches the DSP as **event 8**. The pending
  register (IPR) has one bit per channel, so completions that arrive before it is cleared merge into one;
  the finished ping-pong buffer is the one the channel's PaRAM set does not point into (SRC for transmit,
  DST for receive), not the parity of a completion count. That rests on the EDMA3 documentation (the link
  reload happens when the last transfer request is issued, before IPR is set); **code reading**, not checked
  on a unit. Found in an early bring-up firmware of the smpltrek-re project (not in its published repository), where a 7 ms display frame let 96 blocks
  complete against 77 counted.
- **Channels:** with the original firmware's one-word-short first transfer, even buffer words land in slot
  1, the **left** channel: confirmed by ear on headphones (smpltrek-re session M1, image 0.3.1, which uses the same
  scheme with 256-word buffers): even words left only, odd words right only, both at the same level. The receive
  side follows the same order (INPUT L in IN L, INPUT R in IN R: Enaga session 1); the line outputs are not yet
  checked. With nothing connected, 0.3.1's input peak sat around 2 in 32,768.
- **Speaker:** plays with GP2[8] (speaker mute) driven low and headphones unplugged (M1). Enaga releases it unless
  the SPEAKER setting is OFF or the app mutes it (docs/design.md "Settings"). Headphones cut it in hardware (unit
  session 3, E3 check; no code involved, as smpltrek-re HW-15 inferred). GP3[8] is the only jack-sensing input the stock
  firmware reads (smpltrek-re `docs/04-hardware-access.md`); it mixes the input into the monitor only while GP3[8] reads high,
  i.e. with headphones in, which keeps the speaker from feeding back.
- **Audio GPIOs:**

| Pin | Use |
|---|---|
| GP2[9] | output mute, active high |
| GP2[8] | speaker mute, active high |
| GP4[10] | codec reset: low at start, released 1 ms before the first I2C write (**unconfirmed** as RESET) |
| GP4[14], GP4[15], GP4[12] | analogue power, switched on in that order 2 ms and 5 ms apart |
| GP1[2] | built-in microphone switch |
| GP7[14], GP7[15] | INPUT L front end and its +10 dB stage |
| GP5[11] | INPUT R microphone stage |
| GP1[3] | line input pad |
| GP4[13] | held high (role unknown) |
| GP3[8] | headphone detect: 1 with a plug in PHONES, 0 otherwise; INPUT L / R and the outputs do not change it (unit, E3 check) |

- **Input routes** (`audio_input` in `firmware/kernel/audio.c`; apps choose them with `enaga::system::input`).
  They copy the original firmware's REC SRC end states (smpltrek-re HW-17, HW-28, HW-29), read from its code
  and not yet checked on a unit route by route. Codec registers: 15 / 16 the ADC PGAs; 17 MIC3R into the left
  ADC; 19 LINE1L into the left ADC; 22 LINE1R into the right ADC; 21 / 24 the cross paths, never used. Slots:
  even words are the left ADC.

| Source, level | Codec | Pins | The app hears |
|---|---|---|---|
| MIC | 17 = 0xF0, 19 = 0xFC (left ADC on, LINE1L off), 22 = 0xF8 (right ADC off) | GP1[2] high, GP7[14] high | left ADC in both channels, only while GP3[8] is high |
| HIZ LOW / NORMAL / HIGH | 19 = 0x84, 22 = 0xF8; 15 = 0 / 0 / 20 (+10 dB) | GP7[14] low; GP7[15] low / high / high | left ADC in both |
| DYNAMIC_MIC LOW / NORMAL / HIGH | 19 = 0xF8, 22 = 0x84; 16 = 0 / 0 / 42 (+21 dB) | GP5[11] low / high / high | right ADC in both |
| LINE_STEREO LOW / NORMAL, HIGH | 19 = 22 = 0x84 | GP1[3] low (PAD) / high | left and right |
| LINE_MONO LOW / NORMAL, HIGH | 19 = 0x84, 22 = 0xF8 | GP1[3] low (PAD) / high | left ADC in both |

Pins not named are left in the line state: GP1[2] low, GP7[14] high, GP7[15] low, GP5[11] low, GP1[3] high.
LINE_STEREO NORMAL is the start-up state. Stock LINE_MONO with the PAD powers both ADCs and reads both slots,
an artefact of its PAD code. Enaga keeps it mono, as Lofi-12 XT's selector does.

## MIDI (DIN)

UART0 at `0x01C42000`, set up as the original firmware does (smpltrek-re HW-11):
- **Power and pins:** PSC0 module 9; PINMUX3 nibbles 4 and 5 = 2 (RXD, TXD).
- **Format:** MDR = 0 (16x), divisor 372, so 186 MHz / 16 / 372 = 31,250 baud. LCR = 3 (8N1); FIFOs on, receive
  trigger 1.
- **Interrupt:** event 38 (UART0_INT) on CPU interrupt 7 (`firmware/hal/uart0.c`).

The one interrupt drains the receive FIFO into a 2 KiB ring. It also sends one byte per THRE interrupt from a
2 KiB queue, with a 16-byte real-time queue ahead of it. It may nest inside the app's audio callback, like UART1.
Parsing is in the main loop (`firmware/kernel/midi.c`, docs/api.md "MIDI").

Not yet on a unit: the jacks' wiring and the opto-isolator side are not visible to code, so the first loop through
a cable is the check (checklist step 16).

## SYNC IN / SYNC OUT

GP7[8] (in) and GP7[9] (out), PINMUX17 nibbles 1 and 0 = 8, as the original firmware sets them (smpltrek-re
HW-27).
- **SYNC IN** takes one edge of GP7[8] through the GPIO bank 7 interrupt (event 72, CPU interrupt 9). Each edge's
  time stamp feeds the clock (`firmware/kernel/clock.c`). Which edge is the POL IN setting, as the original
  firmware's POLARITY IN: RISE (the default, value 0) arms the pin's *falling* edge, FALL its rising edge. So the
  board presumably inverts SYNC IN between the jack and the pin, making RISE the jack's rising edge. Enaga keeps the
  stock meaning of each label. (Before E5 Enaga armed the pin's rising edge whatever the setting.) The tempo does
  not depend on the edge; only the phase does, by the width of the other device's pulse.
- **SYNC OUT** pulses for 15 ms, at least 60 ms apart, as the original firmware. POL OUT RISE (the default) idles
  the pin low and pulses it high; FALL idles high and pulses low.
- **Not visible to code:** jack wiring and signal levels. The first unit session checks both directions with
  other gear (checklist step 17).

## USB

USB0 at `0x01E00000`: a Mentor MUSB core at +0x400 behind TI's wrapper (SPRUH79C chapter 33). Enaga runs it as a
full-speed USB MIDI, audio and mass storage device with TinyUSB (`firmware/third_party/tinyusb`). The C6748 port is
`firmware/usb/musb_enaga.h` and the glue `firmware/usb/usb.c`.
- **Unplugging:** the core's DISCON interrupt (the session ended) now tells TinyUSB the device is unplugged;
  TinyUSB left it unhandled, so the device stayed configured in its eyes until the next bus reset.
- **Audio (E5):** USB Audio Class 1.0, as the original firmware: 2 channels of 16 bits at 48 kHz each way,
  isochronous endpoints 0x03 (OUT, adaptive) and 0x83 (IN, asynchronous), 196 bytes, interfaces 2 and 3 under the
  AudioControl interface that also holds MIDI (`firmware/usb/usb_audio.c`). Packets are moved in the USB interrupt
  on every SOF, not through TinyUSB's transfers (which finish in the main loop): the SOF interrupt is on while a
  stream runs (TinyUSB's `dcd_sof_enable` was empty for MUSB), POWER.ISOUPDATE holds an IN packet until the SOF,
  both halves of the double-buffered IN FIFO are kept loaded so a SOF served late (behind the app's callback) costs
  nothing, and the endpoints' own interrupts stay off. Clock drift: what the computer sends is resampled (linear
  interpolation) by a ratio that keeps its ring at 3 ms; what goes to the computer is sent 47, 48 or 49 frames a
  packet to keep its ring at two blocks. The original firmware trims the codec PLL from TIMER64P1 instead
  (HW-18, HW-40). Tested on the emulator's isochronous streams, including a computer clock 1000 ppm fast and slow.
- **Mass storage (USB DRIVE, E5):** bulk-only transport, SCSI, one removable drive that is the whole SD card,
  16 sectors per SD command. No medium except while the menu's USB DRIVE page is open; then UNIT ATTENTION
  (medium changed) and the card. START STOP UNIT with START clear (eject) ends the page; so does OK on the page,
  once the computer has written nothing for 2 s (OK again ends it at once). The size comes from the
  card's CSD (CMD9, read during `sd_init`; CSD 1.0 and 2.0), which the emulator's SDSC card exercises; an SDHC card
  is first read on the unit.
- **Bring-up:** as the original firmware (smpltrek-re HW-12):
  1. CFGCHIP2 = 0x0972: 24 MHz reference, PHY clock from AUXCLK, PHY PLL on, VBUS and session comparators on.
  2. PSC1 module 1 on.
  3. CTRLR reset.
  4. Wait for USB0PHYCLKGD (CFGCHIP2 bit 17; within 10 ms on the unit, HW-59).
  5. Wrapper masks 0x01FF1E1F.
  6. POWER.HSENAB off (it is on after the reset; the original firmware writes POWER = 0x40). Left on, the core
     chirps and a computer runs it at high speed, where the full-speed descriptors are wrong (unit session 6).
  7. SOFTCONN.
- **Interrupts:** event 19 on CPU interrupt 10. Status comes from the wrapper with CTRLR.UINT = 0: INTMASKEDR,
  cleared through INTCLRR, then a write of 0 to EOIR. The core's own INTRTX / INTRRX / INTRUSB must not be read in
  this mode, since a read clears them (SPRUH79C 33.2.8.13.1, 33.4.9, 33.4.23-33.4.27). TinyUSB is patched to
  take its status from the port.
- **FIFOs:** dynamic, in the core's 4 KiB. The core has no RX / TX_DOUBLE_PACKET_DISABLE registers (0x340 / 0x342,
  found on newer MUSB revisions); double buffering is the DPB bit of TXFIFOSZ / RXFIFOSZ.
- **The debug port:** the MIDI interface has two cables. Cable 0 is MIDI; cable 1 ("Enaga Debug"; on Windows'
  older MIDI stack "MIDIIN2 (...)" / "MIDIOUT2 (...)") carries Enaga's log, its state, the screen and memory words
  as SysEx, for `tools/usb_debug.py` (`firmware/usb/usb_debug.c` has the protocol). It sends nothing until asked,
  and fills at most 128 bytes of the MIDI IN buffer, so MIDI waits behind no more than four packets of it (two in
  the buffer, two in the FIFO). During a
  session on a unit:

  ```
  pip install python-rtmidi
  python3 -B tools/usb_debug.py log --out session.log    # Enaga's log as it happens (Ctrl-C ends it)
  python3 -B tools/usb_debug.py status                    # version, app, crash, audio, USB and SD counters
  python3 -B tools/usb_debug.py screen shot.png           # the screen, in place of a photo
  python3 -B tools/usb_debug.py peek usb_drive_errors --elf build/firmware/enaga.elf
  python3 -B tools/usb_debug.py press F1 POWER            # keys from the computer (POWER only with F1: the menu)
  python3 -B tools/usb_debug.py turn VALUE 3              # a knob, by detents
  python3 -B tools/usb_debug.py pad 5 --velocity 90       # a pad
  ```

  Keys, knobs and pads from the computer are delivered as the unit's own (pads as press, pressure and release,
  without raw readings), and each answers once a frame has been drawn after it, so a `screen` next shows its
  effect. While they are in use a small square shows at the top right of the display (not in `screen`'s image);
  pressing or turning anything on the unit lets go of everything the computer held, as do 5 s without any request
  from the computer (a script holding longer uses `Client.wait`, which asks HELLO each second) and USB going. A
  computer cannot switch the unit off. `peek` reads RAM, the USB0 wrapper's registers and SYSCFG0, not the MUSB core's (reading its interrupt
  registers clears them). The port needs the firmware the tool came with: an older firmware has one cable.
- **Endpoints and IDs:** the MIDI interface uses bulk endpoints 0x01 and 0x81 (64 bytes), audio isochronous
  0x03 and 0x83 (196 bytes), the mass storage interface (USB DRIVE, interface 4) 0x02 and 0x82. The USB IDs are
  TinyUSB's test values (0xCAFE / 0x4011) until a pid.codes product ID is granted.

Tested on the emulator's USB model and scripted host (enumeration, MIDI both ways, clock, unplugging), and on the
unit in session 5 (0.2.0): Windows lists it as "Enaga SmplTrek"; through usbipd and pyusb from WSL, channel
messages and SysEx (one transfer or split across transfers, packets ending CIN 5, 6 and 7) arrive with `U`, STEP 1-4
reach the host (START, 263 clocks in about 5.5 s, STOP), the MIDI IN / MIDI OUT settings at DIN stop each
direction, and after unplugging and replugging under CHECK it enumerates again and carries MIDI both ways.

## SD card

**MMCSD0**, pins on PINMUX10; card detect on GP4[0] (low = card present); GP4[1] is driven low. 399 kHz
for identification, then 23 MHz with a 4-bit bus. SDSC and SDHC cards are handled (`hal/mmcsd.c`).
MMCST0 clears on read, and an error (TOUTRD, TOUTRS, CRCWR, CRCRD, CRCRS) can come in the same read as
the DATDNE, DRRDY or DXRDY being waited for, so a data-phase wait fails when any error bit came with its flag;
otherwise a write the card rejected looks like a success (found in an early bring-up firmware of the smpltrek-re project, not in its published
repository; the emulator injects no SD errors, so this path is untested).

**Writes without DMA (unit session 2):** put 64 bytes in the FIFO (write direction, 64-byte level) *before*
CMD24, send CMD24 **without DCLR**, then 64 bytes at each DXRDY (waiting for MMCST1 FIFOEMP works too); Linux's
davinci_mmc does the same. The controller holds a write command until its FIFO has data, so the command first
(as the stock firmware does, with DMATRIG and EDMA filling the FIFO) gets no response; and with DCLR after
priming the controller leaves the transfer without DATDNE. After either failure the card answers nothing,
not even CMD0, until it is taken out; GP4[1] is not its power switch. One sector takes 1-3 ms. Multi-block
transfers (CMD18, CMD25, then CMD12) use the same sequence with MMCNBLK set; the boot probe checks CMD25.

## Interrupts and exceptions

The C674x interrupt controller routes events to CPU interrupts 4-15 through INTMUX1-3. Enaga uses CPU
interrupt 4 for event 4 (TIMER64P0 TINT12, the watchdog tick; the original firmware's clock tick is the same
event), CPU interrupt 6 for event 46 (UART1, the panel MCU), CPU interrupt 7 for event 38 (UART0, MIDI) and CPU
interrupt 8 for event 8 (EDMA3 CC0 region 1 completion). Exceptions (internal and software) are enabled
with TSR.GEE and enter through the NMI vector; the EFR and IERR bit values follow TI's SYS/BIOS c64p
exception module.

## EDMA state at hand-over

The bootloader copies the system image by EDMA and leaves EDMA3 CC0 powered with completion state behind: on the
first Enaga run (2026-11-18) a channel was left pending with its interrupt enabled in a way that made every IEVAL
re-raise shadow region 1's interrupt (measured indirectly: 658,339 interrupts in a second). Which channel and
region are not known. A firmware should clear IER and IPR (IECR/IECRH, ICR/ICRH) before enabling its own EDMA
interrupts.

## Power and clocks

Peripherals are powered through the two power and sleep controllers (PSC0 at `0x01C10000`, PSC1 at
`0x01E27000`) before use: PSC0 module 0 and 1 (EDMA3 CC0 and TC0), 5 (MMCSD0); PSC1 module 7 (McASP0), 10
(SPI1), 12 (UART1). I2C0 has no PSC module and runs from the 24 MHz oscillator. TIMER64P0 is believed to
count at 24 MHz (**unconfirmed**); Enaga gives it a 240,000-count period, 10 ms if so, for the watchdog
tick, and measures no time with it.

## Checks on a unit

Things only a unit can settle, in the order of a session. `python3 -B tools/unit_card.py` stages the files in
`build/unit-card/` and prints their SHA-256; `python3 -B tools/unit_dry_run.py` walks this list on the emulator
first (from the simulated cache hand-over) and saves the screen at each step in `build/unit-dry-run/`. The CHECK app is `tests/unit/check` (its header lists every
control); hold F4 (HOME) for one second to go to its next page. Images from E3's speaker change on play the speaker
unless headphones are in or SPEAKER is OFF (step 15); earlier images kept it muted. Report each result with what the screen showed.

1. **Install.** A card with only `SmplTrek.bin` (staged as `build/unit-card/install/SmplTrek.bin`) at its root; F4 (HOME) held while switching on, SYSTEM UPDATE,
   restart. Expected: the ENAGA splash with MCU, AUDIO and CARD, then Enaga's status page with `NO /ENAGA/APPS`,
   and `SMPLTREK` as the third log line (the device, from the bootloader's file name; `UNKNOWN DEVICE` means the
   name was not found where `firmware/kernel/device.c` looks). A unit stuck on the SONICWARE logo with the power key dead: remove power, then BOOT MODE and the official
   update ("Installing and recovery").
2. **Cache state at entry** (measured by session M1, "Boot"): the status page's second log line. Expected
   `CACHE 043 8F>444 FF`; anything else before the `>` means Enaga read the registers wrongly or the bootloader
   differs from M1's. After
   the `>` it must read `444 FF`.
3. **Audio engine with no app:** on the status page, AUDIO shows status 0 and a block count that keeps rising;
   LOAD shows the peak (per mille) and 0 overruns. Report both numbers after about a minute.
4. **Power off:** POWER held one second shows BYE and the unit switches off. Note whether a short press does
   anything (it should not).
5. **App start:** copy `app/ENAGA` to the card, switch on. Expected: the CHECK app's AUDIO page.
6. **Channels (AUDIO page):** STEP 1 gives 750 Hz in the **left** ear only, STEP 2 1500 Hz in the **right** only,
   STEP 3 750 Hz in both, STEP 4 off; VALUE changes the level. M1 confirmed the order on its own image; a swap
   here means Enaga's ring differs from it. If a line output can be connected, check it the same way.
7. **Input (AUDIO page):** a signal into INPUT L, then INPUT R (or the built-in microphone, if the input chain
   uses it) moves IN L / IN R (per mille of full scale). A swap means the receive side's channel belief ("Audio")
   is wrong. Report the readings with nothing connected, too.
   **Input routes** ("Input routes" under "Audio"): STEP 9-13 choose the source, B its level. The line under the
   meters shows the choice, and ERR if the codec did not answer. Report IN L / IN R for each:
   - **STEP 9, MIC:** speak near the unit. With headphones in, both meters move. Unplug them: both read about 0.
   - **STEP 10, HI-Z**, a guitar or a line signal into INPUT L. Both meters follow INPUT L; NORM reads louder
     than LOW and HIGH louder again. INPUT R does nothing.
   - **STEP 11, DYN MIC**, a dynamic mic (or a quiet line signal) into INPUT R. Both meters follow INPUT R; LOW,
     NORM, HIGH rise as above, NORM by about 28 dB on the manual's figures. INPUT L does nothing.
   - **STEP 12, LINE ST:** INPUT L moves IN L and INPUT R moves IN R. LOW (the PAD) is about 20 dB quieter than
     NORM.
   - **STEP 13, LINE MONO:** INPUT L moves both meters, and INPUT R does nothing. LOW is quieter, as above.
   Any meter moving for the wrong jack, or a level step that changes nothing, means that route's pins differ
   from the stock code's reading. Note which.
8. **Audio load with caches (LOAD page), the E1 exit measurement:** STEP 1 (SYNTH), then STEP 5, 6, 7, 8 (16, 64,
   128, 256 voices); wait two seconds at each and note LOAD (percent of the block budget), CYC/VOICE and
   FIT AT 90%, and whether CAP appears.
   Repeat with STEP 2 (SAMPLE) and STEP 3 (GRAIN). For comparison, the emulator (no caches, no bus latency) gives
   about 2,500 cycles per voice per block for each workload and fits about 177 voices; the unit's SAMPLE and
   GRAIN against SYNTH show what DDR access costs.
9. **Keys and LEDs (PANEL page):** press every key once, slowly. Each must show its name (as printed on the
   panel; F1-F8 for the 2 x 4 function block, "Keys") and PRESSES going up by exactly one per press, which is the bounce check; quick double presses must
   count two. A key with an LED lights it green while held. Report any key with the wrong name, a count that
   jumps, or the wrong or no LED.
10. **Encoders and pads (PANEL page):** each encoder one detent at a time: its number changes by one per detent,
    with the sign clockwise positive. A, B and C set VOL L, VOL R and the microphone LED (0-10); VALUE cycles the
    VALUE LED red, green, blue. PAD MSG/S should read about 3,930 with no pad touched (M1's rate without the
    battery channel); much less means bytes are lost. The PAD line shows the pad with the highest reading; each
    pad lights green by pressure. Note which pad lights for each (the "LED map" order), the RAW reading of
    untouched pads (any pad glowing at rest shows it), and the RAW range when pressed hard. The bottom line shows
    the last pad pressed: VEL its velocity, PRS its pressure. Hit a pad and keep resting on it: PRS stays 0. Then
    press harder and ease off: PRS should follow smoothly, reaching 127 only when pressed very hard. Report how
    it feels (too stiff, too soft, jumpy).
11. **Card speed (CARD page, the fourth):** STEP 1, then wait for DONE (a few seconds; the card needs 4 MiB free).
    Report WRITE, READ, SCATTER (KiB/s and READS/S) and STREAMS, the card's make, size and format (FAT32 or
    exFAT), and run it twice. These are the rates an app gets through Enaga's file service, and they size
    streaming (design.md "Files"). A failure stops CHECK with CARD TEST FAILED on Enaga's status page: report
    its log lines (`CARD <reason>`, and from Enaga `SD ERR <code> <word>` with the SD driver's details). Lines
    `SD SLOW <n> MS` (the slowest one-sector write, when over 50 ms) are worth a photo either way. The test file
    `/RECORD/CHECK.BIN` is removed in both cases.
12. **Synth (optional, if time allows):** switch off, replace the card's `ENAGA` folder with `synth/ENAGA`, switch
    on. Expected: the synth's OSC 1 page with INIT at the top left. Pads play notes (lower row white keys, upper
    row black), STEP 1-16 chromatic notes; F2-F8 change pages; F1 + F2 opens PATCH, where VALUE chooses a patch and
    OK loads it. Choose U1 (after the factory patches), press F1 + OK: SAVED U1. Switch off and on: the same sound
    comes back, with U1 INIT at the top left. Report the voices and load at the top right (V and percent of the audio budget; the emulator
    gives at most 30 % with every feature on) for: one note; eight notes with INIT; SUPERSAW holding a chord;
    and any crackle or dropout. Note how the pads feel: whether soft hits
    sound and hard hits are loud (velocity thresholds are provisional, "Pads").

13. **Menu and switching (E3's exit):** with CHECK and SYNTH both on the card (copy `synth/ENAGA/APPS/SYNTH` into
    the card's `ENAGA/APPS`), switch on. Expected: the last app (CHECK), or the menu if none is recorded. Hold F1 and
    press POWER: the menu (the apps by name, each with an icon, then SETTINGS and STATUS; ON marks the running one;
    note whether the icons read clearly). DOWN to SYNTH, OK: the synth starts.
    F1 + POWER, UP to CHECK, OK: CHECK starts again. Switch back and forth at least ten times, playing a note in the
    synth each time; report any hang, crash screen, wrong app, or sound that stays after switching. Then: F1 + POWER
    with a synth note held (the note should stop when the menu opens, F1 + POWER again to resume); switch off and on
    (the last app comes back); hold F1 while switching on (the menu, `F1 HELD: MENU`; note whether the bootloader
    still starts normally with F1 held). Listen for a click when switching away from the synth with a note
    sounding (it should fade out).
14. **Settings:** F1 + POWER, SETTINGS, OK. With the synth playing behind the menu, VOLUME down with LEFT (quieter
    each step, silent at 0) and back up; BRIGHTNESS down to 1 and back (report how the steps look); PADS to LOW
    and HIGH (a light pad touch should play on HIGH and not on LOW; leave it at MEDIUM); AUTO OFF to 5 MIN. OK, then
    switch off and on: the settings are kept (the card's `/ENAGA/SETTINGS.TXT`). Leave the unit untouched for 5
    minutes: it should switch itself off. Set AUTO OFF back to OFF afterwards.
15. **Speaker and headphones:** with headphones unplugged, play the synth: the speaker should play. Plug headphones in: the
    speaker should stop and the headphones play. SETTINGS, SPEAKER OFF: the speaker stays silent unplugged; back to
    ON. Then open STATUS: `H` (right end of the AUDIO line) is 1 with headphones in and 0 without, whatever else
    is plugged in (first unit result: 0 / 1 / 0 for nothing, headphones, a plug in any other jack).
16. **MIDI (CHECK, MIDI page, the fifth):** with a MIDI keyboard or interface:
    - **In:** connect its MIDI OUT to the unit's MIDI IN. Play notes and turn a knob. IN counts up, and the last
      messages show as hex (`90 3C 64` is note on, key 60, velocity 100; a release shows as `80 3C 00` even if the
      keyboard sends a note on with velocity 0). A SysEx from it (a patch dump, say) shows its length on the
      SYSEX line.
    - **Out:** connect the unit's MIDI OUT to its MIDI IN, or to a computer's MIDI monitor. STEP 1 held is a note
      (key 60) on the channel shown; STEP 2 sends CC 7 = 100; STEP 3 an identity request; STEP 4 starts a 120 BPM
      clock (START, then 24 clocks a beat) and stops it (STOP). A synth set to follow MIDI clock should run at 120.
    - **Loop:** a cable from the unit's MIDI OUT to its own MIDI IN. Each STEP 1-3 press counts on both IN and OUT,
      and the messages list what was sent; the clock counts IN at about 48 a second.
    - **Settings:** SETTINGS, MIDI CH to 2: CH on the MIDI page shows 2 and STEP 1 sends `91`. MIDI THRU ON, with
      the keyboard into MIDI IN and MIDI OUT to a second instrument or monitor: what the keyboard plays comes out
      too. Set both back.
17. **Clock and SYNC (CHECK, MIDI page; SETTINGS):** the CLK line shows the clock's source, tempo, RUN and the beat.
    - **Out:** SETTINGS, CLOCK OUT ON. On the MIDI page STEP 5 starts Enaga's clock at 120 BPM and stops it. A
      synth or drum machine following MIDI clock at the unit's MIDI OUT starts, runs at 120 and stops with it. A
      device on SYNC OUT (SYNC OUT is ON by default; a Volca or Pocket Operator, say) steps two pulses a beat.
    - **MIDI in:** SETTINGS, CLOCK SRC MIDI; start a sequencer sending clock into MIDI IN. CLK shows MIDI, its
      tempo (within 0.5 BPM) and RUN with the beat counting; it stops when the sequencer stops. Continuing from
      the middle of a song resumes at the right beat (Song Position Pointer).
    - **SYNC in:** CLOCK SRC SYNC, a device's sync out into SYNC IN. CLK shows its tempo and runs while it plays,
      stopping a moment after it stops. Try POL IN FALL as well: it should follow just the same (the tempo does
      not depend on the edge). If one setting does not follow at all, note which.
    - **SYNC polarity, with a scope or a second device that shows its input:** POL OUT FALL turns SYNC OUT's
      pulses upside down (idle high, 15 ms low); a device that steps on rising edges then steps 15 ms later, or
      not at all. Set POL OUT back to RISE. With a scope on SYNC IN's jack and a device pulsing into it, note
      whether the pulse at the jack is positive; the stock RISE label is taken to mean the jack's rising edge.
    Set CLOCK SRC back to INT and CLOCK OUT to OFF.
18. **USB MIDI (CHECK, MIDI page):** connect the unit to a computer with a USB cable. It should appear as a MIDI
    device named "Enaga SmplTrek" with no driver (macOS: Audio MIDI Setup, MIDI window; Windows: a DAW's MIDI
    settings or MIDI-OX; Linux: `aconnect -l`). Report if it does not appear, and what the computer says (on
    Linux, `dmesg` right after plugging in).
    - **In:** play notes from a DAW or a virtual keyboard to it. They show on the MIDI page with `U` at the end;
      DIN messages show `D`.
    - **Out:** STEP 1 to 3 arrive in a MIDI monitor on the computer. STEP 5 with CLOCK OUT ON gives a clock a DAW
      can follow.
    - **Settings:** MIDI IN to DIN, and USB notes stop showing; MIDI OUT to DIN, and the computer stops receiving.
      Set both back to BOTH.
    - **Unplug and plug back in** while CHECK runs: nothing should crash, and the device comes back.

19. **Battery (E5):** on batteries (GP3[7] high), open STATUS: `B` (right end of the LOAD line) shows the level, 0-4;
    on USB power it shows `-`. The menu shows a battery mark at the top right on batteries only. Each level change
    logs `BATTERY <level> <raw average>`: note the lines, the battery type and, if possible, the voltage across the
    batteries at each change, which ties the raw values to volts. Set SETTINGS, BATTERY to the type in the unit.
    With nearly flat batteries, if it gets that far: BATTERY EMPTY with a countdown at the bottom of the screen,
    then the unit switches off showing BATTERY EMPTY.
20. **USB DRIVE (E5):** with an app running and USB connected, F1 + POWER, choose USB DRIVE. The app stops and the
    page shows the drive's state. On the computer a drive appears (Windows: a drive letter; macOS: on the desktop;
    Linux: in the file manager). Note its size against the card's.
    - Copy a large file (100 MB or more) to it and time it; copy an app's package into `/ENAGA/APPS/<NAME>/`.
    - Eject it on the computer (Windows: Safely Remove / Eject; macOS: Eject; Linux: Eject or Power Off). The page
      should close by itself with USB DRIVE EJECTED and the menu list the new app. If it stays open after the
      eject, note which computer, and press OK.
    - Start the copied app. Open USB DRIVE again and check the large file is whole (compare it on the computer).
    - **OK without ejecting** (on macOS or Linux, which cache writes): rebuild the running app with a visible
      change, open USB DRIVE, copy its `APP.ENA` over the old one and press OK as soon as the copy looks done. The
      page should show WAITING FOR THE WRITES TO FINISH for a moment, then close with USB DRIVE ENDED; choosing
      the app runs the new build. Note how long the wait was, and whether the computer complains about the eject.
    - Pull the cable while the page is open: the menu comes back with USB UNPLUGGED.
    - USB MIDI still works alongside (step 18) before and after.
    Each of Windows, macOS and Linux if available. Report anything the computer says (on Linux, `dmesg`).
21. **USB audio (E5; CHECK, AUDIO page):** with USB connected, the computer should list an audio device "Enaga
    SmplTrek" for both playback and recording, with no driver (Windows: Sound settings; macOS: Audio MIDI Setup;
    Linux: `aplay -l` and `arecord -l`).
    - **Playback:** STEP 14 (USB) chooses the computer as the input. Play music from the computer to the unit: the
      IN L / IN R meters follow it. Stop: they fall to 0.
    - **Recording:** record from the unit in a DAW (or Audacity) while STEP 1-3 play the tones: the tones arrive,
      left only, right only, both. Listen for clicks over a minute or two.
    - **Both at once** for five minutes: no clicks or dropouts either way. Note the computer and its sample rate
      setting (48 kHz).
    - USB MIDI and USB DRIVE still work alongside. Unplug while playing: nothing crashes; plug back in and it plays.
    - **Heavy app:** record while the synth plays many voices (or CHECK's LOAD page at 64 voices): still no clicks.
      Each recording's end logs `USB AUDIO IN L<underruns> U<ring gaps>`, both 0 when it was clean.
22. **The UI toolkit and the newer screens (E5 steps 5-8).** Copy `widgets/ENAGA/APPS/WIDGETS` into the card's
    `/ENAGA/APPS`, with a few `.WAV` files in a `/SAMPLES` folder.
    - **Menu and SETTINGS:** a thin rule under each title, the current line on a lit bar (SETTINGS too, which used
      `>`), hints on the bottom lines. Note anything cut off or overlapping.
    - **WIDGETS:** F2-F8 are its pages. F6 LEVELS: a signal into INPUT moves the meters, with a peak mark that holds
      about a second; knob A moves PAN either side of a centre mark. F7 PADS: each pad lights its own cell (lower
      row C D E F G A B C, upper row C# D# F# G# A#), so the map matches the panel; note any cell that lights for
      the wrong pad. F8 FILES: OK into SAMPLES, the WAV files listed (folders first), OK on one shows it picked,
      CLR goes back up.
    - **Turning speed:** F3 EDIT, F1 for columns or not; knob D (ATTACK, 0-2000 MS): turned slowly it moves 1 a
      detent, spun quickly it moves much further (3 or 8 a detent). Is the speed-up comfortable, too eager or too
      timid? LEVEL (0-100) never speeds up.
    - **Font:** name a file on the card `KICK_(1).WAV`: on the FILES page it should read exactly that, the `_`,
      `(` and `)` drawn rather than `-` or `?`.
    - **Synth:** F1 + F2, PATCH: UP / DOWN move as well as VALUE, the loaded patch marked `>`; a saved patch shows
      SAVED U1 on a bar over the scope for two seconds. Pressure: hold a pad and press harder with a modulation
      slot set to PRESS (MOD page): it should respond only after the hit, not to the hit itself.

Afterwards the official update restores the unit ("Installing and recovery").
