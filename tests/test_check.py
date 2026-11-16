"""The CHECK app for sessions on a unit (tests/unit/check, docs/hardware.md "Checks on a unit") runs on the
emulator: tones on the expected channels, the load page's voice cap, and key LEDs on the panel page.

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest discover -s tests -p test_check.py -v
"""
import unittest

from test_e1 import BOOT, HERE, MS, STEP1, build_app, component, paths, samples
from emulate import Emulation

F4, F5 = 27, 22
F5_GREEN = 94                    # its panel MCU address (docs/hardware.md "LED map")


@unittest.skipUnless(build_app.tools_present(), "tic6x GCC not built (tools/build_tic6x_*.sh)")
@unittest.skipUnless(paths.emulator_present(), "ENAGA_EMULATOR not set to the SmplTrek emulator directory")
class Check(unittest.TestCase):
    def press(self, emu, key, hold_ms=15):
        emu.key(key, True)
        emu.run(hold_ms * MS)
        emu.key(key, False)
        emu.run(15 * MS)

    def channels(self, emu):
        emu.run(100 * MS)
        words = samples(emu)[-4096:]
        return words[0::2], words[1::2]

    def test_check_app_pages(self):
        with Emulation(HERE / "unit" / "check", "CHECK") as emu:
            self.assertIsNone(emu.run(BOOT)["error"])
            report = emu.report()
            self.assertEqual((report["app_status"], report["log"][-2:]), ("APP RUNNING", ["check ready", "CHECK"]))

            self.press(emu, STEP1)                                   # AUDIO: 750 Hz, left only
            left, right = self.channels(emu)
            self.assertAlmostEqual(component(left, 64), 0.25, places=2)
            self.assertLess(max(map(abs, right)), 2 ** 20)
            self.press(emu, STEP1 + 1)                               # 1500 Hz, right only
            left, right = self.channels(emu)
            self.assertAlmostEqual(component(right, 32), 0.25, places=2)
            self.assertLess(max(map(abs, left)), 2 ** 20)

            self.press(emu, F4, 1100)                              # LOAD
            self.press(emu, STEP1 + 7)                               # 256 synth voices: over budget, so capped
            emu.run(200 * MS)
            overruns = emu.word("audio_overruns")
            self.assertLess(emu.word("audio_cycles_last"), 0.9 * 496_000)
            emu.run(200 * MS)
            self.assertEqual(emu.word("audio_overruns"), overruns)  # the cap holds
            self.press(emu, STEP1 + 4)                               # keys still work: 16 voices
            emu.run(20 * MS)
            self.assertLess(emu.word("audio_cycles_last"), 0.2 * 496_000)

            self.press(emu, F4, 1100)                              # PANEL: a held key lights its LED
            emu.key(F5, True)
            emu.run(30 * MS)
            self.assertEqual(emu.cpu.read(emu.names["led_level"], 144)[F5_GREEN], 10)
            emu.key(F5, False)
            emu.run(30 * MS)
            self.assertEqual(emu.cpu.read(emu.names["led_level"], 144)[F5_GREEN], 0)
            self.assertEqual(emu.report()["app_status"], "APP RUNNING")


if __name__ == "__main__":
    unittest.main()
