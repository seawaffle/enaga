#!/usr/bin/env python3
"""Walk the checklist for a session on a unit (docs/hardware.md "Checks on a unit") on the emulator.

    ENAGA_EMULATOR=/path/to/emulator python3 -B tools/unit_dry_run.py    # -> build/unit-dry-run/

Uses the files tools/unit_card.py stages, starts each run from BOOT 1.0.17's simulated cache state, and saves
the screen at each step (NN-name.png) with results.json: every check with what was expected and seen. The
emulator cannot show what only a unit can (the install, sound, the physical panel, real cache timing); those
stay for the session. A few minutes of host time; exits 1 if any check fails.
"""
import json
import math
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "tests"))
import paths  # noqa: E402
import unit_card  # noqa: E402
from emulate import Emulation  # noqa: E402
from test_e1 import E1, component, samples  # noqa: E402

OUT = paths.BUILD / "unit-dry-run"
MS = 372_000
BOOT = 300_000_000
BUDGET = 496_000
STEP1, F4 = 32, 27
GPIO45_OUT = 0x01E26064                    # GPIO banks 4/5 OUT_DATA; bank 4 is the low half
KEY_NAMES = {0: "PUSH B", 1: "PUSH A", 2: "PUSH C", 3: "PUSH D", 4: "PUSH VALUE", 8: "SELECT L", 9: "SELECT R",
             16: "OCTAVE L", 17: "UP", 18: "RIGHT", 19: "F1", 20: "F3", 21: "F7", 22: "F5", 23: "OK",
             24: "LEFT", 25: "DOWN", 26: "F2", 27: "F4", 28: "F8", 29: "F6", 30: "CLR", 31: "OCTAVE R",
             **{STEP1 + n: f"STEP {n + 1}" for n in range(16)}}
BUTTONS = dict(zip((27, 21, 26, 20, 28, 19, 29, 22, 30, 23), E1.LED_BUTTONS))   # key -> (a, b, c) of its green
ENCODER_PINS = [((3, 14), (3, 15)), ((3, 12), (3, 10)), ((5, 2), (5, 0)), ((5, 5), (5, 4)), ((5, 7), (5, 8))]
HANDOVER = {0x01840000: 0, 0x01840020: 4, 0x01840040: 3}
HANDOVER_MARS = {17, 128, 192, 193, 194, 195, 199}


def address(abc):
    a, b, c = abc
    return 48 * a + 8 * b + c


class DryRun:
    def __init__(self):
        self.checks, self.shots, self.step = [], [], None

    def check(self, name, expected, observed, ok=None):
        ok = expected == observed if ok is None else bool(ok)
        self.checks.append(dict(step=self.step, name=name, expected=expected, observed=observed, ok=ok))
        print(f"  {'ok  ' if ok else 'FAIL'} {name}: {observed}" + ("" if ok else f" (expected {expected})"))

    def shot(self, emu, name, caption):
        from display import png
        emu.run(120 * MS)                                       # past the next 50 ms redraw
        frame, _ = emu.cpu.oled_frame()
        file = f"{len(self.shots) + 1:02d}-{name}.png"
        (OUT / file).write_bytes(png(frame))
        self.shots.append(dict(step=self.step, file=file, caption=caption))

    def begin(self, step, title):
        self.step = step
        print(f"{step}. {title}")


def start(files, input_audio=False):
    emu = Emulation(card_files=files)
    for reg, value in HANDOVER.items():
        emu.cpu.write(reg, value.to_bytes(4, "little"))
    for n in range(16, 256):
        emu.cpu.write(0x01848000 + 4 * n, int(n in HANDOVER_MARS).to_bytes(4, "little"))
    for pins in ENCODER_PINS:                                   # encoders at rest: both pins high
        for bank, pin in pins:
            emu.cpu.gpio_input(bank, pin, True)
    if input_audio:                                             # slot 0: 1 kHz at 0.5, slot 1: 1 kHz at 0.25
        words = []
        for i in range(48):
            v = math.sin(2 * math.pi * i / 48)
            words += [int(0.5 * v * 2 ** 31), int(0.25 * v * 2 ** 31)]
        emu.cpu.audio_input(struct.pack(f"<{len(words)}i", *words), loop=True)
    return emu


def press(emu, key, hold_ms=30):
    emu.key(key, True)
    emu.run(hold_ms * MS)
    emu.key(key, False)
    emu.run(30 * MS)


def turn(emu, encoder, detents):
    (ba, pa), (bb, pb) = ENCODER_PINS[encoder]
    sequence = (2, 0, 1, 3) if detents > 0 else (1, 0, 2, 3)   # phase = pin A | pin B << 1; 3 is rest
    for _ in range(abs(detents)):
        for phase in sequence:
            emu.cpu.gpio_input(ba, pa, phase & 1)
            emu.cpu.gpio_input(bb, pb, phase >> 1 & 1)
            emu.run(3 * MS)


