"""The UI toolkit on the firmware: examples/widgets driven on the emulator through every widget (list, parameter
page with groups and resets, text entry, dialog) by keys and knobs, following what the widgets report in its log,
and checking that each page puts its widget on the screen. The widgets' own logic is tested on the host
(tests/test_ui.py).

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest tests.test_widgets
"""
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENAGA = HERE.parent
sys.path.insert(0, str(ENAGA / "tools"))

import build_app  # noqa: E402
import paths  # noqa: E402
from unit_dry_run import ENCODER_PINS, turn  # noqa: E402

BOOT = 300_000_000
MS = 372_000
UP, DOWN, LEFT, RIGHT, OK, CLR = 17, 25, 24, 18, 23, 30
F2, F3, F4, F5 = 26, 20, 27, 22
SELECT_RIGHT, PUSH_B = 9, 0
KNOB_A, KNOB_B, VALUE = 0, 1, 4


@unittest.skipUnless(paths.emulator_present() and build_app.tools_present(), "needs ENAGA_EMULATOR and the toolchain")
class Widgets(unittest.TestCase):
    def setUp(self):
        from emulate import Emulation
        self.emu = Emulation(ENAGA / "examples" / "widgets")
        self.addCleanup(self.emu.close)
        for pins in ENCODER_PINS:                    # at rest both encoder pins are high
            for bank, pin in pins:
                self.emu.cpu.gpio_input(bank, pin, True)
        self.assertIsNone(self.emu.run(BOOT)["error"])

    def press(self, key):
        self.emu.key(key, True)
        self.assertIsNone(self.emu.run(20 * MS)["error"])
        self.emu.key(key, False)
        self.assertIsNone(self.emu.run(20 * MS)["error"])

    def turn(self, encoder, detents):               # through the encoder's quadrature pins (tools/unit_dry_run.py)
        turn(self.emu, encoder, detents)
        self.assertIsNone(self.emu.run(10 * MS)["error"])

    def log(self):
        return self.emu.report()["log"]

    def screen(self):                                # the frame's pixels, after the next redraw
        self.assertIsNone(self.emu.run(120 * MS)["error"])
        frame, _ = self.emu.cpu.oled_frame()
        return frame

    def test_every_widget_through_its_controls(self):
        first = self.screen()
        self.press(DOWN); self.press(DOWN)           # the list: BRASS, chosen with OK
        self.press(OK)
        self.assertEqual(self.log()[-1], "CHOSE 2")
        self.assertNotEqual(self.screen(), first)

        self.press(F3)                               # the parameter page
        self.turn(KNOB_B, 3)                         # CUTOFF 64 -> 67
        self.assertEqual(self.log()[-1], "CHANGED 1 67")
        self.press(PUSH_B)                           # back to its initial value
        self.assertEqual(self.log()[-1], "CHANGED 1 64")
        self.press(SELECT_RIGHT)                     # the second group: RELEASE on knob A
        self.turn(KNOB_A, -2)
        self.assertEqual(self.log()[-1], "CHANGED 4 298")

        self.press(F4)                               # text entry, starting from MY SOUND
        self.turn(KNOB_B, 1)                         # M -> N
        self.press(RIGHT); self.press(UP)            # Y -> Z
        self.press(OK)
        self.assertEqual(self.log()[-1], "NAME NZ SOUND")
        self.press(F4)
        self.turn(VALUE, 1)
        self.press(CLR)                              # cancelled: the name stays
        self.assertEqual(self.log()[-1], "NAME KEPT")

        self.press(F5)                               # the dialog: no, then yes
        self.press(CLR)
        self.assertEqual(self.log()[-1], "RESET NO")
        self.press(F5)
        self.press(OK)
        self.assertEqual(self.log()[-1], "RESET YES")

    def test_an_open_dialog_takes_the_keys(self):
        self.press(F5)
        before = self.log()
        self.press(F2)                               # would switch page; swallowed by the dialog
        self.press(DOWN)
        self.assertEqual(self.log(), before)
        self.press(OK)
        self.assertEqual(self.log()[-1], "RESET YES")


if __name__ == "__main__":
    unittest.main()
