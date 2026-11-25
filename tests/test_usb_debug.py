"""The debug port (firmware/usb/usb_debug.c, tools/usb_debug.py) on the emulator: a second USB MIDI cable carrying
Enaga's log, its state, the screen and memory words as SysEx, driven through tools/usb_debug.py's Client over the
emulator's USB host, while cable 0 stays the app's MIDI, intact.

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


@unittest.skipUnless(paths.emulator_present() and build_app.tools_present(), "needs ENAGA_EMULATOR and the toolchain")
class UsbDebug(unittest.TestCase):
    def setUp(self):
        from emulate import Emulation
        self.emu = Emulation(HERE / "apps" / "midi")
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

    def test_two_cables_and_hello(self):
        config = bytes(self.config["configuration"]) if isinstance(self.config, dict) else bytes(self.config)
        self.assertIn(bytes([6, 0x25, 0x01, 2, 1, 5]), config)          # MIDI OUT endpoint: 2 embedded jacks (1, 5)
        self.assertEqual(self.client.hello(), "ENAGA 0.3.0 1")

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
        self.assertEqual(self.client.hello(), "ENAGA 0.3.0 1")           # and the port still answers


if __name__ == "__main__":
    unittest.main()
