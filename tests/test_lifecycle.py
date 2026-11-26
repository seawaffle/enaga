"""An app's life on the emulator at its edges, each with a small app made here: failing or faulting while it
starts, asking for things in init(), crashing with the input and clock changed, the start-up record after a start
from the menu, paths at the length limit, all files and streams open at once, a save after one cut short, a
large package, the settings page left without OK, and MIDI clock on a port MIDI IN leaves out.

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest tests.test_lifecycle
"""
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENAGA = HERE.parent
sys.path.insert(0, str(ENAGA / "tools"))

import build_app  # noqa: E402
import card  # noqa: E402
import paths  # noqa: E402

MS = 372_000
F1, OK, DOWN, LEFT, STEP1 = 19, 23, 25, 24, 32
HEAD = "#include <enaga/app.hpp>\n#include <enaga/files.hpp>\n#include <enaga/clock.hpp>\n#include <enaga/ui.hpp>\n"


def package(key, code, title=None):
    folder = paths.BUILD / "lifecycle" / key
    folder.mkdir(parents=True, exist_ok=True)
    source = folder / "app.cpp"
    if not source.exists() or source.read_text() != HEAD + code:
        source.write_text(HEAD + code)
    return build_app.build(folder, folder / "out", title or key.upper())["package"]


def padded(p, size):                                  # the payload grown with zeros, the icon dropped
    header = list(build_app.HEADER.unpack_from(p))
    payload = p[build_app.HEADER.size:build_app.HEADER.size + header[4]].ljust(size, b"\0")
    header[4], header[7], header[9] = len(payload), zlib.crc32(payload), 0
    return build_app.HEADER.pack(*header) + payload


