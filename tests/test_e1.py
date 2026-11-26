"""E1: the firmware boots on the emulator, loads an app from the card, services the audio interrupt, sends
display frames by EDMA, delivers key events, and recovers from apps that fail or hang in either context.

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest discover -s tests -p test_e1.py -v

Needs the tic6x GCC, host gcc and g++, and the SmplTrek emulator (README.md, "Testing without a unit"). Each
boot is about 300 million guest cycles (a few seconds). Without ENAGA_EMULATOR these tests are skipped.
"""
import cmath
import itertools
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
CACHE_LOG = "CACHE 043 8F>444 FF"   # the bootloader's hand-over as measured (smpltrek-re session M1), then after set-up


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
    def test_the_whole_runs_audio_is_kept(self):
        """keep_audio collects past the emulator's 0.68 s ring: the tone example for 1.5 s, every word in order (the
        last stretch is what the ring still holds) and left and right in step."""
        with Emulation(ENAGA / "examples" / "tone", keep_audio=True) as emu:
            self.assertIsNone(emu.run(BOOT + 1500 * MS)["error"])
            raw, info = emu.cpu.audio_output()
            self.assertEqual(emu.audio_lost, 0)
            self.assertEqual(len(emu.audio) // 4, info["transmitted_words"])
            self.assertEqual(bytes(emu.audio[-len(raw):]), raw)
            wav, wav_info = emu.audio_wav()
            self.assertGreater(wav_info["frames"], 2 * 32767)
            self.assertEqual(emu.audio_slot, (info["first_capture_slot"] + info["transmitted_words"]
                                              - info["captured_words"]) & 1)

    def test_device_from_the_bootloaders_file_name(self):
        """The device is told by the update file name its bootloader left in DDR (firmware/kernel/device.c)."""
        for file, device, name in [("Lofi-12 XT.bin", 2, "LOFI-12 XT"), ("CyDrums.bin", 3, "CYDRUMS"),
                                   ("SmplTrek.bin", 1, "SMPLTREK"), (None, 0, "UNKNOWN DEVICE"),
                                   ("SmplTrek.binX", 0, "UNKNOWN DEVICE")]:
            with self.subTest(file=file), Emulation(bootloader_file=file) as emu:
                self.assertIsNone(emu.run(BOOT)["error"])
                self.assertEqual(emu.word("device_id"), device)
                self.assertEqual(emu.report()["log"][2], name)

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
            self.assertEqual(report["log"], ["ENAGA 0.3.0", CACHE_LOG, "SMPLTREK"])
            self.assertGreater(report["oled"]["complete_frames"], 0)

    def test_cache_set_up_from_the_bootloaders_hand_over(self):
        # BOOT 1.0.17's state at the jump, from smpltrek-re's static trace and simulation (docs/hardware.md "Boot").
        cacheable = {17, 128, 192, 193, 194, 195, 199}
        with Emulation() as emu:
            put = lambda address, value: emu.cpu.write(address, value.to_bytes(4, "little"))
            get = lambda address: int.from_bytes(emu.cpu.read(address, 4), "little")
            put(0x01840000, 0)
            put(0x01840020, 4)
            put(0x01840040, 3)
            for n in range(16, 256):
                put(0x01848000 + 4 * n, int(n in cacheable))
            self.assertIsNone(emu.run(BOOT)["error"])
            self.assertEqual(emu.report()["log"][:2], ["ENAGA 0.3.0", "CACHE 043 8F>444 FF"])
            self.assertEqual([get(0x01840000) & 7, get(0x01840020), get(0x01840040)], [4, 4, 4])
            self.assertEqual([n for n in range(16, 256) if get(0x01848000 + 4 * n)], list(range(192, 200)))
            self.assertGreater(emu.report()["audio_blocks"], 100)

    def test_tone_app_plays_requested_note(self):
        with Emulation(ENAGA / "examples" / "tone") as emu:
            self.assertIsNone(emu.run(BOOT)["error"])
            report = emu.report()
            self.assertEqual((report["app_status"], report["app_name"]), ("APP RUNNING", "TONE"))
            self.assertEqual(report["log"], ["ENAGA 0.3.0", CACHE_LOG, "SMPLTREK", "tone ready", "TONE"])
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

    def test_long_callback_never_tears_a_block(self):
        # A callback that runs 2.5 or 3.5 blocks lets the ring finish two or three blocks whose completions
        # merge into one pending bit. Counting completions then refills the buffer that is playing, and a
        # played block comes out half old, half new; the buffer is taken from the PaRAM instead.
        with Emulation(HERE / "apps" / "stall") as emu:
            self.assertIsNone(emu.run(BOOT)["error"])
            for key in (STEP1, STEP1 + 1):
                blocks = emu.report()["audio_blocks"]
                emu.key(key, True)
                emu.run(20 * MS)
                emu.key(key, False)
                self.assertIsNone(emu.run(20 * MS)["error"])
                self.assertIn(emu.report()["audio_blocks"] - blocks, range(29, 32))    # replayed blocks count
                runs = [len(list(run)) for _, run in itertools.groupby(samples(emu)[-6000:])]
                self.assertEqual(set(runs[1:-1]), {128}, key)                       # every block one level
            self.assertGreaterEqual(emu.report()["audio_overruns"], 2)

    def test_output_words_are_the_samples_clipped(self):
        """Each sample goes out as x * 2^31 truncated, +-1 and past it (and infinities) at full scale, NaN as
        silence: the clip app's edge cases and random bit patterns, word for word."""
        edges = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x3F7FFFFF, 0xBF7FFFFF, 0x3F800001, 0xBF800001,
                 0x3F000000, 0xBF000000, 0x30000000, 0xB0000000, 0x30400000, 0x2FFFFFFF, 0x00000001, 0x807FFFFF,
                 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000, 0x7F800001, 0x7F7FFFFF, 0xFF7FFFFF, 0x40000000]
        patterns, s = [], 1
        for i in range(128):
            s = (s * 1664525 + 1013904223) & 0xFFFFFFFF
            patterns.append(edges[i] if i < len(edges) else s)

        def word(bits):
            x = struct.unpack("<f", struct.pack("<I", bits))[0]
            if math.isnan(x):
                return 0
            if x >= 1.0:
                return 2 ** 31 - 1
            if x <= -1.0:
                return -2 ** 31
            return int(x * 2 ** 31) if abs(x) >= 2 ** -126 else 0     # the C674x reads denormals as zero
        want = [word(bits) for bits in patterns]
        with Emulation(HERE / "apps" / "clip") as emu:
            self.assertIsNone(emu.run(BOOT)["error"])
            self.assertEqual(emu.report()["log"][-2], "clip ready")
            got = samples(emu)[-4096:]
            start = next((k for k in range(128) if list(got[k:k + 128]) == want), None)
            self.assertIsNotNone(start, "no block of the expected words")
            self.assertEqual(list(got[start:start + 3840]), want * 30)

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

    def test_edma_state_left_by_the_bootloader_does_not_storm(self):
        # The bootloader copies Enaga by EDMA. A completion it leaves pending and enabled used to re-raise the
        # audio interrupt from every IEVAL: 658,000 interrupts a second on the first unit run (2026-11-18).
        # Simulated here: EDMA powered, one finished transfer on channel 20, pending and enabled in region 1.
        edma, psc0, ch = 0x01C00000, 0x01C10000, 20
        with Emulation() as emu:
            put = lambda address, value: emu.cpu.write(address, struct.pack("<I", value))
            put(psc0 + 0xA00, 3); put(psc0 + 0x120, 1)
            self.assertIsNone(emu.run(10_000)["error"])
            param = edma + 0x4000 + 0x20 * ch
            for offset, value in ((0, 1 << 20 | ch << 12), (4, 0xC0380000), (8, 1 << 16 | 4), (12, 0xC0380100),
                                  (16, 0), (20, 0xFFFF), (24, 0), (28, 1)):
                put(param + offset, value)
            put(edma + 0x348, 1 << ch); put(edma + 0x1060, 1 << ch); put(edma + 0x1010, 1 << ch)  # DRAE1, IESR, ESR
            self.assertIsNone(emu.run(BOOT)["error"])
            interrupts = struct.unpack("<16I", emu.cpu.read(emu.names["irq_count"], 64))[8]
            self.assertGreater(emu.word("audio_blocks"), 300)
            self.assertLessEqual(interrupts, emu.word("audio_blocks") + 2)    # one interrupt per block
            self.assertGreater(emu.word("kernel_passes"), 300)
            self.assertEqual(emu.word("kernel_crashed"), 0)

    def test_interrupts_leave_a_just_released_frame_alone(self):
        # GCC releases pick()'s frame before its last reads of the array in it (tests/apps/stack). Before the
        # interrupt entry skipped 16 KiB below SP, this run gave 6 wrong results in 2 million calls.
        with Emulation(HERE / "apps" / "stack") as emu:
            self.assertIsNone(emu.run(BOOT + 600_000_000)["error"])
            report = emu.report()
            self.assertEqual(report["app_status"], "APP RUNNING")
            self.assertGreater(report["interrupts"], 1000)
            self.assertEqual(report["log"][-1], "STACK WRONG 0")

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
            crash_loop = {"kernel_crash", "power_key_down", "gpio_read_bank16", "tsc_read"}
            for _ in range(20):
                emu.run(100_003)
                # During a return's delay slots the core's PC already names the next fetch address, but a PC at a
                # function's first packet belongs to that function: either reading may be the right one.
                pc = emu.cpu.state()["pc"]
                self.assertTrue({function_at(pc), function_at(pc - 4)} & crash_loop, hex(pc))

    def key_events(self, emu):
        return [line for line in emu.report()["log"] if line[:1] == "K" and line[1:2].isdigit()]

    def bounce(self, emu, key, settle_down, pulses=4):
        """Contact bounce: the key flips every 0.5 ms a few times before it settles at settle_down."""
        for i in range(pulses):
            emu.key(key, i % 2 == 0 if settle_down else i % 2 == 1)
            emu.run(MS // 2)
        emu.key(key, settle_down)

    def test_bouncing_key_gives_one_press_and_one_release(self):
        with Emulation(HERE / "apps" / "keys") as emu:
            self.assertIsNone(emu.run(BOOT)["error"])
            self.bounce(emu, STEP1, True)
            emu.run(50 * MS)
            self.assertEqual(self.key_events(emu), ["K32 1"])
            self.bounce(emu, STEP1, False)
            emu.run(50 * MS)
            self.assertEqual(self.key_events(emu), ["K32 1", "K32 0"])

    def test_press_is_taken_on_the_first_closed_sample(self):
        with Emulation(HERE / "apps" / "keys") as emu:
            self.assertIsNone(emu.run(BOOT)["error"])
            emu.key(STEP1, True)
            emu.run(7 * MS)                     # one 6 ms sample interval, plus the scan
            self.assertEqual(self.key_events(emu), ["K32 1"])

    def test_one_sample_opening_while_held_is_ignored(self):
        # The original firmware's STEP keys release on the first open sample, so this gave a release and a new
        # press there. Samples are taken on the first main-loop pass 6 ms after the last; passes come with the
        # audio blocks, so on the emulator the gaps alternate 5.25 and 6.75 ms, a pattern that repeats every 12 ms.
        # 3 ms openings 10 ms apart: a gap of 6.75 ms cannot reach from one opening into the next (7 ms), so no two
        # consecutive samples are open; and each opening falls 2 ms further along the 12 ms pattern, so of twelve,
        # several are sampled. (9 ms apart, a 6.75 ms gap could catch two in a row, a real release; 12 ms apart,
        # every opening falls at the same phase and none may be sampled.)
        with Emulation(HERE / "apps" / "keys") as emu:
            self.assertIsNone(emu.run(BOOT)["error"])
            emu.key(STEP1, True)
            emu.run(20 * MS)
            for _ in range(12):
                emu.key(STEP1, False)
                emu.run(3 * MS)
                emu.key(STEP1, True)
                emu.run(7 * MS)
            emu.run(20 * MS)
            self.assertEqual(self.key_events(emu), ["K32 1"])
            emu.key(STEP1, False)
            emu.run(20 * MS)                    # two open samples: 6-12 ms
            self.assertEqual(self.key_events(emu), ["K32 1", "K32 0"])

    def test_short_press_and_quick_repeats_are_all_reported(self):
        with Emulation(HERE / "apps" / "keys") as emu:
            self.assertIsNone(emu.run(BOOT)["error"])
            emu.key(STEP1, True)                # a 13 ms press spans at least two samples
            emu.run(13 * MS)
            emu.key(STEP1, False)
            emu.run(30 * MS)
            for _ in range(3):                  # a 25 Hz roll on one key
                emu.key(STEP1, True)
                emu.run(20 * MS)
                emu.key(STEP1, False)
                emu.run(20 * MS)
            self.assertEqual(self.key_events(emu), ["K32 1", "K32 0"] * 4)

    def test_pads_give_press_and_release_with_velocity(self):
        def reading(pad, value):
            emu.cpu.uart_receive(1, bytes([0xB0 | pad, value & 0x7F, value >> 7]))
            emu.run(4 * MS)                     # the MCU reads each pad about every 3.8 ms
        with Emulation(HERE / "apps" / "keys") as emu:
            self.assertIsNone(emu.run(BOOT)["error"])
            # 1000 (PADS MEDIUM) starts a press, sent at the next report with velocity from the higher of the two:
            # 1 + 126 (peak - 1000) / 2300, 127 from 3300. Under 750 a release; a tap that falls under 750 at that
            # next report still gives its press first.
            for value in (90, 999, 1200, 2150, 900, 760, 749, 300, 3000, 2000, 0, 1500, 0, 16383, 16383, 0):
                reading(3, value)
            reading(12, 1000)
            pads = [line for line in emu.report()["log"] if line[:1] == "P"]
            self.assertEqual(pads, ["P3 64", "P3 0", "P3 110", "P3 0", "P3 28", "P3 0", "P3 127", "P3 0"])
            reading(12, 1000)
            pads = [line for line in emu.report()["log"] if line[:1] == "P"]
            self.assertEqual(pads[-1], "P12 1")

    def test_pad_pressure_after_the_hit_settles(self):
        def reading(pad, value):
            emu.cpu.uart_receive(1, bytes([0xB0 | pad, value & 0x7F, value >> 7]))
            emu.run(4 * MS)
        with Emulation(HERE / "apps" / "keys") as emu:
            self.assertIsNone(emu.run(BOOT)["error"])
            # A hit at 2000 settles at 1200: its 40th report (1200) is the baseline. 1350 is not 200 above it, so no
            # pressure; 2500 turns it on. The cube curve (threshold 1000 to 3300) gives 0 for 1200 and 35 for 2500,
            # so the mean of the last 9 climbs 35 k / 9 as 2500s replace 1200s; 2520 (36) moves it by only 1.
            # No pressure after the release.
            for value in [2000] + [1200] * 45 + [1350] * 3 + [2500] * 12 + [2520] * 3 + [0, 2500, 0]:
                reading(5, value)
            pads = [line for line in emu.report()["log"] if line[:1] in "PA"]
            self.assertEqual(pads, ["P5 55"] + [f"A5 {35 * k // 9}" for k in range(1, 10)]
                             + ["P5 0", "P5 83", "P5 0"])

    # Panel LED addresses (a, b, c), transcribed from the smpltrek-re notes (docs/hardware.md "Panel MCU"):
    # pads by MCU channel as (red, green, blue); STEP and button LEDs as their green address (red at c + 1).
    LED_PADS = [((2, 1, 5), (2, 1, 4), (2, 1, 6)), ((2, 1, 2), (2, 1, 1), (2, 1, 3)), ((0, 1, 5), (0, 1, 4), (0, 1, 6)),
                ((0, 1, 2), (0, 1, 1), (0, 1, 3)), ((1, 1, 5), (1, 1, 4), (1, 1, 6)), ((1, 1, 2), (1, 1, 1), (1, 1, 3)),
                ((2, 2, 4), (2, 2, 3), (2, 2, 5)), ((0, 2, 7), (0, 2, 6), (0, 1, 0)), ((1, 2, 4), (1, 2, 3), (1, 2, 5)),
                ((1, 2, 7), (1, 2, 6), (1, 1, 0)), ((0, 2, 4), (0, 2, 3), (0, 2, 5)), ((2, 2, 7), (2, 2, 6), (2, 1, 0)),
                ((0, 2, 1), (0, 2, 0), (0, 2, 2)), ((2, 2, 1), (2, 2, 0), (2, 2, 2)), ((1, 2, 1), (1, 2, 0), (1, 2, 2))]
    LED_STEPS = [(1, 3, 0), (1, 3, 2), (0, 3, 2), (0, 3, 4), (0, 3, 6), (0, 4, 0), (1, 4, 2), (1, 4, 4),
                 (2, 5, 4), (0, 3, 0), (2, 5, 6), (1, 3, 4), (1, 3, 6), (1, 4, 0), (0, 4, 2), (0, 4, 4)]
    LED_BUTTONS = [(0, 5, 2), (0, 5, 4), (1, 5, 0), (1, 5, 2), (1, 5, 4),     # F4 F7 F2 F3 F8
                   (0, 5, 0), (0, 5, 6), (1, 5, 6), (0, 4, 6), (1, 4, 6)]     # F1 F6 F5 CLR OK

    @staticmethod
    def led_walk():
        """What each step of tests/apps/leds lights: {address n = 48a + 8b + c: level}."""
        n = lambda a, b, c: 48 * a + 8 * b + c
        row0 = lambda c, v: {n(a, 0, c): v for a in range(3)}           # all three a-planes
        walk = [{n(*r): 1, n(*g): 2, n(*b): 3} for r, g, b in E1.LED_PADS]
        walk += [row0(0, 3), row0(1, 3), row0(2, 3), {**row0(5, 1), **row0(4, 2), **row0(3, 3)}]
        walk += [{n(*g): 2, n(*g) + 1: 1} for g in E1.LED_STEPS + E1.LED_BUTTONS]
        return walk + [{}]                                               # OCTAVE LEFT: no LED

    def leds(self, emu):
        return emu.cpu.read(emu.names["led_level"], 144)

    def press(self, emu, key):
        emu.key(key, True)
        emu.run(15 * MS)
        emu.key(key, False)
        emu.run(15 * MS)

    def test_led_api_drives_the_mapped_addresses(self):
        walk = self.led_walk()
        used = set().union(*walk)
        self.assertEqual(len(used), 115)            # every address the original firmware drives, no other
        with Emulation(HERE / "apps" / "leds") as emu:
            self.assertIsNone(emu.run(BOOT)["error"])
            start = self.leds(emu)
            self.assertEqual([start[i] for i in sorted(used)], [0] * 115)    # switched off at start-up
            self.assertTrue(all(start[i] == 0xFF for i in range(144) if i not in used))   # never written
            for step, lit in enumerate(walk):
                self.press(emu, STEP1)
                expected = bytes(lit.get(i, 0 if i in used else 0xFF) for i in range(144))
                self.assertEqual(self.leds(emu), expected, f"walk step {step}")
            self.press(emu, STEP1 + 1)              # red 15 is clamped to the maximum, 10
            self.assertEqual(self.leds(emu)[48 * 2 + 8 + 5], 10)
            self.press(emu, STEP1 + 2)              # from the audio callback: ignored
            self.assertEqual([self.leds(emu)[i] for i in (106, 105, 107)], [0, 0, 0])
            self.press(emu, STEP1 + 3)              # the app is dropped: every LED off
            self.assertEqual(emu.report()["crash_reason"], "leds done")
            final = self.leds(emu)
            self.assertEqual([final[i] for i in sorted(used)], [0] * 115)

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
