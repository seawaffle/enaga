"""The tempo clock, transport and SYNC (E4 step 4) on the emulator: the app's internal clock with MIDI clock and
SYNC OUT pulses timed from it, Song Position Pointer before CONTINUE, following MIDI clock and following SYNC IN
pulses (and bridging them to MIDI clock), and the app's ticks arriving in order whatever the source.

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest tests.test_clock
"""
import re
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENAGA = HERE.parent
sys.path.insert(0, str(ENAGA / "tools"))

import build_app  # noqa: E402
import paths  # noqa: E402

BOOT = 300_000_000
CPS = 372_000_000
MS = CPS // 1000
STEP1 = 32
BLOCK_MS = 64 / 48                                   # outputs land on audio blocks: late by less than one


@unittest.skipUnless(paths.emulator_present() and build_app.tools_present(), "needs ENAGA_EMULATOR and the toolchain")
class Clock(unittest.TestCase):
    def setUp(self):
        from emulate import Emulation
        self.emu = Emulation(HERE / "apps" / "clock")
        self.assertIsNone(self.emu.run(BOOT)["error"])

    def tearDown(self):
        self.emu.close()

    def setting(self, name, value):
        self.emu.cpu.write(self.emu.names[name], value.to_bytes(4, "little"))

    def run_ms(self, ms):
        self.assertIsNone(self.emu.run(int(ms * MS))["error"])

    def press(self, step):
        self.emu.key(STEP1 + step - 1, True)
        self.run_ms(10)
        self.emu.key(STEP1 + step - 1, False)
        self.run_ms(10)

    def lines(self, prefix):
        return [x for x in self.emu.report()["log"] if x.startswith(prefix)]

    def counts(self):                                # (ticks, gaps, last tick) from the app
        self.press(6)
        n, g, last = re.fullmatch(r"N(\d+) G(\d+) L(\d+)", self.lines("N")[-1]).groups()
        return int(n), int(g), int(last)

    def tempo(self):
        self.press(7)
        b, r = re.fullmatch(r"B(\d+) R(\d)", self.lines("B")[-1]).groups()
        return int(b) / 10, int(r)

    def sent(self):
        data, _, cycles = self.emu.cpu.uart_sent(0, 0, cycles=True)
        return data, cycles

    def test_internal_clock_drives_midi_clock_and_sync_out(self):
        self.setting("clock_out_setting", 1)
        self.press(1)                                # START at 120 BPM
        self.run_ms(2000)
        self.press(2)                                # STOP
        self.run_ms(20)
        data, cycles = self.sent()
        self.assertEqual(data[0], 0xFA)
        self.assertEqual(data[-1], 0xFC)
        clocks = [c for b, c in zip(data, cycles) if b == 0xF8]
        gaps = [(b - a) / MS for a, b in zip(clocks, clocks[1:])]
        self.assertTrue(95 <= len(clocks) <= 99, len(clocks))          # 48 a second
        self.assertAlmostEqual(sum(gaps) / len(gaps), 125 / 6, delta=0.02)   # exact on average
        self.assertTrue(all(abs(g - 125 / 6) < BLOCK_MS + 0.1 for g in gaps), gaps)
        rows, _ = self.emu.cpu.gpio_trace()
        rises = [r["cycles"] for r in rows if r["bank"] == 7 and r["pin"] == 9 and r["high"]]
        falls = [r["cycles"] for r in rows if r["bank"] == 7 and r["pin"] == 9 and not r["high"]][1:]   # [0]: set-up
        self.assertTrue(8 <= len(rises) <= 9, len(rises))              # 2 a beat
        for a, b in zip(rises, rises[1:]):
            self.assertLess(abs((b - a) / MS - 250), BLOCK_MS + 0.1)
        for up, down in zip(rises, falls):
            self.assertLess(abs((down - up) / MS - 15), BLOCK_MS + 0.1)
        n, g, last = self.counts()
        self.assertEqual(g, 0)
        self.assertEqual(n, last + 1)                                  # ticks 0..last, none missing
        self.assertTrue(380 <= n <= 392, n)                            # 192 a second
        transport = self.lines("T")
        self.assertEqual(len(transport), 2)
        self.assertEqual(transport[0], "T1 0")
        self.assertTrue(transport[1].startswith("T2 "))

    def test_continue_sends_song_position_first_and_locate_while_stopped(self):
        self.setting("clock_out_setting", 1)
        self.press(1)
        self.run_ms(500)
        self.press(2)
        _, _, last = self.counts()
        total = self.emu.cpu.uart_sent(0, 0)[1]
        self.press(3)                                # CONTINUE: SPP (sixteenths, 24 ticks each), then FB
        self.run_ms(5)
        data = self.emu.cpu.uart_sent(0, total)[0]
        resume = last + 1
        self.assertEqual(data[:4], bytes([0xF2, (resume // 24) & 0x7F, (resume // 24) >> 7, 0xFB]))
        self.run_ms(200)
        self.press(2)
        total = self.emu.cpu.uart_sent(0, 0)[1]
        self.press(4)                                # locate to tick 384 (16 sixteenths) while stopped
        self.run_ms(5)
        self.assertEqual(self.emu.cpu.uart_sent(0, total)[0], bytes([0xF2, 16, 0]))
        self.assertEqual(self.lines("T")[-1], "T4 384")
        self.press(5)                                # tempo is the app's to set with the internal clock
        self.assertEqual(self.lines("C5")[-1], "C5 0")
        self.assertEqual(self.tempo(), (150.0, 0))

    def midi_clocks(self, bpm, count):
        for _ in range(count):
            self.emu.cpu.midi_in(b"\xf8")
            self.run_ms(60_000 / (bpm * 24))

    def test_following_midi_clock(self):
        self.setting("clock_source_setting", 1)
        self.midi_clocks(100, 24)                    # clock while stopped: the tempo is known before START
        self.press(1)                                # the app cannot drive an outside clock
        self.assertEqual(self.lines("C1")[-1], "C1 -1")
        self.emu.cpu.midi_in(b"\xfa")
        self.midi_clocks(100, 48)                    # two beats
        tempo, running = self.tempo()
        self.assertEqual(running, 1)
        self.assertAlmostEqual(tempo, 100, delta=0.5)
        n, g, last = self.counts()
        self.assertEqual(g, 0)
        self.assertTrue(188 <= n <= 192, n)          # 4 ticks per clock, never past the next clock's
        self.emu.cpu.midi_in(b"\xfc")
        self.run_ms(10)
        self.emu.cpu.midi_in(bytes([0xF2, 8, 0]))    # song position: 8 sixteenths, tick 192
        self.run_ms(10)
        self.emu.cpu.midi_in(b"\xfb")
        self.midi_clocks(100, 24)
        transport = self.lines("T")
        self.assertEqual(transport[0], "T1 0")
        self.assertTrue(transport[-3].startswith("T2 "))
        self.assertEqual(transport[-2:], ["T4 192", "T3 192"])        # located by the SPP, then continued
        n2, g2, last2 = self.counts()
        self.assertEqual(g2, 0)
        self.assertTrue(192 + 92 <= last2 <= 192 + 96, last2)        # resumed at 192

    def test_following_sync_in_and_bridging_it_to_midi_clock(self):
        self.setting("clock_source_setting", 2)
        self.setting("clock_out_setting", 1)
        self.emu.cpu.sync_in(90, 8)                  # 8 pulses at 90 BPM, 2 a beat: 333 ms apart
        self.run_ms(7 * 1000 / 3 + 100)
        tempo, running = self.tempo()
        self.assertEqual(running, 1)
        self.assertAlmostEqual(tempo, 90, delta=0.5)
        self.run_ms(1000)                            # no more pulses: stops after 2.5 intervals
        self.assertEqual(self.tempo()[1], 0)
        transport = self.lines("T")
        self.assertEqual(transport[0], "T1 0")
        self.assertTrue(transport[-1].startswith("T2 "))
        n, g, last = self.counts()
        self.assertEqual(g, 0)
        self.assertTrue(7 * 48 <= n <= 8 * 48, n)    # 48 per pulse; waits at the eighth pulse's 48
        data, _ = self.sent()
        self.assertEqual(data.count(0xF8), (n + 3) // 4)              # MIDI clock every 4 ticks
        self.assertEqual(data[-1], 0xFC)


if __name__ == "__main__":
    unittest.main()
