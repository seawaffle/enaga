"""The debug port (firmware/usb/usb_debug.c, tools/usb_debug.py) on the emulator: a second USB MIDI cable carrying
Enaga's log, its state, the screen and memory words as SysEx, driven through tools/usb_debug.py's Client over the
emulator's USB host, while cable 0 stays the app's MIDI, intact; and keys, knobs and pads pressed from the computer.
Client's own checks run on a stand-in port without the emulator.

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest tests.test_usb_debug
"""
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENAGA = HERE.parent
sys.path.insert(0, str(ENAGA / "tools"))
sys.path.insert(0, str(HERE))

import build_app  # noqa: E402
import paths  # noqa: E402
import usb_debug  # noqa: E402

BOOT = 300_000_000
MS = 372_000
STEP1 = 32
EP_OUT, EP_IN = 0x01, 0x81


class EmulatorTransport:
    """The debug cable through the emulator's USB host; what arrives on cable 0 is kept in `midi`."""

    def __init__(self, emu):
        self.emu = emu
        self.unpacker = usb_debug.Unpacker()
        self.midi = b""                        # cable 0's packets, as they came
        self.emulated_ms = 0

    def clock(self):
        return self.emulated_ms / 1000

    def run_ms(self, ms):
        assert self.emu.run(int(ms * MS))["error"] is None
        self.emulated_ms += ms

    def send(self, sysex):
        result = self.emu.cpu.usb_out(EP_OUT, usb_debug.packets(1, sysex))
        assert result["status"] == "ok", result

    def receive(self, timeout):
        out, waited = [], 0.0
        while True:
            data = b""
            while True:
                r = self.emu.cpu.usb_in(EP_IN, 64, timeout_ms=1)
                if r["status"] != "ok" or not r["data"]:
                    break
                data += bytes(r["data"])
            for i in range(0, len(data), 4):
                if data[i] >> 4 == 0:
                    self.midi += data[i:i + 4]
            out += self.unpacker.feed(data).get(1, [])
            if out or waited >= timeout:
                return out
            self.run_ms(5)
            waited += 0.005


class Emulated(unittest.TestCase):
    APP = "midi"

    def setUp(self):
        from emulate import Emulation
        self.emu = Emulation(HERE / "apps" / self.APP)
        self.addCleanup(self.emu.close)
        self.assertIsNone(self.emu.run(BOOT)["error"])
        self.emu.cpu.usb_attach()
        self.config = self.emu.cpu.usb_enumerate()
        self.transport = EmulatorTransport(self.emu)
        self.client = usb_debug.Client(self.transport, clock=self.transport.clock)

    def press(self, step):
        self.emu.key(STEP1 + step - 1, True)
        self.transport.run_ms(20)
        self.emu.key(STEP1 + step - 1, False)
        self.transport.run_ms(10)


