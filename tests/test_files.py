"""The file service on the emulator: tests/apps/files runs the file API through its cases and writes RESULTS.TXT;
this reads it, and the files the app wrote, back from the card image (tools/card.py, Enaga's own FatFs).

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest tests.test_files
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
BIG = bytes((i * 7 + (i >> 9)) & 255 for i in range(1 << 20))

# Every check the app makes, and its expected result, whatever the card (the removal and card-full runs add or
# change some below).
EXPECTED = {
    "data_folder_is_files": 1, "save_first": 0, "read_first": 1, "save_second": 0, "read_second": 1,
    "info_size": 6, "info_folder": 1, "case_insensitive": 6,
    "deny_apps": -5, "deny_other_app": -5, "deny_prefix_lookalike": -5, "deny_root": -5, "deny_area_itself": -5,
    "bad_dotdot": -9, "bad_dot": -9, "bad_relative": -9, "bad_trailing_slash": -9, "bad_empty_part": -9,
    "unaligned": 1, "save_shared": 0, "read_shared": 4, "tmp_fallback": 7, "too_big": -11, "not_found": -2,
    "folder_make": 0, "folder_again": 0, "folder_remove": 0, "remove_missing": -2,
    "open_eight": 0, "open_ninth": -6, "size": 1 << 20,
    "open_write": 0, "save_over_open": -12, "write_on_read_handle": -5, "bad_handle": -10,
    "load_queued": 0, "load_result": 1 << 20, "load_intact": 1,
    "write_open": 0, "write_queued": [0, 0, 0, 0], "write_result": [65536] * 4, "write_close": 0,
    "list_queued": 0, "list_big": 1 << 20, "list_folder": 1,
    "seek_while_pending": -12, "cancelled": [-8, -8, -8],
    "audio_context": -13,
}


@unittest.skipUnless(paths.emulator_present() and build_app.tools_present(), "needs ENAGA_EMULATOR and the toolchain")
class Files(unittest.TestCase):
    def run_app(self, fs, size_mb=64, pull=False, fill_to=None, crash=False):
        from emulate import Emulation
        files = {"/SAMPLES/BIG.RAW": BIG, "/SAMPLES/ONLYTMP.BIN.TMP": b"tmpdata"}
        if pull:
            files["/SAMPLES/PULL"] = b""
        if crash:
            files["/SAMPLES/CRASH"] = b""
        with tempfile.TemporaryDirectory() as d:
            image = Path(d) / "card.img"
            package = build_app.build(HERE / "apps" / "files", paths.BUILD / "apps" / "files", "FILES")["package"]
            card.make(image, size_mb, fs, {**files, "/ENAGA/APPS/FILES/APP.ENA": package})
            if fill_to is not None:
                card.put(image, "/FILLER.BIN", bytes(card.free(image) - fill_to))
            with Emulation() as emu:
                emu.cpu.sd_load(image.read_bytes())
                log = []
                for _ in range(400):                              # up to 4 s of emulated time
                    self.assertIsNone(emu.run(10 * MS)["error"])
                    log = emu.report()["log"]
                    if pull is True and "FILES PULL" in log:
                        emu.cpu.gpio_input(4, 0, True)            # card detect high: the card is out
                        self.assertIsNone(emu.run(400 * MS)["error"])
                        emu.cpu.gpio_input(4, 0, False)
                        pull = "done"
                    if "FILES DONE" in log or (crash and "FATAL" in log):
                        break
                if crash:
                    self.assertIsNone(emu.run(300 * MS)["error"])     # passes after the drop
                    report = emu.report()
                    return report, emu.word("files_requests_done"), image, emu.cpu.sd_read()
                self.assertIn("FILES DONE", log, log)
                image.write_bytes(emu.cpu.sd_read())
            results = {}
            text = card.get(image, "/ENAGA/DATA/FILES/RESULTS.TXT")
            self.assertIsNotNone(text, "no RESULTS.TXT")
            for line in text.decode().splitlines():
                name, value = line.rsplit(" ", 1)
                results.setdefault(name, []).append(int(value))
            results = {k: v[0] if len(v) == 1 else v for k, v in results.items()}
            return results, image, card.listdir(image, "/ENAGA/DATA/FILES"), card.get(image, "/RECORD/OUT.BIN"), \
                card.get(image, "/ENAGA/DATA/FILES/A long patch name.txt")

    def check(self, results, expected):
        for name, value in expected.items():
            self.assertEqual(results.get(name), value, name)

    def common(self, results, folder, out, patch):
        self.assertGreater(results["load_passes"], 5)                # the load ran across main-loop passes
        self.assertEqual(patch, b"second")                            # the second save replaced the first
        self.assertEqual(sorted(name for name, _ in folder), ["A long patch name.txt", "RESULTS.TXT"])   # no .TMP

    def test_fat_card_with_a_removal(self):
        results, image, folder, out, patch = self.run_app("fat", pull=True)
        self.check(results, {**EXPECTED, "list_count": 4,             # BIG.RAW, ONLYTMP.BIN.TMP, PULL, new folder
                             "card_removed": 0, "removed_read_all": -1, "removed_size": -1, "removed_close": -1,
                             "removed_open": -1, "card_back": 1, "back_read_all": 1})
        self.common(results, folder, out, patch)
        self.assertEqual(out, BIG[:4 * 65536])
        self.assertIsNone(card.get(image, "/RECORD/locked.bin.TMP"))

    def test_exfat_card(self):
        results, image, folder, out, patch = self.run_app("exfat")
        self.check(results, {**EXPECTED, "list_count": 3})
        self.assertNotIn("card_removed", results)
        self.common(results, folder, out, patch)
        self.assertEqual(out, BIG[:4 * 65536])

    def test_app_dropped_with_a_load_pending(self):
        report, done, image, data = self.run_app("fat", crash=True)
        self.assertEqual((report["app_status"], report["crash_reason"]), ("APP STOPPED", "files crash"))
        self.assertNotIn("FILES CALLBACK AFTER DROP", report["log"])
        self.assertEqual(done, 0)                                     # the load was dropped, not finished
        with tempfile.TemporaryDirectory() as d:                      # the card is still sound
            copy = Path(d) / "card.img"
            copy.write_bytes(data)
            self.assertEqual(card.get(copy, "/SAMPLES/BIG.RAW"), BIG)

    def test_full_card(self):
        results, image, folder, out, patch = self.run_app("fat", size_mb=8, fill_to=100 * 1024)
        writes = results["write_result"]
        self.assertEqual(writes[0], 65536)
        self.assertIn(-4, writes)                                     # ENAGA_FILE_FULL
        self.assertEqual(results["full_remove"], 0)
        self.assertIsNone(out)
        self.assertEqual(patch, b"second")


    def test_write_probe(self):
        # /ENAGA/SDTEST.BIN: both SD write sequences into the file's first two sectors, read back; the rest of the
        # card untouched (the file still reads, with the patterns in it).
        from emulate import Emulation
        with tempfile.TemporaryDirectory() as d:
            image = Path(d) / "card.img"
            card.make(image, 64, "fat", {"/ENAGA/SDTEST.BIN": bytes(4096), "/SAMPLES/BIG.RAW": BIG})
            with Emulation() as emu:
                emu.cpu.sd_load(image.read_bytes())
                self.assertIsNone(emu.run(1000 * MS)["error"])
                log = emu.report()["log"]
                image.write_bytes(emu.cpu.sd_read())
            self.assertEqual([line.split()[:3] for line in log if line.startswith("SDT")],
                             [["SDT", "0", "OK"], ["SDT", "1", "OK"]], log)
            data = card.get(image, "/ENAGA/SDTEST.BIN")
            for mode in range(2):
                words = struct.unpack("<128I", data[512 * mode:512 * mode + 512])
                self.assertEqual(words, tuple((mode + 1) << 28 | i << 16 | (~i & 0xFFFF) for i in range(128)))
            self.assertEqual(data[1024:], bytes(3072))
            self.assertEqual(card.get(image, "/SAMPLES/BIG.RAW"), BIG)


if __name__ == "__main__":
    unittest.main()
