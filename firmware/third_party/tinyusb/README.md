# TinyUSB (vendored subset)

TinyUSB 0.21.0 (released 2026-06-30), MIT licence (`LICENSE`), from
https://github.com/hathach/tinyusb/archive/refs/tags/0.21.0.tar.gz
(SHA-256 `b7cdf35c5ccefb0f61640aff94a732ab4ebcb21498201e4a161545891b23ba01`).

Kept: `src/tusb.c`, `tusb.h`, `tusb_option.h`, `src/common/`, `src/device/`, `src/osal/osal.h` and
`osal_none.h`, `src/class/midi/` (MIDI 1.0 device), `src/class/audio/audio.h` (the MIDI class uses its definitions), and `src/portable/mentor/musb/dcd_musb.c` and `musb_type.h`
(the Mentor MUSB device driver). Everything else (host stack, other classes, other ports, RTOS layers, examples)
is left out. Enaga's configuration and C6748 port are in `firmware/usb/`.

Local changes, each marked `Enaga:` in the source:

- `src/portable/mentor/musb/dcd_musb.c`: includes `musb_enaga.h` for `TUP_USBIP_MUSB_ENAGA`, and takes the
  interrupt status from the port's `musb_dcd_int_status` when the port defines it, and its re-check for packets
  that arrived meanwhile from `musb_dcd_int_pending` (the C6748's TI wrapper gathers the core's interrupts, and with
  CTRLR.UINT = 0 the core's own interrupt registers must not be read: SPRUH79C 33.4.23-33.4.27). It also skips the RX / TX_DOUBLE_PACKET_DISABLE registers (core 0x340 / 0x342)
  when the port defines `MUSB_CFG_NO_DOUBLE_PACKET_DISABLE`: the C6748's core has none (its double buffering is the
  DPB bit of TXFIFOSZ / RXFIFOSZ alone), and the emulator stops on the access.
- `src/class/midi/midi_device.c`: real-time bytes (F8-FF) written as a stream go out as CIN 0xF (single byte), as
  USB MIDI 1.0 table 4-1 has them; TinyUSB sent them as CIN 0x5 (single-byte system common).