@unittest.skipUnless(paths.emulator_present() and build_app.tools_present(), "needs ENAGA_EMULATOR and the toolchain")
class UsbDebug(Emulated):
    def test_two_cables_and_hello(self):
        config = bytes(self.config["configuration"]) if isinstance(self.config, dict) else bytes(self.config)
        self.assertIn(bytes([6, 0x25, 0x01, 2, 1, 5]), config)          # MIDI OUT endpoint: 2 embedded jacks (1, 5)
        self.assertEqual(self.client.hello(), "ENAGA 0.3.0 2")

    def test_the_log_kept_and_as_it_happens(self):
        self.client.log(True)
        lines = self.client.log_lines(0.2)
        log = self.emu.report()["log"]
        self.assertEqual([text for _, text in lines][-len(log):], log[-len(lines):])
        self.assertEqual([s for s, _ in lines], list(range(lines[0][0], lines[0][0] + len(lines))))   # in order
        self.emu.cpu.usb_out(EP_OUT, bytes([0x09, 0x90, 0x3C, 0x64]))  # MIDI in on cable 0: the app logs it
        self.transport.run_ms(10)
        new = self.client.log_lines(0.2)
        self.assertEqual([text for _, text in new], ["M90 3C 64"])
        self.assertEqual(new[0][0], lines[-1][0] + 1)
        self.client.log(False)
        self.emu.cpu.usb_out(EP_OUT, bytes([0x09, 0x90, 0x3D, 0x64]))
        self.transport.run_ms(10)
        self.assertEqual(self.client.log_lines(0.1), [])

    def test_cable_0_stays_intact_while_the_log_streams(self):
        self.client.log(True)
        self.client.log_lines(0.2)
        self.press(1)                                                    # note on, to USB
        self.press(2)                                                    # a 300-byte SysEx and a clock
        for i in range(20):                                              # MIDI in: log lines, while it goes out
            self.emu.cpu.usb_out(EP_OUT, bytes([0x09, 0x90, i, 0x64]))
            self.transport.run_ms(2)
        lines = self.client.log_lines(0.3)
        self.assertEqual([t for _, t in lines if t.startswith("M90")], [f"M90 {i:02X} 64" for i in range(20)])
        midi = self.transport.midi
        packets = [midi[i:i + 4] for i in range(0, len(midi), 4)]
        self.assertEqual(packets[0], bytes([0x09, 0x90, 60, 100]))
        sysex = usb_debug.Unpacker().feed(midi).get(0, [])
        self.assertEqual(sysex, [bytes([0xF0] + [i & 0x7F for i in range(298)] + [0xF7])])
        self.assertIn(bytes([0x0F, 0xF8, 0, 0]), packets)

    def test_midi_waits_behind_little_debug_data(self):
        """During a screen (about 23 KB on the debug cable), a note on cable 0 waits behind at most DEBUG_SHARE bytes
        of it in TinyUSB's buffer and one packet in the FIFO: 48 event packets, not the buffer's 128 and more."""
        self.client.ask(usb_debug.SCREEN)
        self.transport.run_ms(5)                                         # the screen starts going
        self.press(1)                                                    # a note on, while nobody reads
        data = b""
        for _ in range(4000):
            r = self.emu.cpu.usb_in(EP_IN, 64, timeout_ms=1)
            if r["status"] == "ok" and r["data"]:
                data += bytes(r["data"])
            if bytes([0x09, 0x90, 60, 100]) in [data[i:i + 4] for i in range(0, len(data), 4)]:
                break
        packets = [data[i:i + 4] for i in range(0, len(data), 4)]
        ahead = packets.index(bytes([0x09, 0x90, 60, 100]))
        self.assertGreater(ahead, 0)                                     # the screen was going
        self.assertLessEqual(ahead, (128 + 64) // 4)

    def test_status(self):
        status = dict(line.split(" ", 1) for line in self.client.status())
        self.assertEqual(status["VERSION"], "0.3.0")
        self.assertEqual(status["APP_STATUS"], "APP RUNNING")
        self.assertEqual(int(status["LOG_COUNT"]), self.emu.word("log_count"))
        self.assertIn("USB_AUDIO_IN", status)

    def test_screen(self):
        from display import pixels
        frame = self.client.screen()
        shown = pixels(self.emu.cpu.oled_frame()[0])
        self.assertEqual([g for row in frame for g in row], [p // 17 for p in shown])
        self.assertTrue(usb_debug.png(frame).startswith(b"\x89PNG"))

    def test_peek(self):
        address = usb_debug.elf_symbol(paths.BUILD / "firmware" / "enaga.elf", "log_count")
        self.assertEqual(self.client.peek(address), [self.emu.word("log_count")])
        words = self.client.peek(address, 3)
        self.assertEqual(len(words), 3)
        with self.assertRaisesRegex(RuntimeError, "NOT ALLOWED"):
            self.client.peek(0x01E00400)                                 # the USB core: reads clear its interrupts
        with self.assertRaisesRegex(RuntimeError, "UNKNOWN"):
            self.client.ask(0x33)
            self.client.answer(0x33, lambda got: got)

    def test_a_computer_that_stops_reading_does_not_hold_midi(self):
        self.client.log(True)
        for i in range(40):                                              # lines pile up; nobody reads
            self.emu.cpu.usb_out(EP_OUT, bytes([0x09, 0x90, i, 0x64]))
            self.transport.run_ms(5)
        self.transport.run_ms(2500)                                      # past the debug port's patience
        self.transport.receive(0.05)                                     # the computer reads again
        self.transport.midi = b""
        self.press(1)
        self.transport.receive(0.05)
        self.assertIn(bytes([0x09, 0x90, 60, 100]), self.transport.midi)
        self.client.log_lines(0.1)                                       # whatever was under way
        self.emu.cpu.usb_out(EP_OUT, bytes([0x09, 0x90, 0x7F, 0x64]))    # the app logs a new line ...
        self.transport.run_ms(10)
        self.assertEqual(self.client.log_lines(0.1), [])                 # ... which does not come: the log stopped
        self.assertIn("M90 7F 64", self.emu.report()["log"])
        self.assertEqual(self.client.hello(), "ENAGA 0.3.0 2")           # and the port still answers

    def test_a_remote_key_is_a_key(self):
        """STEP 1 pressed from the computer sends the app's note, as the unit's own key does."""
        self.client.press("STEP1")
        self.transport.receive(0.05)
        self.assertIn(bytes([0x09, 0x90, 60, 100]), [self.transport.midi[i:i + 4]
                                                     for i in range(0, len(self.transport.midi), 4)])


@unittest.skipUnless(paths.emulator_present() and build_app.tools_present(), "needs ENAGA_EMULATOR and the toolchain")
class RemoteInput(Emulated):
    """Keys, knobs and pads from the debug port, seen by tests/apps/keys, which logs every event."""
    APP = "keys"

    def log(self):
        return self.emu.report()["log"]

    def marked(self):
        """The mark on the display (not on the canvas, which SCREEN shows)."""
        from display import pixels
        self.transport.run_ms(5)                                         # the frame's DMA
        shown = pixels(self.emu.cpu.oled_frame()[0])
        return all(shown[128 * y + x] == 255 for y in range(3) for x in range(125, 128))

    def test_keys_and_the_mark(self):
        self.client.key("STEP1", True)
        self.assertEqual(self.log()[-1], "K32 1")
        self.assertTrue(self.marked())                                   # the answer came after a frame
        frame = self.client.screen()
        self.assertFalse(all(frame[y][x] == 15 for y in range(3) for x in range(125, 128)))   # the app's frame
        self.client.key(32, False)
        self.assertEqual(self.log()[-1], "K32 0")
        self.client.press("F2", "F3")
        self.assertEqual(self.log()[-4:], ["K26 1", "K20 1", "K20 0", "K26 0"])
        self.client.release()
        self.assertFalse(self.marked())
        with self.assertRaisesRegex(KeyError, "not a key"):
            self.client.key("STEP17", True)

    def test_knobs(self):
        self.client.turn("VALUE", -3)
        self.client.turn("A", 200)
        self.assertEqual(self.log()[-2:], ["E4 -3", "E0 200"])

    def test_pads_and_pressure(self):
        self.client.pad(5, 90)
        self.client.pressure(5, 40)
        self.client.pad(5, 0)
        self.assertEqual(self.log()[-3:], ["P5 90", "A5 40", "P5 0"])
        with self.assertRaisesRegex(RuntimeError, "NOT ALLOWED"):
            self.client.pressure(5, 40)                                  # not held
        self.client.pad(14, 127)
        self.client.release()
        self.assertEqual(self.log()[-2:], ["P14 127", "P14 0"])

    def test_power_only_opens_the_menu(self):
        with self.assertRaisesRegex(RuntimeError, "NOT ALLOWED"):
            self.client.key("POWER", True)                               # alone it would switch the unit off
        self.client.key("F1", True)
        self.client.key("POWER", True)
        self.assertEqual(self.emu.word("menu_active"), 1)
        self.assertEqual(self.log()[-2:], ["K19 1", "K19 0"])            # the app let go of F1 as the menu opened
        self.transport.run_ms(1500)                                      # POWER held: still on
        self.assertEqual(self.emu.word("menu_active"), 1)
        self.client.release()
        self.client.press("F1", "POWER")
        self.assertEqual(self.emu.word("menu_active"), 0)

    def test_the_unit_takes_over(self):
        self.client.key("STEP1", True)
        self.client.pad(3, 64)
        self.emu.key(STEP1 + 1, True)                                    # STEP 2 on the unit
        self.transport.run_ms(30)
        self.assertEqual(self.log()[-3:], ["P3 0", "K32 0", "K33 1"])
        self.assertEqual(self.emu.word("remote_active"), 0)
        self.emu.key(STEP1 + 1, False)
        self.transport.run_ms(30)

    def test_a_hold_lasts_while_the_computer_asks(self):
        """A program killed while holding a key never lets go, and Windows keeps reading the port (unit session,
        2026-11-25): the unit lets go after 5 s without a request. Client.wait keeps a hold going."""
        self.client.key("STEP1", True)
        self.client.wait(7)                                              # HELLO each second
        self.assertEqual(self.log()[-1], "K32 1")
        self.assertEqual(self.emu.word("remote_active"), 1)
        self.client.messages(0)                                          # the HELLO answers are not left waiting
        self.transport.run_ms(4000)
        self.assertEqual(self.log()[-1], "K32 1")
        self.transport.run_ms(1500)                                      # 5 s without a request: let go
        self.assertEqual(self.log()[-1], "K32 0")
        self.assertEqual(self.emu.word("remote_active"), 0)

    def test_a_computer_gone_quiet_lets_go(self):
        self.client.key("STEP1", True)
        self.client.log(True)
        for i in range(10):                                              # log lines pile up; nobody reads
            self.client.ask(usb_debug.STATUS)
            self.transport.run_ms(5)
        self.transport.run_ms(2500)
        self.assertIn("K32 0", self.log())
        self.assertEqual(self.emu.word("remote_active"), 0)


class FakeTransport:
    """Messages handed out a batch per receive; what was sent is kept, and RELEASE answered."""

    def __init__(self, batches):
        self.batches, self.sent = list(batches), []

    def send(self, sysex):
        self.sent.append(bytes(sysex))
        if usb_debug.decode(sysex)[0] == usb_debug.RELEASE:
            self.batches.append([usb_debug.request(usb_debug.RELEASE)])

    def receive(self, timeout):
        return self.batches.pop(0) if self.batches else []


class ClientChecks(unittest.TestCase):
    def test_log_lines_keep_reading_past_other_messages(self):
        log = [usb_debug.request(usb_debug.LOG, usb_debug.number(n, 3) + f"LINE {n}".encode()) for n in range(3)]
        status = usb_debug.request(usb_debug.STATUS, b"X")
        client = usb_debug.Client(FakeTransport([[log[0], status], [log[1]], [log[2]]]))
        self.assertEqual(client.log_lines(0), [(0, "LINE 0")])
        self.assertEqual(client.log_lines(0), [(1, "LINE 1")])
        self.assertEqual(client.log_lines(0), [(2, "LINE 2")])
        self.assertEqual(client.waiting, [(usb_debug.STATUS, b"X")])

    def test_out_of_range_arguments_are_refused_before_sending(self):
        transport = FakeTransport([])
        client = usb_debug.Client(transport)
        for call in (lambda: client.turn("VALUE", 8192), lambda: client.turn("A", -8193),
                     lambda: client.peek(0x80000000, 33), lambda: client.peek(0x80000000, 0),
                     lambda: client.peek(1 << 32)):
            with self.assertRaises(ValueError):
                call()
        with self.assertRaises(KeyError):
            client.press("300")
        with self.assertRaises(KeyError):
            client.key(128, True)
        self.assertEqual(transport.sent, [])

    def test_a_short_peek_answer_is_an_error(self):
        answer = usb_debug.request(usb_debug.PEEK, usb_debug.number(0x80000000, 5) + usb_debug.number(7, 5) * 2)
        client = usb_debug.Client(FakeTransport([[answer]]))
        with self.assertRaisesRegex(RuntimeError, "2 of the 3 words"):
            client.peek(0x80000000, 3)

    def test_the_command_line_says_what_is_wrong(self):
        import contextlib
        import io
        client = usb_debug.Client(FakeTransport([]))
        for argv, message in ((["turn", "VALUE", "10000"], "-8192 to 8191"), (["peek", "0x80000000", "200"], "1 to 32"),
                              (["press", "300"], "not a key")):
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                original, usb_debug.open_port = usb_debug.open_port, lambda port: client
                try:
                    self.assertEqual(usb_debug.main(argv), 1)
                finally:
                    usb_debug.open_port = original
            self.assertIn(message, err.getvalue())


if __name__ == "__main__":
    unittest.main()
