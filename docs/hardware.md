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
  either chip.

## Boot

The bootloader (in SPI flash, never modified by Enaga) copies the system image's sections to their load
addresses and branches to its entry point. At that moment the PLLs, DDR and caches are configured, the
power hold (GP4[11]) is already driven high, and the OLED shows the SONICWARE logo. The bootloader runs
from `0xC7020000`-`0xC70B0000` while loading, so an image must not load there; the original firmware loads
from `0xC0000000`, which is where Enaga loads too. A system image that never takes over leaves the unit on,
showing the logo, with the power key dead: remove the batteries or power to recover, then use BOOT MODE.

## Installing and recovery

- An update file is a `cmtd` container holding one `mmtd` system image (format and CRC seeds in
  `tools/sysimage.py`); `firmware/build.py` produces `build/firmware/ENAGA_<version>.bin`.
- **Install:** copy the file to the root of the SD card named `SmplTrek.bin`, hold **HOME** while switching
  on to reach BOOT MODE, and choose SYSTEM UPDATE. The bootloader checks the file, writes it to flash,
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
number = row x 8 + column, forming a 48-bit switch word. Names known so far:

| Bit | Key | Bit | Key |
|---|---|---|---|
| 19 | FUNC | 26 | REC |
| 20 | PROJECT | 27 | HOME |
| 21 | MIXER | 28 | EDIT |
| 22 | PLAY | 29 | STOP |
| 23 | OK | 31 | OCTAVE RIGHT |
| 24 | LEFT | 32-47 | STEP 1-16 |

The other bits are delivered to apps by number until they are identified. The switches are not debounced
in hardware.

## Encoders

Five quadrature encoders on GPIO: A GP3[14]/[15], B GP3[12]/[10], C GP5[2]/[0], D GP5[5]/[4], VALUE
GP5[7]/[8]. Both pins high is the rest position between detents; decoding as in `hal/encoders.c` gives one
count per detent.

## Panel MCU (pads, LEDs, battery)

A separate microcontroller scans the 15 pressure pads and the battery voltage and drives the LEDs. It talks
to the DSP over **UART1** at 465,000 baud, 8N1 (186 MHz / 16 / 25).

| Direction | Bytes | Meaning |
|---|---|---|
| to MCU | `90`, `B1`, `A0` | start-up, 50 ms apart; `A0` asks for the `A1` reply and may need repeating |
| to MCU | `B2`, `C2` | power-off (see Power) |
| to MCU | `C1 a b c v` | set LED (a, b, c) to level v |
| from MCU | `Bn lo hi` | analogue channel n: 14-bit reading `lo \| hi << 7`; n = 0-14 pads, 15 battery |
| from MCU | `A1` + 6 bytes | MCU information; bytes 3-5 are its version |

LED addresses are a = 0-2, b = 0-5, c = 0-7 (144 addresses); levels 0-10 are usable (from 11 a neighbouring
LED lights too). Which address is which physical LED is **unconfirmed**. The original firmware treats a pad
reading of 800 or more as a press.

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
  completions are signalled in EDMA shadow region 1, which reaches the DSP as **event 8**.
- **Channels:** with the original firmware's one-word-short first transfer, even buffer words land in slot
  1, believed to be the **left** channel (**unconfirmed** by ear).
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

## Interrupts and exceptions

The C674x interrupt controller routes events to CPU interrupts 4-15 through INTMUX1-3. Enaga uses CPU
interrupt 8 for event 8 (EDMA3 CC0 region 1 completion). Exceptions (internal and software) are enabled
with TSR.GEE and enter through the NMI vector; the EFR and IERR bit values follow TI's SYS/BIOS c64p
exception module.

## Power and clocks

Peripherals are powered through the two power and sleep controllers (PSC0 at `0x01C10000`, PSC1 at
`0x01E27000`) before use: PSC0 module 0 and 1 (EDMA3 CC0 and TC0), 5 (MMCSD0); PSC1 module 7 (McASP0), 10
(SPI1), 12 (UART1). I2C0 has no PSC module and runs from the 24 MHz oscillator. TIMER64P0 is believed to
count at 24 MHz (**unconfirmed**; Enaga only checks that it runs).
