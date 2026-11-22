"""The battery (E5 step 1) on the emulator: the level from panel channel 15 with the original firmware's thresholds
and hysteresis, -1 off batteries (GP3[7] low), the BATTERY setting, the mark in the menu and STATUS, and the
switch-off when it stays empty, which stops the app (its stop() saves a file) before releasing the power hold.

The sample period and the time to switch off are shortened through Enaga's variables (battery_sample_ms,
battery_empty_ms); the original firmware samples once a second and averages eight.

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest tests.test_battery
"""
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENAGA = HERE.parent
sys.path.insert(0, str(ENAGA / "tools"))

import build_app  # noqa: E402
import card  # noqa: E402
import paths  # noqa: E402

BOOT = 300_000_000
MS = 372_000
STEP1 = 32
F1 = 19


@unittest.skipUnless(paths.emulator_present() and build_app.tools_present(), "needs ENAGA_EMULATOR and the toolchain")
class Battery(unittest.TestCase):
    def setUp(self):
        from emulate import Emulation
        self.emu = Emulation(HERE / "apps" / "battery", "BATTERY")
        self.addCleanup(self.emu.close)
        self.assertIsNone(self.emu.run(BOOT)["error"])
        self.poke("battery_sample_ms", 5)              # eight samples in 40 ms
        self.emu.cpu.gpio_input(3, 7, True)            # on batteries

    def poke(self, name, value):
        self.emu.cpu.write(self.emu.names[name], value.to_bytes(4, "little"))

    def run_ms(self, ms):
        self.assertIsNone(self.emu.run(int(ms * MS))["error"])

    def battery(self, raw):                            # a channel 15 reading, then time for a full average
        self.emu.cpu.uart_receive(1, bytes([0xBF, raw & 0x7F, raw >> 7]))
        self.run_ms(60)

    def level(self):                                   # what the app's enaga::system::battery() says
        self.emu.key(STEP1, True)
        self.run_ms(20)
        self.emu.key(STEP1, False)
        self.run_ms(10)
        return [x for x in self.emu.report()["log"] if x[:1] == "B" and x[:2] != "BA"][-1]

    def test_level_thresholds_hysteresis_and_power_source(self):
        self.battery(3100)                             # ALKALINE: below 3180, so 3
        self.assertEqual(self.level(), "B3")
        self.battery(3200)                             # back above 3180 but under 3280: still 3
        self.assertEqual(self.level(), "B3")
        self.battery(3300)
        self.assertEqual(self.level(), "B4")
        self.battery(2900)                             # under 2980: 2, logged with the average
        self.assertEqual(self.level(), "B2")
        self.assertIn("BATTERY 2 2900", self.emu.report()["log"])
        self.battery(1500)                             # under 2000: ignored
        self.assertEqual(self.level(), "B2")
        self.emu.cpu.gpio_input(3, 7, False)           # USB or adapter power
        self.run_ms(10)
        self.assertEqual(self.level(), "B-1")

    def test_the_menu_shows_the_level(self):
        self.battery(3300)                             # 4: four bars
        self.emu.cpu.gpio_input(3, 0, True)            # POWER up (active low), then F1 + POWER
        self.emu.key(F1, True)
        self.run_ms(30)
        self.emu.cpu.gpio_input(3, 0, False)
        self.run_ms(60)
        self.emu.cpu.gpio_input(3, 0, True)
        self.emu.key(F1, False)
        self.run_ms(100)
        canvas = self.emu.cpu.read(self.emu.names["canvas"], 128 * 128)
        row = [canvas[3 * 128 + x] for x in range(112, 128)]    # through the middle of the mark
        self.assertEqual(row, [10, 0, 12, 12, 0, 12, 12, 0, 12, 12, 0, 12, 12, 10, 10, 10])
        self.emu.cpu.gpio_input(3, 7, False)           # on USB: no mark
        self.run_ms(100)
        canvas = self.emu.cpu.read(self.emu.names["canvas"], 128 * 128)
        self.assertFalse(any(canvas[y * 128 + x] for y in range(7) for x in range(112, 128)))

    def test_battery_type_changes_the_thresholds(self):
        self.battery(2960)                             # ALKALINE: under 2980, 2
        self.assertEqual(self.level(), "B2")
        self.emu.cpu.write(self.emu.names["battery_type_setting"], (1).to_bytes(4, "little"))
        self.battery(2960)                             # NiMH: at least 2955 (rising from 2), under 3005: 3
        self.assertEqual(self.level(), "B3")

    def test_an_empty_battery_warns_then_switches_off_after_stopping_the_app(self):
        self.poke("battery_empty_ms", 400)
        self.battery(2500)                             # under 2630: empty
        self.assertEqual(self.level(), "B0")
        self.assertIn("BATTERY EMPTY", self.emu.report()["log"])
        canvas = self.emu.cpu.read(self.emu.names["canvas"], 128 * 128)
        self.assertTrue(any(canvas[118 * 128:128 * 128]), "the warning bar at the bottom")
        self.assertEqual(self.emu.word("powering_off"), 0)
        self.run_ms(500)
        self.assertEqual(self.emu.word("powering_off"), 1)
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / "card.img"
            image.write_bytes(self.emu.cpu.sd_read())
            self.assertEqual(card.get(image, "/ENAGA/DATA/BATTERY/STOP.TXT"), b"STOPPED")

    def test_a_battery_that_recovers_does_not_switch_off(self):
        self.poke("battery_empty_ms", 400)
        self.battery(2500)
        self.battery(2800)                             # 2730 or more: back to 1 before the time is up
        self.run_ms(500)
        self.assertEqual(self.emu.word("powering_off"), 0)
        self.assertEqual(self.level(), "B1")


if __name__ == "__main__":
    unittest.main()
