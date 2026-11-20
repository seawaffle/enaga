"""MIDI on the DIN jacks (E4 step 3) on the emulator: the parser (running status, real-time inside messages,
system common, SysEx in chunks and cut short, active sensing), sending (every helper, a long SysEx with a clock
byte going ahead of it), and MIDI THRU.

Short cases go in through the emulator's unpaced UART0 receive path, in bursts of at most 16 bytes (the receive
FIFO). Long streams use its paced MIDI IN sender (`midi_in`: one byte per character time, as a real sender), the
OUT-to-IN cable (`midi_loopback`) and the transmit timestamps (`uart_sent(..., cycles=True)`).

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest tests.test_midi
"""
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENAGA = HERE.parent
sys.path.insert(0, str(ENAGA / "tools"))

import build_app  # noqa: E402
import paths  # noqa: E402

BOOT = 300_000_000
MS = 372_000
CHARACTER = 119_040                                  # cycles per byte at 31,250 baud 8N1
STEP1 = 32


@unittest.skipUnless(paths.emulator_present() and build_app.tools_present(), "needs ENAGA_EMULATOR and the toolchain")
class Midi(unittest.TestCase):
    def setUp(self):
        from emulate import Emulation
        self.emu = Emulation(HERE / "apps" / "midi")
        self.assertIsNone(self.emu.run(BOOT)["error"])
        self.logged = len(self.emu.report()["log"])

    def tearDown(self):
        self.emu.close()

    def receive(self, data):
        for i in range(0, len(data), 16):            # one FIFO's worth, then 6 ms: 16 bytes take 5.1 ms on the wire
            self.emu.cpu.uart_receive(0, bytes(data[i:i + 16]))
            self.assertIsNone(self.emu.run(6 * MS)["error"])

    def new_lines(self):                             # log lines since the last call (the log keeps the last 16)
        log = self.emu.report()["log"]
        total = self.emu.word("log_count")
        new = log[len(log) - min(len(log), total - self.logged):] if total > self.logged else []
        self.logged = total
        return new

    def press(self, step):
        self.emu.key(STEP1 + step - 1, True)
        self.assertIsNone(self.emu.run(20 * MS)["error"])
        self.emu.key(STEP1 + step - 1, False)

    def sent(self, since=0):
        data, total = self.emu.cpu.uart_sent(0, since)
        return data

    def test_parser(self):
        self.new_lines()
        # Running status across a clock byte; note on velocity 0 becomes note off.
        self.receive([0x90, 0x3C, 0x64, 0x3C, 0x00, 0xF8, 0x40, 0x7F])
        self.assertEqual(self.new_lines(), ["M90 3C 64", "M80 3C 00", "MF8 00 00", "M90 40 7F"])
        # Running status on a one-byte message; system common clears running status; undefined bytes dropped.
        self.receive([0xC0, 0x05, 0x06, 0xF2, 0x10, 0x20, 0x40, 0xF6, 0xF1, 0x05, 0xF4, 0x41, 0xF9, 0xB0, 0x07, 0x64])
        self.assertEqual(self.new_lines(), ["MC0 05 00", "MC0 06 00", "MF2 10 20", "MF6 00 00", "MF1 05 00",
                                            "MB0 07 64"])
        # Pitch bend, channel pressure, poly pressure; real-time in the middle of a message.
        self.receive([0xE3, 0x00, 0xFA, 0x40, 0xD3, 0x22, 0xA3, 0x3C, 0x11, 0xFC])
        self.assertEqual(self.new_lines(), ["MFA 00 00", "ME3 00 40", "MD3 22 00", "MA3 3C 11", "MFC 00 00"])

    def test_sysex_in_chunks_and_cut_short(self):
        self.new_lines()
        body = [i & 0x7F for i in range(300)]
        self.receive([0xF0] + body[:100] + [0xF8] + body[100:] + [0xF7])   # 302 bytes, a clock inside
        self.assertEqual(self.new_lines(), ["MF8 00 00", "S256 1", "S46 2"])
        self.receive([0xF0, 0x01, 0x02, 0x90, 0x3C, 0x64])             # a status byte cuts it
        self.assertEqual(self.new_lines(), ["S3 7", "M90 3C 64"])
        self.receive([0xF7, 0x3C, 0x00])                                # a stray F7 clears running status
        self.assertEqual(self.new_lines(), [])

    def test_active_sensing_timeout_sends_all_notes_off(self):
        self.new_lines()
        before = self.emu.word("midi_messages_in")
        self.receive([0xFE])
        self.assertIsNone(self.emu.run(200 * MS)["error"])
        self.receive([0xFE])                                            # kept alive: nothing yet
        self.assertIsNone(self.emu.run(200 * MS)["error"])
        self.assertEqual(self.emu.word("midi_messages_in"), before)
        self.assertIsNone(self.emu.run(200 * MS)["error"])              # over 300 ms since the last FE
        self.assertEqual(self.emu.word("midi_messages_in"), before + 16)
        self.assertEqual(self.new_lines()[-1], "MBF 7B 00")
        self.assertIsNone(self.emu.run(500 * MS)["error"])              # once only
        self.assertEqual(self.emu.word("midi_messages_in"), before + 16)

    def test_sending(self):
        self.press(1)
        self.assertIsNone(self.emu.run(10 * MS)["error"])
        self.assertEqual(self.sent(), bytes([0x90, 60, 100]))
        self.press(3)
        self.assertIsNone(self.emu.run(30 * MS)["error"])
        self.assertEqual(self.sent(3), bytes([0x80, 60, 0, 0xA0, 60, 33, 0xB0, 7, 100, 0xC0, 5, 0xD0, 44,
                                              0xE0, 0, 0, 0xF2, 300 & 0x7F, 300 >> 7, 0xF3, 3]))

    def test_a_clock_byte_goes_ahead_of_a_long_sysex(self):
        self.press(2)
        self.assertIsNone(self.emu.run(150 * MS)["error"])              # 301 bytes take 96 ms
        out = self.sent()
        self.assertEqual(len(out), 301)
        self.assertLessEqual(out.index(0xF8), 2)                        # behind at most the bytes already going
        rest = out[:out.index(0xF8)] + out[out.index(0xF8) + 1:]
        self.assertEqual(rest, bytes([0xF0] + [i & 0x7F for i in range(298)] + [0xF7]))

    def test_thru_copies_everything_but_sysex(self):
        self.emu.cpu.write(self.emu.names["midi_thru"], (1).to_bytes(4, "little"))
        self.receive([0x90, 0x3C, 0x64, 0x3C, 0x00, 0xF8, 0xF0, 0x01, 0x02, 0xF7, 0xB1, 0x07, 0x10])
        self.assertIsNone(self.emu.run(10 * MS)["error"])
        out = self.sent()                                               # the clock may go ahead (real-time)
        self.assertEqual(out.count(0xF8), 1)
        self.assertEqual(out.replace(b"\xf8", b""), bytes([0x90, 0x3C, 0x64, 0x90, 0x3C, 0x00, 0xB1, 0x07, 0x10]))


    def paced(self, data, extra_ms=10):              # through the paced sender, then until it has all arrived
        self.emu.cpu.midi_in(bytes(data))
        self.assertIsNone(self.emu.run(len(data) * CHARACTER + extra_ms * MS)["error"])
        self.assertEqual(self.emu.cpu.midi_state()["queued"], 0)

    def assert_nothing_lost(self):
        self.assertEqual(self.emu.cpu.midi_state()["overruns"], 0)
        self.assertEqual(self.emu.word("uart0_overruns"), 0)
        self.assertEqual(self.emu.word("uart0_ring_lost"), 0)

    def test_a_long_sysex_dump_at_full_rate(self):
        self.new_lines()
        body = [(i * 7) & 0x7F for i in range(1000)]
        self.paced([0xF0] + body + [0xF7])          # 1,002 bytes back to back, about 321 ms
        self.assertEqual(self.new_lines(), ["S256 1", "S256 0", "S256 0", "S234 2"])
        self.assert_nothing_lost()

    def test_a_dense_stream_of_notes_at_full_rate(self):
        before = self.emu.word("midi_messages_in")
        stream = [0x90]
        for i in range(200):                         # running status: 400 data bytes, 200 notes on and off
            stream += [36 + i % 48, 100 if i % 2 == 0 else 0]
        stream += [0xB0, 64, 127] * 50 + [0xF8] * 100
        self.paced(stream)
        self.assertEqual(self.emu.word("midi_messages_in") - before, 200 + 50 + 100)
        self.assertEqual(self.new_lines()[-1], "MF8 00 00")
        self.assert_nothing_lost()

    def test_sent_bytes_go_back_to_back_and_a_clock_cuts_in(self):
        self.press(2)                                # the 300-byte SysEx, then a clock byte
        self.assertIsNone(self.emu.run(150 * MS)["error"])
        data, total, cycles = self.emu.cpu.uart_sent(0, 0, cycles=True)
        self.assertEqual(len(data), 301)
        gaps = [b - a for a, b in zip(cycles, cycles[1:])]
        self.assertEqual(max(gaps), CHARACTER)       # no idle time between bytes: the queue keeps the line busy
        self.assertEqual(min(gaps), CHARACTER)
        self.assertLessEqual(data.index(0xF8), 2)

    def test_round_trip_through_a_cable(self):
        self.emu.cpu.midi_loopback(True)
        self.new_lines()
        self.press(1)                                # note on, key 60
        self.press(2)                                # the 300-byte SysEx and a clock byte
        self.press(3)                                # every helper once
        self.assertIsNone(self.emu.run(200 * MS)["error"])
        lines = self.new_lines()
        self.assertEqual(lines[0], "M90 3C 64")
        self.assertEqual([x for x in lines if x[0] == "S"], ["S256 1", "S44 2"])
        self.assertIn("MF8 00 00", lines)
        self.assertEqual(lines[-8:], ["M80 3C 00", "MA0 3C 21", "MB0 07 64", "MC0 05 00", "MD0 2C 00", "ME0 00 00",
                                      "MF2 2C 02", "MF3 03 00"])
        self.assert_nothing_lost()

if __name__ == "__main__":
    unittest.main()
