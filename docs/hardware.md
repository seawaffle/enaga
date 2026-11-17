# SmplTrek hardware notes for Enaga

What Enaga's drivers rely on, in one place. Register-level detail lives next to the code that uses it (the
comment at the top of each `firmware/hal/*.c` and `firmware/kernel/audio.c`); this document is the map.
Facts marked **unconfirmed** are believed from how the original firmware uses the hardware but have not
been checked directly on a unit. TI documents cited: the C6748 data sheet (SPRS590), the OMAP-L138/C6748
technical reference manual (SPRUH79), the C674x CPU and megamodule guides (SPRUFE8, SPRUFK5) and the
TLV320AIC3106 data sheet (SLAS509).

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
  writes DDR behind the caches. Both chips answer JEDEC ID `C2 20 18` (Macronix, 16 MiB; smpltrek session M1, 2026-11-16, measured).
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

**Cache state at entry (measured on a unit, smpltrek session M1, 2026-11-16; it matches the earlier static trace
and simulation of BOOT 1.0.17's cache start-up).** At the jump L2CFG = 0 (L2 all RAM, no L2 cache), L1PCFG = 4 (32 KiB
L1P cache), L1DCFG = 3 (16 KiB L1D cache, 16 KiB L1D RAM at `0x11F00000`), and MAR 17, 128, 192-195 and 199
cacheable, MAR 196-198 (`0xC4000000`-`0xC6FFFFFF`) not. The bootloader does not write back or invalidate any
cache before branching, and it uses neither L2 nor L1D as RAM (its code, data and stack are in DDR at
`0xC7xxxxxx`). The original firmware's configuration under "Processor and memory" is set by its own start-up,
not by the bootloader.

**How the image reaches DDR (code reading, smpltrek).** The flash is read by EDMA, not by the CPU: receive on
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
  `tools/sysimage.py`); `firmware/build.py` produces `build/firmware/ENAGA_<version>.bin`.
- **Install:** copy the file to the root of the SD card named `SmplTrek.bin`, hold **F4** (printed HOME on a SmplTrek)
  while switching on to reach BOOT MODE, and choose SYSTEM UPDATE. The bootloader checks the file, writes it to flash,
  verifies it and asks for a restart.
- **Recovery:** the same procedure with an official SmplTrek system update restores the original firmware.
  Because Enaga never touches the bootloader or the sound content in flash, that one step is enough.

## Power

| Signal | Use |
|---|---|
| GP4[11] | power hold: high keeps the unit on; low switches it off |
| GP3[0] | power key, active low |
| GP3[7] | power source: high = batteries (**unconfirmed**) |
| panel channel 15 | battery voltage, reported by the panel MCU (below) |

Switching off, in the original firmware's order: `B2` to the panel MCU, DAC mute, `C2`, then GP4[11] low.

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
Lofi-12 XT and CyDrums print PTN, COPY, PAD, REC, Play, MIX, MUTE there, see smpltrek
`sdk/docs/cross-flash-button-map.md`):

| | 1st | 2nd | 3rd | 4th |
|---|---|---|---|---|
| upper row | F1 (19, FUNC) | F2 (26, REC) | F3 (20, PROJECT) | F4 (27, HOME) |
| lower row | F5 (22, PLAY) | F6 (29, STOP) | F7 (21, MIXER) | F8 (28, EDIT) |

F1 is func on all three products, and F4 is the key the SmplTrek bootloader tests for BOOT MODE. The names come from the original firmware's factory test page, which
draws the panel and fills in each button whose bit is set, matched to the manual's layout; they are still
to be confirmed key by key on a unit. The switches are not debounced in hardware. Bounce was measured on STEP 1
only (smpltrek session M1: 20 presses sampled every 10 µs): presses gave one edge, releases up to 7 edges over
175 µs, the longest settle time 784 µs, no glitches; a two-sample release filter at 6 ms is ample. Enaga follows the original firmware (`hal/keys.c`; smpltrek claim HW-51): each key is sampled every
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
not: about 4,192 `Bn` messages a second, all 16 channels at about 262 each, every 3.69-3.97 ms (smpltrek session
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
press.

### LED map

From the original firmware (the smpltrek notes, `sdk/docs/hardware-access.md` "Panel LEDs"; the graded table of
all 144 addresses is printed by `sdk/analysis/led_table.py` there). It drives 115 addresses; the other 29 are
never written, by it or by Enaga: (0,0,6-7), (1,0,6-7), (2,0,6-7), (0,1,7), (1,1,7), (2,1,7) and (2,3,0) to
(2,5,3). Confirmation on a unit, per LED: **device** = every channel's address seen lighting it; **partly** =
the LED seen on one channel, the others inferred; **static** = from the code only. Unit session L1 (planned in
smpltrek) is to check the rest and the brightness steps.

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
  on a unit. Found in smpltrek's bring-up firmware (commit cdd968f), where a 7 ms display frame let 96 blocks
  complete against 77 counted.
- **Channels:** with the original firmware's one-word-short first transfer, even buffer words land in slot
  1, the **left** channel: confirmed by ear on headphones (smpltrek session M1, image 0.3.1, which uses the same
  scheme with 256-word buffers): even words left only, odd words right only, both at the same level. The line
  outputs and the receive side (which input lands in which words) are not yet checked; with nothing connected,
  0.3.1's input peak sat around 2 in 32,768.
- **Speaker:** plays with GP2[8] (speaker mute) driven low and headphones unplugged (M1). Enaga keeps it muted.
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
| GP3[8] | jack detect input (polarity unconfirmed) |

## SD card

**MMCSD0**, pins on PINMUX10; card detect on GP4[0] (low = card present); GP4[1] is driven low. 399 kHz
for identification, then 23 MHz with a 4-bit bus. SDSC and SDHC cards are handled (`hal/mmcsd.c`).
MMCST0 clears on read, and an error (TOUTRD, TOUTRS, CRCWR, CRCRD, CRCRS) can come in the same read as
the DATDNE, DRRDY or DXRDY being waited for, so a data-phase wait fails when any error bit came with its flag;
otherwise a write the card rejected looks like a success (found in smpltrek's bring-up firmware, commit
cdd968f; the emulator injects no SD errors, so this path is untested).

## Interrupts and exceptions

The C674x interrupt controller routes events to CPU interrupts 4-15 through INTMUX1-3. Enaga uses CPU
interrupt 4 for event 4 (TIMER64P0 TINT12, the watchdog tick; the original firmware's clock tick is the same
event) and CPU interrupt 8 for event 8 (EDMA3 CC0 region 1 completion). Exceptions (internal and software) are enabled
with TSR.GEE and enter through the NMI vector; the EFR and IERR bit values follow TI's SYS/BIOS c64p
exception module.

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
control); hold F4 (HOME) for one second to go to its next page. The speaker stays muted (GP2[9] low, GP2[8] high), so
listen on headphones or the line output. Report each result with what the screen showed.

1. **Install.** A card with only `install/SmplTrek.bin` at its root; F4 (HOME) held while switching on, SYSTEM UPDATE,
   restart. Expected: the ENAGA splash with MCU, AUDIO and CARD, then Enaga's status page with `NO /ENAGA/APPS`.
   A unit stuck on the SONICWARE logo with the power key dead: remove power, then BOOT MODE and the official
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
    untouched pads (any pad glowing at rest shows it), and the RAW range when pressed hard.

Afterwards the official update restores the unit ("Installing and recovery").
