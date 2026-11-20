"""The Enaga menu and app switching on the emulator (docs/design.md decisions 2 and 3, "Boot and app switching"):
F1 + POWER, switching between two apps repeatedly, the boot policy (last app, F1 held at power-on), an app
that fails at start-up not being loaded again, display names, open_menu, the fade and the stop() limit on a
switch, the SETTINGS page and its file, AUTO OFF and set_busy, an app's own settings (enaga::Settings), and
the input selection reset between apps.

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest tests.test_menu
"""
import struct
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
F1, OK, DOWN, UP, LEFT, RIGHT, CLR, STEP1 = 19, 23, 25, 17, 24, 18, 30, 32
APPS = {"TONE": ENAGA / "examples" / "tone", "CXXCHECK": ENAGA / "examples" / "cxxcheck", "HANG": HERE / "apps" / "hang",
        "MENUAPP": HERE / "apps" / "menu", "INPUT": HERE / "apps" / "input"}
TITLES = {"MENUAPP": "MENU-TEST"}                     # display names that differ from the folder


@unittest.skipUnless(paths.emulator_present() and build_app.tools_present(), "needs ENAGA_EMULATOR and the toolchain")
class Menu(unittest.TestCase):
    def card(self, d, names):
        image = Path(d) / "card.img"
        files = {}
        for name in names:
            package = build_app.build(APPS[name], paths.BUILD / "apps" / APPS[name].name, TITLES.get(name, name))["package"]
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

    def number(self, emu, name):                     # a float variable
        return struct.unpack("<f", struct.pack("<I", emu.word(name)))[0]

    def speaker_muted(self, emu):                    # GP2[8] driven high (GPIO bank pair 1 OUT_DATA, bit 8)
        return (int.from_bytes(emu.cpu.read(0x01E26000 + 0x14 + 0x28, 4), "little") >> 8) & 1

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

    def test_the_input_goes_back_to_stereo_line_when_an_app_stops(self):
        with tempfile.TemporaryDirectory() as d:
            image = self.card(d, ["INPUT", "TONE"])
            with self.boot(image) as emu:
                def route():                           # left / right PGA, LINE1L / LINE1R into their ADCs
                    return tuple(emu.cpu.codec_register(0, r) for r in (15, 16, 19, 22))
                self.press(emu, OK)
                self.assertEqual(self.state(emu)["name"], "INPUT")
                self.press(emu, STEP1 + 7)             # dynamic mic, HIGH
                self.assertEqual(route(), (0, 42, 0xF8, 0x84))
                self.chord(emu)
                self.press(emu, DOWN)
                self.press(emu, OK)
                self.assertIsNone(emu.run(100 * MS)["error"])
                self.assertEqual(self.state(emu)["name"], "TONE")
                self.assertEqual(route(), (0, 0, 0x84, 0x84))

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

    def test_names_open_menu_fade_slow_stop_settings_and_auto_off(self):
        with tempfile.TemporaryDirectory() as d:
            image = self.card(d, ["MENUAPP", "TONE"])
            with self.boot(image) as emu:
                self.assertEqual(self.state(emu)["menu"], 1)
                raw = emu.cpu.read(emu.names["menu_titles"], 48)
                titles = [raw[i:i + 24].split(b"\0")[0].decode() for i in (0, 24)]
                self.assertEqual(sorted(titles), ["MENU-TEST", "TONE"])          # the packages' names, not folders
                for _ in range(titles.index("MENU-TEST")):
                    self.press(emu, DOWN)
                self.press(emu, OK)
                self.assertEqual(self.state(emu)["name"], "MENU-TEST")
                self.press(emu, STEP1)                                          # open_menu from the app
                self.assertEqual(self.state(emu)["menu"], 1)
                self.press(emu, CLR)
                self.assertEqual(self.state(emu)["menu"], 0)
                self.assertEqual(self.speaker_muted(emu), 0)                    # SPEAKER ON by default
                self.press(emu, STEP1 + 3)                                      # the app mutes it
                self.assertEqual(self.speaker_muted(emu), 1)
                self.press(emu, STEP1 + 2)                                      # busy: AUTO OFF waits
                emu.cpu.write(emu.names["settings_auto_off_ms"], (300).to_bytes(4, "little"))
                self.assertIsNone(emu.run(600 * MS)["error"])
                self.assertEqual(emu.word("powering_off"), 0)
                emu.cpu.write(emu.names["settings_auto_off_ms"], (0).to_bytes(4, "little"))
                # A stop() that never finishes (and keeps feeding the watchdog by saving): dropped after
                # APPS_STOP_MS, and the switch to TONE goes on.
                self.press(emu, STEP1 + 1)
                self.chord(emu)
                self.press(emu, UP if titles.index("TONE") == 0 else DOWN)
                self.press(emu, OK)
                # The app's constant 0.25 fades out before stop() (the capture holds the last 0.68 s): no step.
                raw, info = emu.cpu.audio_output()
                words = struct.unpack(f"<{len(raw) // 4}i", raw)
                left = words[0 if info["first_capture_slot"] == 0 else 1::2]
                self.assertGreater(max(left), 0.24 * 2 ** 31)
                self.assertLess(left[-1], 0.001 * 2 ** 31)
                self.assertLess(max(abs(a - b) for a, b in zip(left, left[1:])), 0.01 * 2 ** 31)
                self.assertIsNone(emu.run(3500 * MS)["error"])
                s = self.state(emu)
                self.assertEqual((s["menu"], s["running"], s["name"]), (0, True, "TONE"), s)
                self.assertIn("APP STOP TOO LONG", s["log"])
                self.assertEqual(self.speaker_muted(emu), 0)                    # the app's mute ends with it
                self.assertEqual(emu.word("kernel_crashed"), 0)
                # SETTINGS: VOLUME 6, BRIGHTNESS 9, PADS HIGH, AUTO OFF 5 MIN, SPEAKER OFF, applied at once, saved on OK.
                self.chord(emu)
                for _ in range(3):
                    self.press(emu, UP)
                self.press(emu, DOWN)
                self.press(emu, DOWN)
                self.press(emu, OK)
                for _ in range(4):
                    self.press(emu, LEFT)
                self.assertAlmostEqual(self.number(emu, "audio_volume"), 0.251, places=3)
                self.press(emu, DOWN)
                self.press(emu, LEFT)
                self.press(emu, DOWN)
                self.press(emu, RIGHT)
                self.assertEqual(emu.word("pad_press_level"), 600)
                self.press(emu, DOWN)
                self.press(emu, RIGHT)
                self.assertEqual(emu.word("settings_auto_off_ms"), 300_000)
                self.press(emu, DOWN)
                self.press(emu, RIGHT)
                self.assertEqual(self.speaker_muted(emu), 1)
                self.press(emu, OK)
                self.assertEqual(self.state(emu)["menu"], 1)
                self.press(emu, CLR)
                self.assertIsNone(emu.run(2200 * MS)["error"])
                image.write_bytes(emu.cpu.sd_read())
            self.assertEqual(card.get(image, "/ENAGA/SETTINGS.TXT"),
                             b"VOLUME=6\nBRIGHTNESS=9\nPADS=HIGH\nAUTO OFF=5 MIN\nSPEAKER=OFF\nMIDI CH=1\nMIDI THRU=OFF\n"
                             b"CLOCK SRC=INT\nCLOCK OUT=OFF\nSYNC OUT=ON\n")
            self.assertEqual(card.get(image, "/ENAGA/DATA/MENU-TEST/SETTINGS.TXT"),
                             b"STARTS=1\nNOTE=SAVED BY MENU-TEST\n")
            with self.boot(image) as emu:              # read back at power-on; AUTO OFF without the app busy
                self.assertAlmostEqual(self.number(emu, "audio_volume"), 0.251, places=3)
                self.assertEqual((emu.word("pad_press_level"), emu.word("pad_release_level")), (600, 450))
                self.assertEqual(self.speaker_muted(emu), 1)
                self.assertEqual(self.state(emu)["name"], "TONE")
                emu.cpu.write(emu.names["settings_auto_off_ms"], (300).to_bytes(4, "little"))
                self.assertIsNone(emu.run(600 * MS)["error"])
                self.assertEqual(emu.word("powering_off"), 1)


if __name__ == "__main__":
    unittest.main()
