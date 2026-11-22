"""The UI toolkit on the firmware: examples/widgets driven on the emulator through every widget (list and grid,
parameter page with groups and resets, text entry, dialog, notice, meters, pad map, file browser) by keys, knobs and
pads, following what the widgets report in its log, and checking that each page puts its widget on the screen. The widgets' own logic is tested on the host
(tests/test_ui.py).

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest tests.test_widgets
"""
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
UP, DOWN, LEFT, RIGHT, OK, CLR = 17, 25, 24, 18, 23, 30
F1, F2, F3, F4, F5, F6, F7, F8 = 19, 26, 20, 27, 22, 29, 21, 28
SELECT_RIGHT, PUSH_B = 9, 0
KNOB_A, KNOB_B, VALUE = 0, 1, 4


@unittest.skipUnless(paths.emulator_present() and build_app.tools_present(), "needs ENAGA_EMULATOR and the toolchain")
class Widgets(unittest.TestCase):
    def setUp(self):
        from emulate import Emulation
        card = paths.BUILD / "widgets-card"
        card.mkdir(parents=True, exist_ok=True)
        (card / "kick.wav").write_bytes(b"RIFF")
        (card / "notes.txt").write_bytes(b"x")
        self.emu = Emulation(ENAGA / "examples" / "widgets", card_files={
            "/SAMPLES/Drums/kick_01.wav": card / "kick.wav", "/SAMPLES/notes.txt": card / "notes.txt"})
        self.addCleanup(self.emu.close)
        if self._testMethodName == "test_the_levels_page_meters_the_input":     # set before the codec starts
            self.emu.cpu.audio_input(struct.pack("<II", 0x20000000, 0x10000000) * 64, loop=True)
        self.assertIsNone(self.emu.run(BOOT)["error"])

    def press(self, key):
        self.emu.key(key, True)
        self.assertIsNone(self.emu.run(20 * MS)["error"])
        self.emu.key(key, False)
        self.assertIsNone(self.emu.run(20 * MS)["error"])

    def turn(self, encoder, detents, phase_ms=20):  # 80 ms a detent: deliberate, so a ParamPage does not speed up
        self.emu.turn(encoder, detents, phase_ms)
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
        self.turn(KNOB_A, 5, phase_ms=3)             # a quick spin, 12 ms a detent: 1 + 4 x 8
        self.assertEqual(self.log()[-1], "CHANGED 4 331")
        self.turn(KNOB_B, 5, phase_ms=3)             # LEVEL, 0-100: not wide, never faster
        self.assertEqual(self.log()[-1], "CHANGED 5 85")

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

    def grey(self, frame, x, y):                     # 0-15
        from display import pixels
        return pixels(frame)[y * 128 + x] // 17

    def test_a_notice_shows_for_a_moment(self):
        self.press(DOWN)
        self.press(OK)
        frame = self.screen()
        self.assertEqual((self.grey(frame, 0, 118), self.grey(frame, 127, 127)), (4, 4))   # its bar, over the hints
        self.assertIsNone(self.emu.run(1500 * MS)["error"])
        frame = self.screen()
        self.assertEqual((self.grey(frame, 0, 118), self.grey(frame, 127, 127)), (0, 0))   # gone, the hints back
        self.press(F6)                                                      # no input here: meters' tracks only
        frame = self.screen()
        self.assertEqual([self.grey(frame, x, 74) for x in (10, 50)], [2, 2])

    def test_the_levels_page_meters_the_input(self):
        self.press(F6)
        frame = self.screen()
        self.assertIn(9, [self.grey(frame, x, 74) for x in (10, 50)])       # a level in at least one
        self.turn(KNOB_A, -10)                                              # PAN L10: lit left of the middle
        frame = self.screen()
        self.assertEqual([self.grey(frame, x, 52) for x in (103, 106, 108)], [2, 9, 6])

    def test_the_sounds_as_a_grid(self):
        self.press(F1)                                                      # the grid, cursor on PIANO
        self.press(RIGHT); self.press(DOWN)                                 # STRINGS, then PAD under it
        self.press(OK)
        self.assertEqual(self.log()[-1], "CHOSE 5")
        frame = self.screen()
        self.assertEqual(self.grey(frame, 34, 37), 9)                       # PAD's cell lit (column 2, row 2)
        self.press(F1)                                                      # back to the list, at PAD
        self.press(DOWN); self.press(OK)
        self.assertEqual(self.log()[-1], "CHOSE 6")

    def test_edit_tabs_and_columns(self):
        self.press(F3)
        frame = self.screen()
        self.assertEqual(self.grey(frame, 127 - 6 * 7, 13), 4)              # OSC, the current tab, lit
        self.press(F1)                                                      # columns
        self.turn(KNOB_B, 3)
        self.assertEqual(self.log()[-1], "CHANGED 1 67")
        columns = self.screen()
        bars = [self.grey(columns, x, 12 + 14 + 34) for x in (0, 32, 64, 32 + 27)]
        self.assertEqual(bars, [2, 9, 9, 2])        # a bar per column: WAVE at 0, CUTOFF 67 of 127, RESONANCE 20%
        self.press(F1)
        self.assertNotEqual(self.screen(), columns)                         # rows again

    def test_the_pads_light_on_the_map(self):
        self.press(F7)
        for _ in range(2):                                                  # pad 3 (D#) hard: two reports
            self.emu.cpu.uart_receive(1, bytes([0xB0 | 3, 3300 & 0x7F, 3300 >> 7]))
            self.assertIsNone(self.emu.run(4 * MS)["error"])
        frame = self.screen()
        top = 12 + 24                                                       # the upper row
        self.assertEqual(self.grey(frame, 24 + 1, top + 1), 15)             # pad 3: upper, x 24, lit and framed
        self.assertEqual(self.grey(frame, 8 + 2, top + 2), 2)               # pad 1 dark
        for _ in range(2):
            self.emu.cpu.uart_receive(1, bytes([0xB0 | 3, 1, 0]))
            self.assertIsNone(self.emu.run(4 * MS)["error"])
        self.assertEqual(self.grey(self.screen(), 24 + 2, top + 2), 2)      # released: dark again

    def test_the_file_browser(self):
        self.press(F8)
        self.assertIsNone(self.emu.run(300 * MS)["error"])                  # the listing, in the background
        self.press(DOWN)                                                    # ENAGA/, SAMPLES/: into SAMPLES
        self.press(OK)
        self.assertIsNone(self.emu.run(300 * MS)["error"])
        self.press(OK)                                                      # DRUMS/ first, then NOTES.TXT
        self.assertIsNone(self.emu.run(300 * MS)["error"])
        self.press(OK)
        self.assertEqual(self.log()[-1], "FILE KICK_01.WAV")
        self.press(CLR); self.press(CLR)                                    # back up to the root
        self.assertIsNone(self.emu.run(300 * MS)["error"])
        self.press(DOWN); self.press(OK)
        self.assertIsNone(self.emu.run(300 * MS)["error"])
        self.press(DOWN); self.press(OK)
        self.assertEqual(self.log()[-1], "FILE NOTES.TXT")

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
