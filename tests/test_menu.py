"""The Enaga menu and app switching on the emulator (docs/design.md decisions 2 and 3, "Boot and app switching"):
F1 + POWER, switching between two apps repeatedly, the boot policy (last app, F1 held at power-on) and an app
that fails at start-up not being loaded again.

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest tests.test_menu
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

MS = 372_000
F1, OK, DOWN, UP, STEP1 = 19, 23, 25, 17, 32
APPS = {"TONE": ENAGA / "examples" / "tone", "CXXCHECK": ENAGA / "examples" / "cxxcheck", "HANG": HERE / "apps" / "hang"}


@unittest.skipUnless(paths.emulator_present() and build_app.tools_present(), "needs ENAGA_EMULATOR and the toolchain")
class Menu(unittest.TestCase):
    def card(self, d, names):
        image = Path(d) / "card.img"
        files = {}
        for name in names:
            package = build_app.build(APPS[name], paths.BUILD / "apps" / APPS[name].name, name)["package"]
            files[f"/ENAGA/APPS/{name}/APP.ENA"] = package
        card.make(image, 16, "fat", files)
        return image

    def boot(self, image, f1=False):
        from emulate import Emulation
        emu = Emulation()
        emu.cpu.sd_load(image.read_bytes())
        emu.cpu.gpio_input(3, 0, True)                 # POWER up (active low)
        if f1:
            emu.key(F1, True)
        self.assertIsNone(emu.run(700 * MS)["error"])
        if f1:
            emu.key(F1, False)
            self.assertIsNone(emu.run(30 * MS)["error"])
        return emu

    def press(self, emu, key, ms=30):
        emu.key(key, True)
        self.assertIsNone(emu.run(ms * MS)["error"])
        emu.key(key, False)
        self.assertIsNone(emu.run(ms * MS)["error"])

    def chord(self, emu):                              # F1 held, POWER pressed and released, F1 released
        emu.key(F1, True)
        self.assertIsNone(emu.run(30 * MS)["error"])
        emu.cpu.gpio_input(3, 0, False)
        self.assertIsNone(emu.run(60 * MS)["error"])
        emu.cpu.gpio_input(3, 0, True)
        self.assertIsNone(emu.run(30 * MS)["error"])
        emu.key(F1, False)
        self.assertIsNone(emu.run(30 * MS)["error"])

    def state(self, emu):
        r = emu.report()
        return dict(menu=emu.word("menu_active"), running=r["app_running"], name=r["app_name"],
                    status=r["app_status"], message=emu.text("menu_message", 40), log=r["log"])

    def test_switching_two_apps_repeatedly(self):
        with tempfile.TemporaryDirectory() as d:
            image = self.card(d, ["TONE", "CXXCHECK"])
            with self.boot(image) as emu:
                s = self.state(emu)
                self.assertEqual((s["menu"], s["running"]), (1, False), s)   # two apps, no record: the menu
                self.press(emu, OK)
                first = self.state(emu)["name"]
                self.assertIn(first, ("TONE", "CXXCHECK"))
                names = [first]
                for _ in range(10):                    # F1 + POWER, the other app, OK
                    self.chord(emu)
                    s = self.state(emu)
                    self.assertEqual((s["menu"], s["running"], s["name"]), (1, True, names[-1]), s)
                    self.press(emu, DOWN if names[-1] == first else UP)
                    self.press(emu, OK)
                    self.assertIsNone(emu.run(100 * MS)["error"])
                    s = self.state(emu)
                    self.assertEqual((s["menu"], s["running"], s["status"]), (0, True, "APP RUNNING"), s)
                    self.assertNotEqual(s["name"], names[-1])
                    names.append(s["name"])
                self.assertEqual(emu.word("kernel_crashed"), 0)
                self.assertEqual(emu.word("audio_overruns"), 0)
                # TONE paused: STEP 1 in the menu is not its note; closed again (F1 + POWER), it is.
                if names[-1] != "TONE":
                    self.chord(emu)
                    self.press(emu, UP if first == "TONE" else DOWN)
                    self.press(emu, OK)
                self.assertEqual(self.state(emu)["name"], "TONE")
                self.chord(emu)
                self.press(emu, STEP1)
                self.assertNotIn("note on", self.state(emu)["log"])
                self.chord(emu)
                self.assertEqual(self.state(emu)["menu"], 0)
                self.press(emu, STEP1)
                self.assertIn("note on", self.state(emu)["log"])
                self.assertIsNone(emu.run(2200 * MS)["error"])      # settled: the record says RUNNING
                image.write_bytes(emu.cpu.sd_read())
            self.assertEqual(card.get(image, "/ENAGA/LAST.TXT"), b"TONE\nRUNNING\n")
            with self.boot(image) as emu:              # the last app comes back
                s = self.state(emu)
                self.assertEqual((s["menu"], s["name"], s["status"]), (0, "TONE", "APP RUNNING"), s)
            with self.boot(image, f1=True) as emu:     # F1 held at power-on: the menu, no app
                s = self.state(emu)
                self.assertEqual((s["menu"], s["running"], s["message"]), (1, False, "F1 HELD: MENU"), s)
            card.put(image, "/ENAGA/LAST.TXT", b"GONE\nRUNNING\n")
            with self.boot(image) as emu:              # the last app since removed: as with no record
                s = self.state(emu)
                self.assertEqual((s["menu"], s["running"], s["message"]), (1, False, ""), s)

    def test_an_app_that_fails_at_start_up_is_not_loaded_again(self):
        with tempfile.TemporaryDirectory() as d:
            image = self.card(d, ["HANG", "TONE"])
            with self.boot(image) as emu:
                if not self.state(emu)["menu"]:
                    self.fail(self.state(emu))
                while emu.text("app_name", 24) != "HANG":   # choose HANG: it never returns from init
                    self.press(emu, OK)
                    if self.state(emu)["name"] == "HANG":
                        break
                    self.chord(emu)
                    self.press(emu, DOWN)
                self.assertIsNone(emu.run(1300 * MS)["error"])
                s = self.state(emu)
                self.assertEqual((s["running"], s["status"], emu.text("crash_reason", 48)),
                                 (False, "APP STOPPED", "APP NOT RESPONDING"), s)
                self.assertEqual(s["menu"], 0, s)                    # the status page shows why
                self.press(emu, OK)
                self.assertEqual(self.state(emu)["menu"], 1)         # OK: the menu
                image.write_bytes(emu.cpu.sd_read())
            self.assertEqual(card.get(image, "/ENAGA/LAST.TXT"), b"HANG\nSTARTING\n")
            for _ in range(2):                         # not loaded at the next boots either
                with self.boot(image) as emu:
                    s = self.state(emu)
                    self.assertEqual((s["menu"], s["running"], s["message"]), (1, False, "HANG DID NOT START"), s)


if __name__ == "__main__":
    unittest.main()
