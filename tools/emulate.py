#!/usr/bin/env python3
"""Run Enaga and an app on the SmplTrek emulator, with a private FAT card holding the app.

    ENAGA_EMULATOR=/path/to/emulator python3 -B tools/emulate.py examples/tone --key 32@300000000

Builds the firmware (firmware/build.py) and the app (tools/build_app.py), writes the package to
/ENAGA/APPS/<NAME>/APP.ENA on a FAT card image (4 MiB unless --card-mib says otherwise, with --file adding more),
boots the firmware as the bootloader would leave it, plays the scripted keys, knob turns and power-off, and
reports Enaga's state, log, audio statistics and the captured output. The last frame (frame.png) and audio
(audio.wav: stereo, 48 kHz, 32-bit samples, the codec's words as captured) are saved under build/emulate/, and
with --get, files from the card afterwards. See README.md, "Testing without a unit", for the emulator.

In Python, `Emulation` does the same step by step: run(cycles), key(key, down), turn(encoder, detents),
power_off() (the app's stop() runs, as when the unit is switched off) and card_file(path).
"""
import argparse
import importlib.util
import json
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENAGA = HERE.parent
sys.path.insert(0, str(HERE))

import build_app  # noqa: E402
import paths  # noqa: E402

# The emulator has its own `build` module; load the firmware's by path under a distinct name.
_spec = importlib.util.spec_from_file_location("enaga_firmware_build", ENAGA / "firmware" / "build.py")
firmware_build = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(firmware_build)

WORK = paths.BUILD
MS = 372_000                               # cycles in a millisecond at 372 MHz
# Each encoder's two quadrature pins (bank, pin): A-D, then VALUE. At rest both are high.
ENCODER_PINS = [((3, 14), (3, 15)), ((3, 12), (3, 10)), ((5, 2), (5, 0)), ((5, 5), (5, 4)), ((5, 7), (5, 8))]
POWER_PIN = (3, 0)                         # the POWER key, low while pressed


def turn(emu, encoder, detents, phase_ms=3):
    """Turns encoder 0-4 (A-D, VALUE) by `detents` (negative: anticlockwise), each detent four pin phases
    `phase_ms` apart, so 12 ms a detent: a quick turn. Runs the emulator meanwhile."""
    (ba, pa), (bb, pb) = ENCODER_PINS[encoder]
    sequence = (2, 0, 1, 3) if detents > 0 else (1, 0, 2, 3)   # phase = pin A | pin B << 1; 3 is rest
    for _ in range(abs(detents)):
        for phase in sequence:
            emu.cpu.gpio_input(ba, pa, phase & 1)
            emu.cpu.gpio_input(bb, pb, phase >> 1 & 1)
            emu.run(phase_ms * MS)