def mcu_traffic(emu, ms, pressed=None):
    """The panel MCU's continuous stream: every channel in turn, 4,192 messages a second (smpltrek-re session M1)."""
    pressed = pressed or {}
    for i in range(ms * 4192 // 1000):
        channel = i % 16
        value = pressed.get(channel, 0)
        emu.cpu.uart_receive(1, bytes([0xB0 | channel, value & 0x7F, value >> 7]))
        emu.run(372_000_000 // 4192)


def leds(emu):
    return emu.cpu.read(emu.names["led_level"], 144)


def channels(emu):
    emu.run(100 * MS)
    words = samples(emu)[-4096:]
    return words[0::2], words[1::2]


def main():
    if not paths.emulator_present():
        print("ENAGA_EMULATOR not set to the SmplTrek emulator directory", file=sys.stderr)
        return 2
    if unit_card.main():
        return 2
    OUT.mkdir(parents=True, exist_ok=True)
    for old in OUT.glob("*.png"):
        old.unlink()
    card = unit_card.OUT
    run = DryRun()

    with start({"/SmplTrek.bin": card / "install" / "SmplTrek.bin"}) as emu:
        run.begin(1, "Install-only card")
        run.check("emulator run", None, emu.run(BOOT)["error"])
        report = emu.report()
        run.check("status", "NO /ENAGA/APPS", report["app_status"])
        run.shot(emu, "status", "Status page, card with only SmplTrek.bin")
        run.begin(2, "Cache state at entry")
        run.check("cache log line", "CACHE 043 8F>444 FF", report["log"][1] if len(report["log"]) > 1 else None)
        run.begin(3, "Audio engine with no app (5 s here, a minute on the unit)")
        blocks = emu.report()["audio_blocks"]
        emu.run(5000 * MS)
        report = emu.report()
        run.check("audio status", 0, report["audio_status"])
        run.check("blocks in 5 s", "about 3750", report["audio_blocks"] - blocks,
                  abs(report["audio_blocks"] - blocks - 3750) < 40)
        run.check("overruns", 0, report["audio_overruns"])
        run.check("LOAD peak, per mille", "small", report["audio_cycles_peak"] * 1000 // BUDGET,
                  report["audio_cycles_peak"] < BUDGET // 10)
        run.shot(emu, "status-running", "Status page after 5 s: AUDIO and LOAD lines")
        run.begin(4, "Power off")
        emu.cpu.gpio_input(3, 0, False)                         # POWER down (active low)
        emu.run(1200 * MS)
        emu.cpu.gpio_input(3, 0, True)
        emu.run(100 * MS)
        hold = int.from_bytes(emu.cpu.read(GPIO45_OUT, 4), "little") >> 11 & 1
        run.check("power hold GP4[11] after POWER held 1.2 s", 0, hold)
        run.shot(emu, "bye", "After POWER held one second")

    files = {"/SmplTrek.bin": card / "install" / "SmplTrek.bin",
             "/ENAGA/APPS/CHECK/APP.ENA": card / "app" / "ENAGA" / "APPS" / "CHECK" / "APP.ENA"}
    with start(files, input_audio=True) as emu:
        run.begin(5, "App start")
        run.check("emulator run", None, emu.run(BOOT)["error"])
        report = emu.report()
        run.check("status", "APP RUNNING", report["app_status"])
        run.check("app log", ["check ready", "CHECK"], report["log"][-2:])
        run.shot(emu, "audio-page", "CHECK, AUDIO page at start")

        run.begin(6, "Channels")
        for step, name, period, side in ((0, "STEP 1: 750 Hz left only", 64, "L"), (1, "STEP 2: 1500 Hz right only", 32, "R"),
                                         (2, "STEP 3: 750 Hz both", 64, "LR"), (3, "STEP 4: off", None, "")):
            press(emu, STEP1 + step)
            left, right = channels(emu)
            level = [round(component(ch, period), 3) if period else 0.0 for ch in (left, right)]
            peak = [max(map(abs, ch)) / 2 ** 31 for ch in (left, right)]
            seen = "".join(s for s, lv, pk in zip("LR", level, peak) if lv > 0.2 and pk > 0.2)
            quiet = all(pk < 1e-3 for s, pk in zip("LR", peak) if s not in side)
            run.check(name, side or "silent", seen or "silent", seen == side and quiet)
            if step < 3:
                run.shot(emu, f"tone-{step + 1}", name)

        run.begin(7, "Input (slot 0 fed 1 kHz at 0.5, slot 1 at 0.25)")
        emu.run(1200 * MS)
        run.shot(emu, "input", "AUDIO page, slot 0 fed at 0.5 and slot 1 at 0.25 of full scale; slot 1 is believed "
                 "left (docs/hardware.md \"Audio\"), so IN L about 250 and IN R about 500")

        run.begin(8, "Audio load (LOAD page)")
        press(emu, F4, 1100)
        for mode, mode_name in enumerate(("SYNTH", "SAMPLE", "GRAIN")):
            press(emu, STEP1 + mode)
            for preset, voices in zip(range(4, 8), (16, 64, 128, 256)):
                press(emu, STEP1 + preset)
                overruns = emu.word("audio_overruns")
                emu.run(2100 * MS)
                last = emu.word("audio_cycles_last")
                capped = voices == 256
                run.check(f"{mode_name} {voices} voices: block cycles", "under 90 %" if capped else "any",
                          f"{last} ({last * 1000 // BUDGET} per mille)", not capped or last < 0.9 * BUDGET)
                if voices in (128, 256):
                    run.shot(emu, f"load-{mode_name.lower()}-{voices}", f"LOAD page, {mode_name}, {voices} voices")
                run.check(f"{mode_name} {voices} voices: overruns over 2 s", "at most 1 (the step)",
                          emu.word("audio_overruns") - overruns, emu.word("audio_overruns") - overruns <= 1)

        run.begin(9, "Keys and LEDs (PANEL page)")
        press(emu, F4, 1100)
        run.shot(emu, "panel-page", "PANEL page before any key (F4's hold is the last event)")
        for key, name in KEY_NAMES.items():
            emu.key(key, True)
            emu.run(30 * MS)
            levels = leds(emu)
            emu.key(key, False)
            emu.run(30 * MS)
            if key in BUTTONS or key >= STEP1:
                green = address(BUTTONS[key] if key in BUTTONS else E1.LED_STEPS[key - STEP1])
                run.check(f"{name}: LED green while held", 10, levels[green])
                run.check(f"{name}: LED off after release", 0, leds(emu)[green])
            else:
                lit = [n for n, v in enumerate(levels) if v not in (0, 0xFF)]   # 0xFF: never written
                run.check(f"{name}: no LED lit while held", [], lit)
            if name in ("PUSH VALUE", "F5", "STEP 16"):
                run.shot(emu, f"key-{name.lower().replace(' ', '-')}", f"After pressing {name} once")
        for _ in range(2):                                      # a quick double press counts two
            press(emu, STEP1, 20)
        run.shot(emu, "key-double", "After a quick double press of STEP 1: PRESSES up by two (it counts presses "
                 "on every page since the app started)")

        run.begin(10, "Encoders and pads (PANEL page)")
        for encoder, detents in ((0, 3), (0, -1), (1, 1), (2, 4), (3, 1), (3, -2), (4, 1)):
            turn(emu, encoder, detents)
        emu.run(30 * MS)
        levels = leds(emu)
        row0 = lambda c: [levels[48 * a + c] for a in range(3)]
        run.check("VOL L after A +3 -1", [2, 2, 2], row0(0))
        run.check("VOL R after B +1", [1, 1, 1], row0(1))
        run.check("MIC after C +4", [4, 4, 4], row0(2))
        run.check("VALUE LED after VALUE +1 (blue, green, red)", [0, 10, 0], [row0(3)[0], row0(4)[0], row0(5)[0]])
        run.shot(emu, "encoders", "PANEL page after A +3 -1, B +1, C +4, D +1 -2, VALUE +1: A 2 B 1 C 4 D -1 VALUE 1")
        messages, overruns = emu.word("pad_messages"), emu.word("uart1_overruns")
        mcu_traffic(emu, 2100)
        run.check("pad messages parsed in 2.1 s of MCU traffic", 8803, emu.word("pad_messages") - messages)
        run.shot(emu, "pads-rest", "PANEL page with the MCU's continuous traffic, no pad touched: PAD MSG/S about "
                 "3930 (15 pads; the battery channel is not delivered)")
        for pad in range(15):
            mcu_traffic(emu, 30, {pad: 16383})                        # pressed hard
            green = address(E1.LED_PADS[pad][1])
            run.check(f"pad {pad}: green at full pressure", 10, leds(emu)[green])
            if pad == 7:
                run.shot(emu, "pad-7", "PANEL page with pad 7 pressed hard: PAD 7 RAW 16383")
            mcu_traffic(emu, 30)
            run.check(f"pad {pad}: off at rest", 0, leds(emu)[green])
        run.check("no UART1 receive overruns", 0, emu.word("uart1_overruns") - overruns)
        report = emu.report()
        run.check("app still running", "APP RUNNING", report["app_status"])

    failed = [c for c in run.checks if not c["ok"]]
    (OUT / "results.json").write_text(json.dumps(dict(checks=run.checks, shots=run.shots), indent=1) + "\n")
    print(f"{len(run.checks) - len(failed)}/{len(run.checks)} checks passed; {len(run.shots)} screens in {OUT}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
