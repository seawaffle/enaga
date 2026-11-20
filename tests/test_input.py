"""Input selection (E4 step 2) on the emulator: each (source, level) leaves the codec registers, front-end pins and
input slots of the matching original REC SRC route (docs/design.md "E4 plan", smpltrek-re HW-28 / HW-29), the
built-in mic is silent without headphones, and the input is quiet while it switches. (The default
coming back when the app stops is in tests/test_menu.py.)

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest tests.test_input
"""
import re
import struct
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENAGA = HERE.parent
sys.path.insert(0, str(ENAGA / "tools"))

import build_app  # noqa: E402
import paths  # noqa: E402

BOOT = 300_000_000
MS = 372_000
STEP1 = 32
GPIO = 0x01E26000
LEFT_IN, RIGHT_IN = 0x10000000, 0x20000000          # distinct levels on the left and right ADC slots
# Slot order: the even words of Enaga's buffers, its left channel, are the codec's slot 1, the second word of each pair
# in the emulator's input stream and output capture (docs/hardware.md "Audio").

# STEP key -> (codec 15, 16, 17, 18, 19, 21, 22, 24), pins (GP1[2], GP7[14], GP7[15], GP5[11], GP1[3]), what the app
# hears on (left, right).
ROUTES = {
    1: ((0, 0, 0xF0, 0xFF, 0xFC, 0xF8, 0xF8, 0xF8), (1, 1, 0, 0, 1), (LEFT_IN, LEFT_IN)),     # built-in mic (headphones in)
    2: ((0, 0, 0xFF, 0xFF, 0x84, 0xF8, 0xF8, 0xF8), (0, 0, 1, 0, 1), (LEFT_IN, LEFT_IN)),     # Hi-Z NORMAL
    3: ((0, 0, 0xFF, 0xFF, 0xF8, 0xF8, 0x84, 0xF8), (0, 1, 0, 1, 1), (RIGHT_IN, RIGHT_IN)),   # dynamic mic NORMAL
    4: ((0, 0, 0xFF, 0xFF, 0x84, 0xF8, 0x84, 0xF8), (0, 1, 0, 0, 1), (LEFT_IN, RIGHT_IN)),    # stereo line
    5: ((0, 0, 0xFF, 0xFF, 0x84, 0xF8, 0xF8, 0xF8), (0, 1, 0, 0, 1), (LEFT_IN, LEFT_IN)),     # mono line
    6: ((0, 0, 0xFF, 0xFF, 0x84, 0xF8, 0x84, 0xF8), (0, 1, 0, 0, 0), (LEFT_IN, RIGHT_IN)),    # stereo line, PAD
    7: ((20, 0, 0xFF, 0xFF, 0x84, 0xF8, 0xF8, 0xF8), (0, 0, 1, 0, 1), (LEFT_IN, LEFT_IN)),    # Hi-Z HIGH: +10 dB PGA
    8: ((0, 42, 0xFF, 0xFF, 0xF8, 0xF8, 0x84, 0xF8), (0, 1, 0, 1, 1), (RIGHT_IN, RIGHT_IN)),  # dynamic mic HIGH: +21 dB
}
DEFAULT = ROUTES[4]


@unittest.skipUnless(paths.emulator_present() and build_app.tools_present(), "needs ENAGA_EMULATOR and the toolchain")
class Input(unittest.TestCase):
    def setUp(self):
        from emulate import Emulation
        self.emu = Emulation(HERE / "apps" / "input")
        self.emu.cpu.audio_input(struct.pack("<II", RIGHT_IN, LEFT_IN) * 64, loop=True)
        self.emu.cpu.gpio_input(3, 8, True)            # headphones in
        self.assertIsNone(self.emu.run(BOOT)["error"])

    def tearDown(self):
        self.emu.close()

    def press(self, step):
        self.emu.key(STEP1 + step - 1, True)
        self.assertIsNone(self.emu.run(30 * MS)["error"])
        self.emu.key(STEP1 + step - 1, False)
        self.assertIsNone(self.emu.run(30 * MS)["error"])

    def codec(self):
        return tuple(self.emu.cpu.codec_register(0, r) for r in (15, 16, 17, 18, 19, 21, 22, 24))

    def pins(self):
        def out(bank, pin):
            word = int.from_bytes(self.emu.cpu.read(GPIO + 0x14 + 0x28 * (bank // 2), 4), "little")
            return (word >> (pin + 16 * (bank & 1))) & 1
        return (out(1, 2), out(7, 14), out(7, 15), out(5, 11), out(1, 3))

    def frames(self):                                 # every captured output frame, (left, right)
        raw, info = self.emu.cpu.audio_output()
        words = struct.unpack(f"<{len(raw) // 4}I", raw)
        if info["first_capture_slot"]:
            words = words[1:]
        return [(words[i + 1], words[i]) for i in range(0, len(words) - 1, 2)]

    def heard(self):                                  # the last frame the app passed through
        return self.frames()[-1]

    def test_each_choice_lands_on_its_original_route(self):
        self.assertEqual((self.codec(), self.pins(), self.heard()), DEFAULT)
        for step, expected in ROUTES.items():
            self.press(step)
            with self.subTest(step=step):
                self.assertEqual((self.codec(), self.pins(), self.heard()), expected)
        log = [line for line in self.emu.report()["log"] if re.fullmatch(r"I-?\d", line)]
        self.assertEqual(log, ["I0"] * len(ROUTES))
        self.press(9)                                 # an unknown source changes nothing
        self.assertEqual(self.emu.report()["log"][-1], "I-1")
        self.assertEqual((self.codec(), self.pins()), ROUTES[8][:2])

    def test_built_in_mic_is_silent_without_headphones(self):
        self.press(1)
        self.assertEqual(self.heard(), (LEFT_IN, LEFT_IN))
        self.emu.cpu.gpio_input(3, 8, False)
        self.assertIsNone(self.emu.run(10 * MS)["error"])
        self.assertEqual(self.heard(), (0, 0))
        self.emu.cpu.gpio_input(3, 8, True)
        self.assertIsNone(self.emu.run(10 * MS)["error"])
        self.assertEqual(self.heard(), (LEFT_IN, LEFT_IN))

    def test_switching_is_quiet_for_a_moment(self):
        before = len(self.frames())
        self.press(3)                                 # dynamic mic
        after = self.frames()[before:]
        longest = run = 0
        for frame in after:
            run = run + 1 if frame == (0, 0) else 0
            longest = max(longest, run)
        self.assertGreaterEqual(longest, 7 * 64)      # INPUT_QUIET_BLOCKS (8) of silence, give or take a block
        self.assertLess(longest, 16 * 64)
        self.assertEqual(after[-1], (RIGHT_IN, RIGHT_IN))

if __name__ == "__main__":
    unittest.main()
