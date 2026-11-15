"""E1: the firmware boots on the emulator, loads an app from the card, services the audio interrupt, sends
display frames by EDMA, delivers key events, and recovers from apps that fail or hang in either context.

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest discover -s tests -p test_e1.py -v

Needs the tic6x GCC, host gcc and g++, and the SmplTrek emulator (README.md, "Testing without a unit"). Each
boot is about 300 million guest cycles (a few seconds). Without ENAGA_EMULATOR these tests are skipped.
"""
import cmath
import math
import struct
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENAGA = HERE.parent
sys.path.insert(0, str(ENAGA / "tools"))

import build_app  # noqa: E402
import paths  # noqa: E402
from emulate import Emulation  # noqa: E402

WORK_E1 = paths.BUILD / "tests"
BOOT = 300_000_000          # past card, app load and audio start
STEP1 = 32
BLOCK_CYCLES = 64 * 372_000_000 // 48_000
MS = 372_000


def samples(emu):
    """Captured output as signed 32-bit words, in slot order (even = slot 0)."""
    raw, info = emu.cpu.audio_output()
    words = struct.unpack(f"<{len(raw) // 4}i", raw)
    return words if info["first_capture_slot"] == 0 else words[1:]


def component(channel, period):
    total = sum(v * cmath.exp(-2j * math.pi * k / period) for k, v in enumerate(channel))
    return abs(total) * 2 / len(channel) / 2 ** 31


