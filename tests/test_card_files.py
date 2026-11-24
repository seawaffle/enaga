"""Extra files on the emulator tools' cards (tools/panel.py and tools/emulate.py --file), long names included: the
cards are built by tools/card.py, through the firmware's own FatFs.

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest tests.test_card_files
"""
import io
import sys
import unittest
from contextlib import redirect_stderr
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENAGA = HERE.parent
sys.path.insert(0, str(ENAGA / "tools"))

import build_app  # noqa: E402
import card  # noqa: E402
import paths  # noqa: E402

MS = 372_000
BOOT = 300_000_000
DOWN, OK, F8 = 25, 23, 28
LONG = "/SAMPLES/Kick long 1.wav"            # spaces, not 8.3; short enough for the app's 21-character log line
WAV = b"RIFF" + bytes(range(256)) * 40
WORK = paths.BUILD / "card-files"


def local_wav():
    WORK.mkdir(parents=True, exist_ok=True)
    local = WORK / "kick.wav"
    local.write_bytes(WAV)
    return local


@unittest.skipUnless(paths.emulator_present() and build_app.tools_present(), "needs ENAGA_EMULATOR and the toolchain")
class CardFiles(unittest.TestCase):
    def test_the_panel_card_holds_a_long_name(self):
        paths.use_emulator()
        import panel
        image = panel.build_card([ENAGA / "examples" / "tone"], {LONG: local_wav()}, 64)
        self.assertEqual(card.get(image, LONG), WAV)
        self.assertIsNotNone(card.get(image, "/ENAGA/APPS/TONE/APP.ENA"))
        self.assertEqual(image.stat().st_size, 64 << 20)

    def test_the_firmware_browses_to_a_long_name(self):
        from emulate import Emulation
        with Emulation(ENAGA / "examples" / "widgets", card_files={LONG: local_wav()}) as emu:
            self.assertEqual(emu.card_file(LONG), WAV)
            self.assertIsNone(emu.run(BOOT)["error"])
            for key in (F8, DOWN, OK, OK):                      # FILES; ENAGA/, SAMPLES/: into SAMPLES; the file
                emu.key(key, True)
                self.assertIsNone(emu.run(20 * MS)["error"])
                emu.key(key, False)
                self.assertIsNone(emu.run(300 * MS)["error"])
            self.assertEqual(emu.report()["log"][-1], "FILE KICK LONG 1.WAV")

    def test_a_file_on_the_app_path_is_refused(self):
        from emulate import Emulation
        with self.assertRaises(ValueError):
            Emulation(ENAGA / "examples" / "tone", card_files={"/enaga/apps/tone/app.ena": local_wav()})
        import panel
        with self.assertRaises(ValueError):
            panel.build_card([ENAGA / "examples" / "tone"], {"/ENAGA/APPS/TONE/APP.ENA": local_wav()})

    def test_option_errors(self):
        local_wav()
        with self.assertRaises(FileNotFoundError):
            card.file_args([f"{LONG}={WORK / 'missing.wav'}"])
        with self.assertRaises(ValueError):
            card.file_args([LONG])
        self.assertEqual(card.file_args([f"{LONG}={WORK / 'kick.wav'}"]), {LONG: WORK / "kick.wav"})
        home = Path("~").expanduser()
        if WORK.is_relative_to(home):
            self.assertEqual(card.file_args([f"/A.WAV=~/{(WORK / 'kick.wav').relative_to(home)}"]),
                             {"/A.WAV": WORK / "kick.wav"})

    def test_files_that_do_not_fit_suggest_a_larger_card(self):
        big = WORK / "big.raw"
        WORK.mkdir(parents=True, exist_ok=True)
        big.write_bytes(bytes(3 << 20))
        with self.assertRaises(card.CardFull):
            card.make(WORK / "small.img", 2, "fat", {"/BIG.RAW": big})
        import emulate
        stderr = io.StringIO()
        with redirect_stderr(stderr):
            self.assertEqual(emulate.main(["--card-mib", "2", "--file", f"/Big file.raw={big}"]), 2)
        self.assertIn("--card-mib", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
