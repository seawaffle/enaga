"""The CHECK app for sessions on a unit (tests/unit/check, docs/hardware.md "Checks on a unit") runs on the
emulator: tones on the expected channels, the load page's voice cap, key LEDs on the panel page, and the card
page's measurement running to the end.

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest discover -s tests -p test_check.py -v
"""
import sys
import tempfile
import unittest
from pathlib import Path

from test_e1 import BOOT, HERE, MS, STEP1, build_app, component, paths, samples
from emulate import WORK, Emulation

sys.path.insert(0, str(HERE.parent / "tools"))
import card  # noqa: E402

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
            # The panel MCU sends about 4,200 pad messages a second all the time (smpltrek session M1); with
            # the main loop slowed by the load, polling lost about one in nine. The receive interrupt keeps all.
            parsed = emu.word("pad_messages")
            for i in range(840):
                emu.cpu.uart_receive(1, bytes([0xB0 | i % 16, i & 0x7F, 1]))
                emu.run(372_000_000 // 4192)
            emu.run(5 * MS)
            self.assertEqual((emu.word("pad_messages") - parsed, emu.word("uart1_overruns")), (840, 0))
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


    def test_card_page_measures_and_cleans_up(self):
        with Emulation(HERE / "unit" / "check", "CHECK") as emu, tempfile.TemporaryDirectory() as d:
            image = Path(d) / "card.img"
            card.make(image, 16, "fat", {"/ENAGA/APPS/CHECK/APP.ENA": (WORK / "emulate" / "CHECK.ENA").read_bytes()})
            emu.cpu.sd_load(image.read_bytes())
            self.assertIsNone(emu.run(BOOT)["error"])
            for _ in range(3):                                       # F4 held a second, three times: CARD
                self.press(emu, F4, hold_ms=1100)
            self.press(emu, STEP1)
            for _ in range(100):                                     # up to 10 s of emulated time
                self.assertIsNone(emu.run(100 * MS)["error"])
                log = emu.report()["log"]
                if any(line.startswith("CARD S") or line.startswith("CARD ") and "/S" not in line and " W " not in line
                       for line in log):
                    break
            self.assertTrue(any(line.startswith("CARD W") for line in log), log)
            write_read = next(line for line in log if line.startswith("CARD W")).split()
            scatter = next(line for line in log if line.startswith("CARD S")).split()
            print(f"\nemulated card: write {write_read[2]} KiB/s, read {write_read[4]} KiB/s, scattered {scatter[2]} KiB/s")
            self.assertTrue(all(int(v) > 0 for v in (write_read[2], write_read[4], scatter[2])))
            self.assertEqual(emu.report()["app_status"], "APP RUNNING")
            image.write_bytes(emu.cpu.sd_read())
            self.assertEqual(card.listdir(image, "/RECORD"), [])           # the test file removed


    def test_card_page_failure_stops_on_the_status_page(self):
        with Emulation(HERE / "unit" / "check", "CHECK") as emu:                # its 4 MiB card: no room for 4 MiB
            self.assertIsNone(emu.run(BOOT)["error"])
            for _ in range(3):
                self.press(emu, F4, hold_ms=1100)
            self.press(emu, STEP1)
            for _ in range(50):
                self.assertIsNone(emu.run(100 * MS)["error"])
                if emu.report()["crash_reason"]:
                    break
            report = emu.report()
            self.assertEqual((report["app_status"], report["crash_reason"]), ("APP STOPPED", "CARD TEST FAILED"))
            self.assertIn("CARD CARD FULL", report["log"])
            with tempfile.TemporaryDirectory() as d:
                image = Path(d) / "card.img"
                image.write_bytes(emu.cpu.sd_read())
                self.assertIsNone(card.get(image, "/RECORD/CHECK.BIN"))       # removed before stopping


if __name__ == "__main__":
    unittest.main()