@unittest.skipUnless(build_app.tools_present(), "tic6x GCC not built (tools/build_tic6x_*.sh)")
@unittest.skipUnless(paths.emulator_present(), "ENAGA_EMULATOR not set to the SmplTrek emulator directory")
class E1(unittest.TestCase):
    def test_boots_without_card_and_runs_audio(self):
        with Emulation() as emu:
            result = emu.run(BOOT)
            self.assertIsNone(result["error"])
            report = emu.report()
            self.assertEqual(report["app_status"], "NO CARD")
            self.assertFalse(report["app_running"])
            self.assertEqual((report["audio_status"], report["audio_codec_errors"]), (0, 0))
            blocks, ticks = report["audio_blocks"], emu.word("watchdog_ticks")
            self.assertGreater(blocks, 100)
            self.assertEqual(report["interrupts"], blocks + ticks)   # audio blocks and the watchdog's tick
            emu.run(10 * BLOCK_CYCLES)
            self.assertIn(emu.report()["audio_blocks"] - blocks, range(9, 12))   # one interrupt per block
            emu.run(100 * MS)
            self.assertIn(emu.word("watchdog_ticks") - ticks, range(10, 13))    # one tick per 10 ms
            self.assertEqual(emu.report()["audio_overruns"], 0)
            self.assertTrue(all(v == 0 for v in samples(emu)[-4096:]))
            self.assertEqual(report["log"], ["ENAGA 0.1.0"])
            self.assertGreater(report["oled"]["complete_frames"], 0)

    def test_tone_app_plays_requested_note(self):
        with Emulation(ENAGA / "examples" / "tone") as emu:
            self.assertIsNone(emu.run(BOOT)["error"])
            report = emu.report()
            self.assertEqual((report["app_status"], report["app_name"]), ("APP RUNNING", "TONE"))
            self.assertEqual(report["log"], ["ENAGA 0.1.0", "tone ready", "TONE"])
            # The heap follows the image (4 KiB aligned) and runs to the end of the 128 MiB DDR.
            package = (paths.BUILD / "emulate" / "TONE.ENA").read_bytes()
            _, _, _, base, size, bss, *_ = build_app.HEADER.unpack_from(package)
            heap, heap_size = struct.unpack("<II", emu.cpu.read(emu.names["app_host"] + 8, 8))
            self.assertEqual(heap, (base + size + bss + 4095) & ~4095)
            self.assertEqual(heap + heap_size, 0xC8000000)
            self.assertGreater(heap_size, 123 * 1024 * 1024)
            emu.key(STEP1, True)
            emu.run(20_000_000)
            emu.key(STEP1, False)
            self.assertIsNone(emu.run(80_000_000)["error"])
            words = samples(emu)[-8192:]
            for channel in (words[0::2], words[1::2]):
                self.assertAlmostEqual(component(channel, 64), 0.25, places=4)    # 750 Hz at 48 kHz, level 25 %
                self.assertAlmostEqual(max(abs(v) for v in channel) / 2 ** 31, 0.25, places=4)
            report = emu.report()
            self.assertEqual(report["log"][-1], "note on")
            self.assertEqual(report["audio_overruns"], 0)
            self.assertGreater(report["audio_cycles_peak"], 0)
            emu.key(STEP1, True)                       # the same STEP again: note off
            emu.run(20_000_000)
            emu.key(STEP1, False)
            emu.run(40_000_000)
            self.assertTrue(all(v == 0 for v in samples(emu)[-4096:]))

    def crash(self, key, reason, exception=False, wait=60_000_000):
        with Emulation(HERE / "apps" / "crash") as emu:
            self.assertIsNone(emu.run(BOOT)["error"])
            words = samples(emu)[-1024:]
            self.assertTrue(all(abs(v / 2 ** 31 - 0.5) < 1e-6 for v in words))   # the app's constant level
            emu.key(key, True)
            emu.run(20_000_000)
            emu.key(key, False)
            self.assertIsNone(emu.run(wait)["error"])
            report = emu.report()
            self.assertFalse(report["app_running"])
            self.assertEqual((report["app_status"], report["crash_reason"]), ("APP STOPPED", reason))
            self.assertEqual(report["log"][-2:], ["FATAL", reason])
            self.assert_recovered(emu)
            if exception:                                               # EFR.SXF, PC inside the app window
                pc = emu.word("crash_pc")
                self.assertEqual((emu.word("crash_efr"), emu.word("crash_ierr")), (1, 0))
                self.assertTrue(build_app.BASE <= pc < build_app.LIMIT, hex(pc))
            else:
                self.assertEqual(emu.word("crash_efr"), 0)

    def assert_recovered(self, emu):
        """After an app was dropped: Enaga's loop runs, the audio interrupt keeps running, output is silent."""
        self.assertFalse(emu.word("kernel_crashed"))
        self.assertEqual((emu.word("irq_active"), emu.word("audio_in_callback"), emu.word("app_busy")), (0, 0, 0))
        blocks, frames = emu.report()["audio_blocks"], emu.word("oled_dma_frames")
        self.assertIsNone(emu.run(60_000_000)["error"])
        expected = 60_000_000 // BLOCK_CYCLES
        self.assertIn(emu.report()["audio_blocks"] - blocks, range(expected - 3, expected + 3))
        self.assertGreaterEqual(emu.word("oled_dma_frames") - frames, 2)   # the main loop draws again
        self.assertTrue(all(v == 0 for v in samples(emu)[-4096:]))

    def test_interrupts_preserve_loop_state(self):
        # Regression: the ISR once saved ILC at *SP, which the C handler's prologue overwrote; an audio
        # interrupt between an inner loop's MVC to ILC and its SPLOOP then restored garbage into ILC.
        import zlib
        data = bytes((i * 7 + (i >> 9)) & 255 for i in range(256 * 1024))
        with Emulation(HERE / "apps" / "loops") as emu:
            self.assertIsNone(emu.run(BOOT + 100_000_000)["error"])
            report = emu.report()
            self.assertEqual(report["app_status"], "APP RUNNING")
            self.assertIn(f"CRC {zlib.crc32(data):08X}", report["log"])
            self.assertIn("BIG OK", report["log"])
            self.assertGreater(report["interrupts"], 100)
            disassembly = (emu.firmware["elf"].parent / "enaga.dis").read_text()
            self.assertIn("sploop", disassembly)   # the loader's own CRC loop is a SPLOOP too

    def test_fatal_in_main_context_returns_to_enaga(self):
        self.crash(STEP1, "main crash")

    def test_fatal_in_audio_callback_returns_to_enaga(self):
        self.crash(STEP1 + 1, "audio crash")

    def test_cpu_exception_in_main_context_drops_the_app(self):
        self.crash(STEP1 + 2, "CPU EXCEPTION", exception=True)

    def test_cpu_exception_in_audio_callback_drops_the_app(self):
        self.crash(STEP1 + 3, "CPU EXCEPTION", exception=True)

    def test_watchdog_drops_an_app_hung_in_main_context(self):
        self.crash(STEP1 + 4, "APP NOT RESPONDING", wait=1100 * MS)

    def test_watchdog_drops_an_app_hung_in_audio_callback(self):
        self.crash(STEP1 + 5, "APP HUNG IN AUDIO", wait=150 * MS)

    def test_watchdog_drops_an_app_hung_in_init(self):
        with Emulation(HERE / "apps" / "hang") as emu:
            self.assertIsNone(emu.run(BOOT + 1100 * MS)["error"])
            report = emu.report()
            self.assertFalse(report["app_running"])
            self.assertEqual((report["app_status"], report["crash_reason"]), ("APP STOPPED", "APP NOT RESPONDING"))
            self.assertEqual(report["log"][-3:], ["hang init", "FATAL", "APP NOT RESPONDING"])
            self.assert_recovered(emu)

    def test_watchdog_shows_the_crash_screen_when_enaga_hangs(self):
        # Fault injection: a branch to itself in unused Enaga RAM, reached through Enaga's own pad callback.
        raw = self.assemble("hang", "0:\tb .S2 0b\n\tnop 5\n")
        target = 0xC0300000
        with Emulation() as emu:
            self.assertIsNone(emu.run(BOOT)["error"])
            emu.cpu.write(target, raw)
            emu.cpu.write(emu.names["pad_callback"], struct.pack("<I", target))
            emu.cpu.uart_receive(1, bytes([0xB0, 0x10, 0x00]))
            self.assertIsNone(emu.run(900 * MS)["error"])
            self.assertEqual(emu.word("kernel_crashed"), 0)              # not before the second is up
            self.assertIsNone(emu.run(200 * MS)["error"])
            self.assertEqual(emu.word("kernel_crashed"), 1)
            self.assertEqual(emu.report()["log"][-2:], ["KERNEL CRASH", "WATCHDOG"])

    def assemble(self, name, text):
        source = WORK_E1 / f"{name}.s"
        source.parent.mkdir(parents=True, exist_ok=True)
        source.write_text("\t.text\n" + text)
        obj, raw = source.with_suffix(".o"), source.with_suffix(".bin")
        build_app.run([paths.tool("as"), "-mlittle-endian", "-march=c674x", "-o", obj, source], source.parent)
        build_app.run([build_app.OBJCOPY, "-O", "binary", "-j", ".text", obj, raw], source.parent)
        return raw.read_bytes()

    def test_cpu_exception_in_enaga_shows_the_crash_screen(self):
        # Fault injection: an SWE packet in unused Enaga RAM, reached through Enaga's own pad callback.
        raw = self.assemble("swe", "\tswe\n" + "\tnop\n" * 7)
        target = 0xC0300000
        with Emulation() as emu:
            self.assertIsNone(emu.run(BOOT)["error"])
            emu.cpu.write(target, raw)
            emu.cpu.write(emu.names["pad_callback"], struct.pack("<I", target))
            emu.cpu.uart_receive(1, bytes([0xB0, 0x10, 0x00]))   # a pad 0 reading
            self.assertIsNone(emu.run(40_000_000)["error"])
            report = emu.report()
            record = struct.unpack("<6I", emu.cpu.read(emu.names["exception_record"], 24))
            self.assertEqual(emu.word("kernel_crashed"), 1)
            self.assertEqual((record[0], record[1], record[2]), (target + 4, 1, 0))   # NRP after SWE, SXF
            self.assertEqual(report["log"][-2:], ["KERNEL CRASH", "SOFTWARE EXCEPTION"])
            blocks = report["audio_blocks"]
            emu.run(10_000_000)
            self.assertEqual(emu.report()["audio_blocks"], blocks)   # interrupts stay off
            self.assertTrue(all(v == 0 for v in samples(emu)[-1024:]))
            # The crash screen is a whole frame at the panel's origin: a frame in flight was allowed to finish
            # (cutting it short would leave the panel's write position mid-frame and shift the screen).
            frame, oled = emu.cpu.oled_frame()
            self.assertEqual(oled["cursor"], 0)
            self.assertEqual(frame, emu.cpu.read(emu.names["framebuffer"], 8192))
            # Waiting for the power key: every sampled PC lies in the crash loop or what it calls.
            functions = sorted((address, name) for name, address in emu.names.items() if address >= 0xC0000000)
            def function_at(pc):
                return max((entry for entry in functions if entry[0] <= pc), default=(0, "?"))[1]
            seen = set()
            for _ in range(20):
                emu.run(100_003)
                # During a return's delay slots the core's PC already names the next fetch address.
                seen.add(function_at(emu.cpu.state()["pc"] - 4))
            self.assertLessEqual(seen, {"kernel_crash", "power_key_down", "gpio_read_bank16", "tsc_read"})

    def test_display_frames_go_out_by_edma(self):
        with Emulation(ENAGA / "examples" / "tone") as emu:
            self.assertIsNone(emu.run(BOOT)["error"])
            dma = emu.word("oled_dma_frames")
            emu.run(100_000_000)                                     # about 270 ms: five 50 ms frames
            self.assertGreaterEqual(emu.word("oled_dma_frames") - dma, 4)
            self.assertEqual(emu.word("oled_dma_timeouts"), 0)
            frame, _ = emu.cpu.oled_frame()
            self.assertEqual(frame, emu.cpu.read(emu.names["framebuffer"], 8192))   # the panel got the last frame


if __name__ == "__main__":
    unittest.main()