@unittest.skipUnless(paths.emulator_present() and build_app.tools_present(), "needs ENAGA_EMULATOR and the toolchain")
class Lifecycle(unittest.TestCase):
    def emulate(self, files, ms=800, **options):
        from emulate import Emulation
        emu = Emulation(card_files=files, **options)
        self.assertIsNone(emu.run(ms * MS)["error"])
        return emu

    def press(self, emu, key):
        emu.key(key, True)
        self.assertIsNone(emu.run(30 * MS)["error"])
        emu.key(key, False)
        self.assertIsNone(emu.run(30 * MS)["error"])

    def chord(self, emu):                              # F1 held, POWER pressed and released
        emu.key(F1, True)
        self.assertIsNone(emu.run(30 * MS)["error"])
        emu.cpu.gpio_input(3, 0, False)
        self.assertIsNone(emu.run(60 * MS)["error"])
        emu.cpu.gpio_input(3, 0, True)
        self.assertIsNone(emu.run(30 * MS)["error"])
        emu.key(F1, False)
        self.assertIsNone(emu.run(30 * MS)["error"])

    def byte(self, emu, name):
        return emu.cpu.read(emu.names[name], 1)[0]

    def test_a_fault_in_init_drops_the_app_not_the_kernel(self):
        p = package("except", "class T : public enaga::App { bool init() override { asm volatile(\"swe\"); return true; } };\n"
                              "ENAGA_APP(T)\n")
        with self.emulate({"/ENAGA/APPS/EXCEPT/APP.ENA": p}) as emu:
            self.assertEqual(emu.word("kernel_crashed"), 0)
            self.assertEqual(emu.text("crash_reason", 48), "CPU EXCEPTION")
            self.assertFalse(emu.report()["app_running"])

    def test_a_failed_init_gives_back_what_it_took(self):
        bad = package("initfail", """
class T : public enaga::App {
    bool init() override {
        for (int i = 0; i < 8; i++) { char path[] = "/SAMPLES/LEAK0.BIN"; path[13] = char('0' + i);
                                      enaga::host().file_open(path, ENAGA_FILE_WRITE); }
        enaga::system::input(ENAGA_INPUT_HIZ, ENAGA_INPUT_HIGH);
        return false;
    }
};
ENAGA_APP(T)
""")
        good = package("good", """
class T : public enaga::App {
    bool init() override {
        enaga::ui::Line l; l.add("OPEN ").number(enaga::host().file_open("/SAMPLES/GOOD.BIN", ENAGA_FILE_WRITE));
        enaga::log(l); return true;
    }
};
ENAGA_APP(T)
""")
        files = {"/ENAGA/APPS/BAD/APP.ENA": bad, "/ENAGA/APPS/GOOD/APP.ENA": good, "/ENAGA/LAST.TXT": b"BAD\nRUNNING\n"}
        with self.emulate(files) as emu:
            self.assertEqual(emu.report()["app_status"], "APP INIT FAILED")
            self.assertEqual(self.byte(emu, "input_source"), 3)          # the input back to stereo line
            self.press(emu, DOWN)
            self.press(emu, OK)
            r = emu.report()
            self.assertEqual((r["app_status"], r["log"][-2]), ("APP RUNNING", "OPEN 0"), r["log"])

    def test_requests_made_in_init_stand(self):
        p = package("asks", "class T : public enaga::App { bool init() override {\n"
                            "    enaga::system::set_busy(true); enaga::system::open_menu(); return true; } };\nENAGA_APP(T)\n")
        with self.emulate({"/ENAGA/APPS/ASKS/APP.ENA": p}) as emu:
            self.assertEqual((emu.word("app_keep_on"), emu.word("menu_active")), (1, 1))

    def test_a_crash_resets_the_input_and_the_clock(self):
        p = package("cleanup", """
class T : public enaga::App {
    bool init() override { enaga::system::input(ENAGA_INPUT_HIZ, ENAGA_INPUT_HIGH); enaga::clock::start(); return true; }
    void on_key(int k, bool d) override { if (k == ENAGA_KEY_STEP1 && d) enaga::fatal("probe crash"); }
};
ENAGA_APP(T)
""")
        with self.emulate({"/ENAGA/APPS/CLEANUP/APP.ENA": p}) as emu:
            self.assertEqual((self.byte(emu, "input_source"), self.byte(emu, "running")), (1, 1))
            emu.key(STEP1, True)
            self.assertIsNone(emu.run(200 * MS)["error"])
            self.assertEqual(emu.text("crash_reason", 48), "probe crash")
            self.assertEqual((self.byte(emu, "input_source"), self.byte(emu, "running")), (3, 0))   # stereo line
            self.assertEqual(emu.word("kernel_crashed"), 0)

    def test_an_app_started_from_the_menu_is_starting_until_it_has_run(self):
        p = package("later", """
class T : public enaga::App {
    uint32_t start_ = 0;
    bool init() override { start_ = enaga::ms(); return true; }
    void update(uint32_t now) override { if (int32_t(now - start_) > 500) for (;;) asm volatile(""); }   // 0.5 s in (signed: update can be given
                                                                    // the pass's ms, from before init)
};
ENAGA_APP(T)
""")
        p = padded(p, 1 << 20)                                            # a load that takes some milliseconds
        tone = build_app.build(ENAGA / "examples" / "tone", paths.BUILD / "apps" / "tone", "TONE")["package"]
        with tempfile.TemporaryDirectory() as d:
            image = Path(d) / "card.img"
            card.make(image, 16, "fat", {"/ENAGA/APPS/LATER/APP.ENA": p, "/ENAGA/APPS/TONE/APP.ENA": tone})
            from emulate import Emulation
            with Emulation() as emu:
                emu.cpu.sd_load(image.read_bytes())
                self.assertIsNone(emu.run(700 * MS)["error"])
                self.assertEqual(emu.word("menu_active"), 1)
                while emu.report()["app_name"] != "LATER":
                    self.press(emu, OK)
                    if emu.report()["app_name"] == "LATER":
                        break
                    self.chord(emu)
                    self.press(emu, DOWN)
                self.assertIsNone(emu.run(1700 * MS)["error"])
                self.assertEqual(emu.text("crash_reason", 48), "APP NOT RESPONDING")
                image.write_bytes(emu.cpu.sd_read())
            self.assertEqual(card.get(image, "/ENAGA/LAST.TXT"), b"LATER\nSTARTING\n")

    def test_paths_at_the_limit(self):
        p = package("paths", """
class T : public enaga::App {
    bool init() override {
        char path[258]; path[0] = '/'; for (int i = 1; i <= 256; i++) path[i] = 'A'; path[257] = 0;
        for (int n = 257; n >= 255; n--) {
            path[n] = 0;
            enaga::ui::Line l; l.add("PATH ").number(n).add(" ").number(enaga::host().file_list(path, nullptr, nullptr, nullptr));
            enaga::log(l);
        }
        return true;
    }
};
ENAGA_APP(T)
""")
        with self.emulate({"/ENAGA/APPS/PATHS/APP.ENA": p}) as emu:
            log = emu.report()["log"]
            self.assertIn("PATH 257 -9", log)
            self.assertIn("PATH 256 -9", log)                             # 255 bytes at most
            self.assertIn("PATH 255 0", log)

    def test_eight_files_and_eight_streams_at_once(self):
        p = package("locks", """
class T : public enaga::App {
    uint8_t ring_[8][4096] = {};
    bool init() override {
        enaga::ui::Line l;
        for (int i = 0; i < 8; i++) { char path[] = "/SAMPLES/FILE0"; path[13] = char('0' + i);
            l.clear().add("FILE ").number(enaga::host().file_open(path, ENAGA_FILE_WRITE)); enaga::log(l); }
        for (int i = 0; i < 8; i++) { char path[] = "/SAMPLES/STRM0"; path[13] = char('0' + i);
            l.clear().add("STREAM ").number(enaga::host().stream_create(path, 0, ring_[i], 4096)); enaga::log(l); }
        return true;
    }
};
ENAGA_APP(T)
""")
        with self.emulate({"/ENAGA/APPS/LOCKS/APP.ENA": p}) as emu:
            log = emu.report()["log"]                                     # the last 16 lines
            self.assertEqual([line for line in log if line.startswith(("FILE ", "STREAM "))][-9:],
                             ["FILE 7"] + [f"STREAM {i}" for i in range(8)])

    def test_a_failed_save_keeps_the_copy_an_earlier_one_left(self):
        p = package("tmpfail", """
class T : public enaga::App {
    bool init() override {
        char small[32];
        enaga::ui::Line l;
        auto *b = new uint8_t[5 * 1024 * 1024]();
        l.add("SAVE ").number(enaga::files::save("/SAMPLES/STATE.BIN", b, 5 * 1024 * 1024)); enaga::log(l);
        l.clear().add("AFTER ").number(enaga::files::read_all("/SAMPLES/STATE.BIN", small, sizeof small)); enaga::log(l);
        delete[] b;
        return true;
    }
};
ENAGA_APP(T)
""")
        with self.emulate({"/ENAGA/APPS/TMP/APP.ENA": p, "/SAMPLES/STATE.BIN.TMP": b"last good state"}, ms=2000) as emu:
            log = emu.report()["log"]
            self.assertIn("SAVE -4", log)                                 # the card is full
            self.assertIn("AFTER 15", log)                                # the earlier copy is still there

    def test_a_large_package_loads(self):
        p = build_app.build(ENAGA / "examples" / "tone", paths.BUILD / "lifecycle" / "big", "BIG")["package"]
        files = {"/ENAGA/APPS/BIG/APP.ENA": padded(p, 8 << 20)}
        with self.emulate(files, ms=3000, card_mib=16) as emu:
            self.assertEqual(emu.word("kernel_crashed"), 0)
            self.assertEqual(emu.report()["app_status"], "APP RUNNING")

    def test_settings_left_with_f1_power_are_saved(self):
        tone = build_app.build(ENAGA / "examples" / "tone", paths.BUILD / "apps" / "tone", "TONE")["package"]
        with tempfile.TemporaryDirectory() as d:
            image = Path(d) / "card.img"
            card.make(image, 16, "fat", {"/ENAGA/APPS/TONE/APP.ENA": tone})
            from emulate import Emulation
            with Emulation() as emu:
                emu.cpu.sd_load(image.read_bytes())
                emu.cpu.gpio_input(3, 0, True)
                self.assertIsNone(emu.run(700 * MS)["error"])
                self.assertTrue(emu.report()["app_running"])
                self.chord(emu)
                self.press(emu, DOWN)                                     # SETTINGS
                self.press(emu, OK)
                self.press(emu, LEFT)                                     # VOLUME 10 -> 9
                self.chord(emu)                                           # back to the app, not by OK
                self.assertEqual(emu.word("menu_active"), 0)
                image.write_bytes(emu.cpu.sd_read())
            self.assertTrue(card.get(image, "/ENAGA/SETTINGS.TXT").startswith(b"VOLUME=9\n"))

    def test_din_clock_follows_midi_in(self):
        with self.emulate({"/ENAGA/SETTINGS.TXT": b"MIDI IN=USB\nCLOCK SRC=MIDI\n"}, ms=300) as emu:
            emu.cpu.midi_in(bytes([0xFA, 0xF8]))
            self.assertIsNone(emu.run(20 * MS)["error"])
            self.assertEqual(self.byte(emu, "running"), 0)


if __name__ == "__main__":
    unittest.main()