class Emulation:
    """Enaga firmware + optional app card on the C++ emulator core."""

    # What the bootloader leaves in DDR that Enaga reads: its update file's name (BOOT 1.0.17 on a SmplTrek has
    # "SmplTrek.bin" at 0xC70A8BA0; firmware/kernel/device.c). None leaves DDR empty, as no known bootloader.
    BOOTLOADER_NAME_AT = 0xC70A8BA0

    def __init__(self, app_dir=None, name=None, card_files=None, bootloader_file="SmplTrek.bin", card_mib=4):
        paths.use_emulator()
        from cpu import CPU
        from media import build_card
        self.firmware = firmware_build.build()
        self.names = self.firmware["symbols"]
        files = dict(card_files or {})
        if app_dir is not None:
            app_dir = Path(app_dir)
            name = (name or app_dir.name).upper()[:8]
            package = build_app.build(app_dir, WORK / "apps" / app_dir.resolve().name, name)["package"]
            local = WORK / "emulate" / f"{name}.ENA"
            local.parent.mkdir(parents=True, exist_ok=True)
            local.write_bytes(package)
            files[f"/ENAGA/APPS/{name}/APP.ENA"] = local
        card = None
        if files:
            card = build_card(WORK / "emulate" / "card.img", files, card_mib).read_bytes()
        self.cpu = CPU()
        self.cpu.enable_board(profile=1)
        self.cpu.map(0xC0000000, 0x8000000, 7)
        entry = self.cpu.load_elf(self.firmware["elf"], map_allocations=False)
        if bootloader_file is not None:
            self.cpu.write(self.BOOTLOADER_NAME_AT, bootloader_file.encode() + b"\0")
        self.cpu.reset(entry, return_address=None)
        if card is not None:
            self.cpu.sd_load(card)
        for pins in ENCODER_PINS:                 # the encoders at rest
            for bank, pin in pins:
                self.cpu.gpio_input(bank, pin, True)
        self.inputs = []

    def word(self, name, signed=False):
        value = int.from_bytes(self.cpu.read(self.names[name], 4), "little")
        return value - (1 << 32) if signed and value & 0x80000000 else value

    def text(self, name, size):
        return self.cpu.read(self.names[name], size).split(b"\0")[0].decode("ascii", "replace")

    def key(self, key, down):
        self.cpu.key_input(key, down)
        self.inputs.append(dict(cycles=self.cpu.state()["cycles"], key=key, down=bool(down)))

    def turn(self, encoder, detents, phase_ms=3):
        """Encoder 0-4 (A-D, VALUE) turned by `detents`; see turn() above."""
        turn(self, encoder, detents, phase_ms)
        self.inputs.append(dict(cycles=self.cpu.state()["cycles"], encoder=encoder, detents=detents))

    def power_off(self):
        """POWER held for 1.2 s, as when switching the unit off: Enaga calls the app's stop() (where apps save),
        shows BYE and switches off. Returns run()'s result; nothing runs after it."""
        self.cpu.gpio_input(*POWER_PIN, True)
        self.run(20 * MS)
        self.cpu.gpio_input(*POWER_PIN, False)
        self.inputs.append(dict(cycles=self.cpu.state()["cycles"], power_off=True))
        return self.run(1200 * MS)

    def card_file(self, path):
        """A file's bytes from the card as it is now (after power_off, say), or None if it is not there."""
        import card
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            image = Path(d) / "card.img"
            image.write_bytes(self.cpu.sd_read())
            return card.get(image, path)

    def run(self, cycles, slice_cycles=1_000_000):
        from cpu import CoreFault
        end = self.cpu.state()["cycles"] + cycles
        start, error = time.perf_counter(), None
        try:
            while self.cpu.state()["cycles"] < end:
                state = self.cpu.run(min(slice_cycles, end - self.cpu.state()["cycles"]))
                if state["status"] != "budget":
                    error = f"stopped: {state['status']} at {state['pc']:#010x}"
                    break
        except CoreFault as fault:
            error = str(fault)
        return dict(error=error, host_seconds=time.perf_counter() - start, cycles=self.cpu.state()["cycles"])

    def log(self):
        count = self.word("log_count")
        lines = self.cpu.read(self.names["log_lines"], 16 * 40)
        first = max(0, count - 16)
        return [lines[(i % 16) * 40:(i % 16) * 40 + 40].split(b"\0")[0].decode("ascii", "replace") for i in range(first, count)]

    def report(self):
        raw_audio, audio = self.cpu.audio_output()
        frame, oled = self.cpu.oled_frame()
        return dict(state=self.cpu.state(), app_status=self.text("app_status", 32), app_name=self.text("app_name", 24),
                    crash_reason=self.text("crash_reason", 48), app_running=bool(self.word("app_current")),
                    log=self.log(), audio_status=self.word("audio_status", signed=True),
                    audio_codec_errors=self.word("audio_codec_errors"), audio_blocks=self.word("audio_blocks"),
                    audio_overruns=self.word("audio_overruns"), audio_cycles_last=self.word("audio_cycles_last"),
                    audio_cycles_peak=self.word("audio_cycles_peak"), interrupts=self.cpu.lib.c6_interrupt_count(self.cpu.handle),
                    capture=audio, oled=oled, inputs=self.inputs)

    def close(self): self.cpu.close()
    def __enter__(self): return self
    def __exit__(self, *_): self.close()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("app_dir", type=Path, nargs="?")
    ap.add_argument("--cycles", type=int, default=400_000_000)
    ap.add_argument("--key", action="append", default=[], help="KEY@CYCLE press (released 20 M cycles later)")
    ap.add_argument("--turn", action="append", default=[],
                    help="ENCODER:DETENTS@CYCLE: encoder 0-4 (A-D, VALUE), detents negative anticlockwise")
    ap.add_argument("--power-off", type=lambda v: int(v, 0), metavar="CYCLE",
                    help="hold POWER at CYCLE: the app's stop() runs and the unit switches off (the run ends there)")
    ap.add_argument("--card-mib", type=int, default=4, help="card size: a power of two, 2-1024 MiB")
    ap.add_argument("--file", action="append", default=[], metavar="CARD_PATH=LOCAL",
                    help="put a local file on the card (8.3 names), as /SAMPLES/KICK.WAV=kick.wav")
    ap.add_argument("--get", action="append", default=[], metavar="CARD_PATH",
                    help="after the run, save this file from the card under build/emulate/card/")
    args = ap.parse_args(argv)
    events = []
    for item in args.key:
        key, at = item.split("@")
        events += [(int(at, 0), "key", (int(key, 0), True)), (int(at, 0) + 20_000_000, "key", (int(key, 0), False))]
    for item in args.turn:
        what, at = item.split("@")
        encoder, detents = what.split(":")
        events.append((int(at, 0), "turn", (int(encoder), int(detents))))
    if args.power_off is not None:
        events.append((args.power_off, "power_off", ()))
    files = dict(item.split("=", 1) for item in args.file)
    out = WORK / "emulate"
    with Emulation(args.app_dir, card_files={k: Path(v) for k, v in files.items()}, card_mib=args.card_mib) as emu:
        now = 0
        result = None
        for at, kind, data in sorted(events, key=lambda e: e[0]) + [(args.cycles, None, ())]:
            if at > now:
                result = emu.run(at - now)
                now = at
                if result["error"]:
                    break
            if kind == "key":
                emu.key(*data)
            elif kind == "turn":
                emu.turn(*data)
                now = emu.cpu.state()["cycles"]
            elif kind == "power_off":
                result = emu.power_off()
                break
        report = {**emu.report(), "run": result}
        out.mkdir(parents=True, exist_ok=True)
        frame, oled = emu.cpu.oled_frame()
        if oled["complete_frames"]:
            from display import png
            (out / "frame.png").write_bytes(png(frame))
        raw_audio, audio = emu.cpu.audio_output()
        if raw_audio:
            from audio_capture import stereo_wav
            wav, _ = stereo_wav(raw_audio, audio["first_capture_slot"])
            (out / "audio.wav").write_bytes(wav)
        for path in args.get:
            data = emu.card_file(path)
            local = out / "card" / path.lstrip("/")
            local.parent.mkdir(parents=True, exist_ok=True)
            if data is None:
                print(f"{path}: not on the card")
            else:
                local.write_bytes(data)
                print(f"{path}: {len(data)} bytes, saved as {local}")
        (out / "result.json").write_text(json.dumps(report, indent=2, sort_keys=True, default=str) + "\n")
        print(json.dumps({k: report[k] for k in ("app_status", "app_name", "crash_reason", "audio_status", "audio_blocks",
                                                 "audio_overruns", "audio_cycles_peak", "interrupts", "log")}, indent=1))
        print(f"run: {result}; report {out / 'result.json'}")
        return 1 if result and result["error"] else 0


if __name__ == "__main__":
    sys.exit(main())
