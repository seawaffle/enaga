"""examples/sampler on the emulator: F1 opens the file browser on /SAMPLES, OK loads the WAV there onto the
selected pad (converted from 44.1 kHz), the waveform shows, the pad plays it at its pitch, knob C retunes it, and
START and END trim and reverse it. Its engine is also tested on the PC (examples/sampler/tests, test_host_test.py).

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest tests.test_sampler_example
"""
import math
import struct
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENAGA = HERE.parent
sys.path.insert(0, str(ENAGA / "tools"))
sys.path.insert(0, str(HERE))

import build_app  # noqa: E402
import paths  # noqa: E402
from test_e1 import samples  # noqa: E402

BOOT = 300_000_000
MS = 372_000
F1, OK = 19, 23
KNOB_A, KNOB_B, KNOB_C = 0, 1, 2
WORK = paths.BUILD / "sampler-example"


def tone_wav(seconds=0.5, rate=44100, hz=1000):
    """A mono 16-bit WAV: hz at half scale, then silence for as long again (so START can be heard to move)."""
    frames = int(rate * seconds)
    data = b"".join(struct.pack("<h", round(16383 * math.sin(2 * math.pi * hz * i / rate))) for i in range(frames))
    data += bytes(2 * frames)
    fmt = struct.pack("<HHIIHH", 1, 1, rate, rate * 2, 2, 16)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body


@unittest.skipUnless(paths.emulator_present() and build_app.tools_present(), "needs ENAGA_EMULATOR and the toolchain")
class SamplerExample(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from emulate import Emulation
        WORK.mkdir(parents=True, exist_ok=True)
        (WORK / "tone.wav").write_bytes(tone_wav())
        cls.emu = Emulation(ENAGA / "examples" / "sampler", card_files={"/SAMPLES/TONE.WAV": WORK / "tone.wav"})
        assert cls.emu.run(BOOT)["error"] is None

    @classmethod
    def tearDownClass(cls):
        cls.emu.close()

    def run_ms(self, ms):
        self.assertIsNone(self.emu.run(ms * MS)["error"])

    def press(self, key):
        self.emu.key(key, True)
        self.run_ms(20)
        self.emu.key(key, False)
        self.run_ms(20)

    def pad(self, pad, reading=6000):
        for value in (reading, reading, 0, 0):       # pressed (two reports), then released
            self.emu.cpu.uart_receive(1, bytes([0xB0 | pad, value & 0x7F, value >> 7]))
            self.run_ms(4)

    def grey(self, frame, x, y):
        from display import pixels
        return pixels(frame)[y * 128 + x] // 17

    def screen(self):
        self.run_ms(120)
        return self.emu.cpu.oled_frame()[0]

    def left(self, ms):
        return samples(self.emu)[0::2][-48 * ms:]

    def pitch(self, left):
        crossings = [i for i in range(1, len(left)) if left[i - 1] < 0 <= left[i]]
        return (len(crossings) - 1) * 48000 / (crossings[-1] - crossings[0]) if len(crossings) > 2 else 0

    def load(self):
        self.press(F1)
        self.run_ms(200)                              # the listing
        self.press(OK)                                # the only entry: TONE.WAV
        for _ in range(100):
            if any(line.startswith("LOADED 1 ") for line in self.emu.report()["log"]):
                break
            self.run_ms(20)
        loaded = [line for line in self.emu.report()["log"] if line.startswith("LOADED 1 ")]
        self.assertTrue(loaded, self.emu.report()["log"])
        return int(loaded[-1].split()[2])

    def test_load_show_and_play(self):
        frames = self.load()
        self.assertAlmostEqual(frames, 48000, delta=2)              # 1 s at 44.1 kHz, made 48 kHz

        # The waveform: lit columns in its box where the tone is, dark where the silence is; START's mark at x 0.
        frame = self.screen()
        tone = [x for x in range(4, 60) if any(self.grey(frame, x, y) == 9 for y in range(14, 64))]
        quiet = [x for x in range(70, 120) if any(self.grey(frame, x, y) == 9 for y in range(14, 30))]
        self.assertGreater(len(tone), 50)
        self.assertEqual(quiet, [])
        self.assertEqual(self.grey(frame, 0, 20), 15)

        # The pad plays it: 1 kHz.
        self.pad(0)
        self.run_ms(150)
        self.assertAlmostEqual(self.pitch(self.left(120)), 1000, delta=3)
        self.run_ms(900)                              # to its end

        # Knob C, +12 semitones: 2 kHz.
        self.emu.turn(KNOB_C, 12, 20)
        self.run_ms(20)
        self.pad(0)
        self.run_ms(100)
        self.assertAlmostEqual(self.pitch(self.left(80)), 2000, delta=6)
        self.run_ms(600)

        # START past the tone (knob A to 60 %): the pad plays only silence.
        self.emu.turn(KNOB_C, -12, 20)
        self.emu.turn(KNOB_A, 600, 1)                 # quick turns move it faster; 1000 steps cover it either way
        self.run_ms(50)
        self.pad(0)
        self.run_ms(150)
        self.assertLess(max(abs(v) for v in self.left(120)), 2 ** 31 // 1000)
        self.assertEqual(self.emu.word("audio_overruns"), 0)


if __name__ == "__main__":
    unittest.main()
