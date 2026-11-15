#!/usr/bin/env python3
"""Run Enaga and an app on the SmplTrek emulator, with a private FAT card holding the app.

    ENAGA_EMULATOR=/path/to/emulator python3 -B tools/emulate.py examples/tone --key 32@300000000

Builds the firmware (firmware/build.py) and the app (tools/build_app.py), writes the package to
/ENAGA/APPS/<NAME>/APP.ENA on a 4 MiB card image, boots the firmware as the bootloader would leave it, and
reports Enaga's state, log, audio statistics and the captured output. The last frame (frame.png) and audio
(audio.wav) are saved under build/emulate/. See README.md, "Testing without a unit", for the emulator.
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


class Emulation:
    """Enaga firmware + optional app card on the C++ emulator core."""

    def __init__(self, app_dir=None, name=None, card_files=None):
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
            card = build_card(WORK / "emulate" / "card.img", files, 4).read_bytes()
        self.cpu = CPU()
        self.cpu.enable_board(profile=1)
        self.cpu.map(0xC0000000, 0x8000000, 7)
        entry = self.cpu.load_elf(self.firmware["elf"], map_allocations=False)
        self.cpu.reset(entry, return_address=None)
        if card is not None:
            self.cpu.sd_load(card)
        self.inputs = []

    def word(self, name, signed=False):
        value = int.from_bytes(self.cpu.read(self.names[name], 4), "little")
        return value - (1 << 32) if signed and value & 0x80000000 else value

    def text(self, name, size):
        return self.cpu.read(self.names[name], size).split(b"\0")[0].decode("ascii", "replace")

    def key(self, key, down):
        self.cpu.key_input(key, down)
        self.inputs.append(dict(cycles=self.cpu.state()["cycles"], key=key, down=bool(down)))

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
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("app_dir", type=Path, nargs="?")
    ap.add_argument("--cycles", type=int, default=400_000_000)
    ap.add_argument("--key", action="append", default=[], help="KEY@CYCLE press (released 20 M cycles later)")
    args = ap.parse_args(argv)
    events = []
    for item in args.key:
        key, at = item.split("@")
        events += [(int(at, 0), int(key, 0), True), (int(at, 0) + 20_000_000, int(key, 0), False)]
    out = WORK / "emulate"
    with Emulation(args.app_dir) as emu:
        now = 0
        result = None
        for at, key, down in sorted(events) + [(args.cycles, None, None)]:
            if at > now:
                result = emu.run(at - now)
                now = at
                if result["error"]:
                    break
            if key is not None:
                emu.key(key, down)
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
        (out / "result.json").write_text(json.dumps(report, indent=2, sort_keys=True, default=str) + "\n")
        print(json.dumps({k: report[k] for k in ("app_status", "app_name", "crash_reason", "audio_status", "audio_blocks",
                                                 "audio_overruns", "audio_cycles_peak", "interrupts", "log")}, indent=1))
        print(f"run: {result}; report {out / 'result.json'}")
        return 1 if result and result["error"] else 0


if __name__ == "__main__":
    sys.exit(main())
