"""USB MIDI (E4 step 5) on the emulator's USB0 model (USB DRIVE, the device's second function: test_usb_drive.py) and scripted full-speed host: enumeration and descriptors,
MIDI both ways (channel messages, SysEx, real-time) in USB MIDI event packets, the MIDI IN / MIDI OUT routing
settings, MIDI clock in and out over USB, and unplugging.

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest tests.test_usb
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
MS = 372_000
STEP1 = 32
EP_OUT, EP_IN = 0x01, 0x81
DIN, USB, BOTH = 0, 1, 2                           # MIDI IN / MIDI OUT setting values


@unittest.skipUnless(paths.emulator_present() and build_app.tools_present(), "needs ENAGA_EMULATOR and the toolchain")
class Usb(unittest.TestCase):
    def start(self, app="midi", bootloader_file="SmplTrek.bin"):
        from emulate import Emulation
        self.emu = Emulation(HERE / "apps" / app, bootloader_file=bootloader_file)
        self.addCleanup(self.emu.close)
        self.assertIsNone(self.emu.run(BOOT)["error"])
        self.emu.cpu.usb_attach()
        return self.emu.cpu.usb_enumerate()

    def setting(self, name, value):
        self.emu.cpu.write(self.emu.names[name], value.to_bytes(4, "little"))

    def run_ms(self, ms):
        self.assertIsNone(self.emu.run(int(ms * MS))["error"])

    def press(self, step):
        self.emu.key(STEP1 + step - 1, True)
        self.run_ms(20)
        self.emu.key(STEP1 + step - 1, False)
        self.run_ms(10)

    def lines(self, prefix):
        return [x for x in self.emu.report()["log"] if x.startswith(prefix)]

    def out(self, *packets):                       # USB MIDI event packets, 4 bytes each
        result = self.emu.cpu.usb_out(EP_OUT, bytes(b for p in packets for b in p))
        self.assertEqual(result["status"], "ok")
        self.run_ms(5)

    def take(self):                                # everything the device has queued on its IN endpoint
        data = b""
        while True:
            r = self.emu.cpu.usb_in(EP_IN, 64, timeout_ms=5)
            if r["status"] != "ok" or not r["data"]:
                return data
            data += bytes(r["data"])

    def test_enumerates_as_a_usb_midi_device(self):
        r = self.start()
        self.assertEqual((r["vendor"], r["product"]), (0xCAFE, 0x4011))   # TinyUSB's test IDs until pid.codes
        self.assertEqual([(e["address"], e["type"], e["max_packet"]) for e in r["endpoints"]],
                         [(EP_OUT, "bulk", 64), (EP_IN, "bulk", 64), (0x03, "isochronous", 196),
                          (0x83, "isochronous", 196), (0x02, "bulk", 64), (0x82, "bulk", 64)])   # MIDI, audio, drive
        config = r["configuration"]
        self.assertEqual(config[9 + 5:9 + 7], bytes([1, 1]))              # interface 0: audio control
        self.assertIn(bytes([9, 4, 1, 0, 2, 1, 3]), config)               # interface 1: MIDI streaming, 2 endpoints
        self.assertIn(bytes([9, 4, 4, 0, 2, 8, 6, 0x50]), config)         # interface 4: mass storage (2, 3: audio)
        product = self.emu.cpu.usb_control((0x80, 6, 0x0302, 0x0409), 64)
        self.assertEqual(bytes(product["data"][2::2]).decode(), "Enaga SmplTrek")
        s = self.emu.cpu.usb_state()
        self.assertEqual((s["configuration"], s["stalls"], s["errors"], s["toggle_errors"]), (1, 0, 0, 0))
        # Full speed, as the descriptors are written: POWER's HSENAB (on after the core's reset, in the emulator
        # too) must be off, or a computer runs the unit at high speed (unit session 6). The emulator's host only
        # sends full-speed traffic, but counts the bus resets that a high-speed host would have chirped on.
        self.assertEqual(self.emu.cpu.read(0x01E00401, 1)[0] & 0x70, 0x40)   # SOFTCONN on, HSENAB and HSMODE off
        self.assertEqual(s.get("high_speed_resets", 0), 0)

    def test_a_refused_request_stalls_and_the_device_answers_on(self):
        # A request the device does not know must stall in every shape, not leave the computer waiting: with no
        # data stage, with data from the computer, and with data to it. (The first two once timed out: the
        # MUSB port's stall left the SETUP's RXRDY parked, dcd_musb.c dcd_edpt_stall.)
        self.start()
        for setup, data in [((0x40, 0x55, 0, 0), None), ((0x41, 0x55, 0, 1), b"\1\2\3"),
                            ((0xC0, 0x55, 0, 0), 4), ((0x21, 0x0B, 1, 0), None)]:
            result = self.emu.cpu.usb_control(setup, data)
            self.assertEqual(result["status"], "stall", setup)
            product = self.emu.cpu.usb_control((0x80, 6, 0x0302, 0x0409), 64)   # and the next request works
            self.assertEqual(product["status"], "ok", setup)

    def test_product_name_follows_the_device(self):
        self.start(bootloader_file="Lofi-12 XT.bin")
        product = self.emu.cpu.usb_control((0x80, 6, 0x0302, 0x0409), 64)
        self.assertEqual(bytes(product["data"][2::2]).decode(), "Enaga Lofi-12 XT")

    def test_midi_in_over_usb(self):
        self.start()
        self.out([0x09, 0x90, 0x3C, 0x64], [0x08, 0x80, 0x3C, 0x00], [0x0B, 0xB2, 0x07, 0x50], [0x0E, 0xE1, 0x00, 0x40],
                 [0x0F, 0xF8, 0, 0], [0x09, 0x90, 0x40, 0x00])
        self.assertEqual(self.lines("M")[-6:], ["M90 3C 64", "M80 3C 00", "MB2 07 50", "ME1 00 40", "MF8 00 00",
                                                "M80 40 00"])
        sysex = [0xF0] + [i & 0x7F for i in range(298)] + [0xF7]          # 300 bytes: CIN 4 x 99, then CIN 6
        packets = [[0x04] + sysex[i:i + 3] for i in range(0, 297, 3)] + [[0x06, sysex[297], sysex[298], 0]]
        packets[-1] = [0x07, sysex[297], sysex[298], sysex[299]]
        self.out(*packets)
        self.assertEqual(self.lines("S")[-2:], ["S256 1", "S44 2"])

    def test_midi_out_over_usb_and_the_routing(self):
        self.start()
        self.press(1)                                                     # note on, to both by default
        self.assertEqual(self.take(), bytes([0x09, 0x90, 60, 100]))
        self.assertEqual(self.emu.cpu.uart_sent(0)[0], bytes([0x90, 60, 100]))
        self.press(2)                                                     # 300-byte SysEx, then a clock
        self.run_ms(120)                                                  # the DIN copy takes 96 ms
        data = self.take()
        packets = [data[i:i + 4] for i in range(0, len(data), 4)]
        self.assertIn(bytes([0x0F, 0xF8, 0, 0]), packets)
        sysex = b"".join(p[1:] if p[0] in (4, 7) else p[1:3] if p[0] == 6 else p[1:2] if p[0] == 5 else b""
                         for p in packets)
        self.assertEqual(sysex, bytes([0xF0] + [i & 0x7F for i in range(298)] + [0xF7]))
        self.setting("midi_out_setting", 1)                              # MIDI OUT = DIN only
        din = self.emu.cpu.uart_sent(0)[1]
        self.press(1)
        self.assertEqual(self.take(), b"")
        self.assertEqual(self.emu.cpu.uart_sent(0, din)[0], bytes([0x90, 60, 100]))
        self.setting("midi_out_setting", 2)                              # MIDI OUT = USB only
        din = self.emu.cpu.uart_sent(0)[1]
        self.press(1)
        self.assertEqual(self.take(), bytes([0x09, 0x90, 60, 100]))
        self.assertEqual(self.emu.cpu.uart_sent(0, din)[0], b"")

    def test_a_host_that_stops_reading_does_not_hold_up_din(self):
        self.start()                                                      # MIDI OUT = BOTH; the host never reads
        din = self.emu.cpu.uart_sent(0)[1]
        for _ in range(12):                                               # 12 x 301 bytes: USB's 2 KiB fills
            self.press(2)
            self.run_ms(120)                                              # each takes 96 ms on the DIN wire
        self.assertEqual(self.emu.cpu.uart_sent(0, din)[0].count(0xF0), 12)   # DIN got every one
        self.assertGreater(self.emu.word("usb_tx_lost"), 0)              # USB dropped what did not fit
        self.take()                                                       # the host reads again: USB carries on
        self.press(1)
        self.assertEqual(self.take(), bytes([0x09, 0x90, 60, 100]))

    def test_midi_in_routing(self):
        self.start()
        self.setting("midi_in_setting", 1)                               # MIDI IN = DIN only: USB ignored
        before = self.emu.word("midi_messages_in")
        self.out([0x09, 0x90, 0x3C, 0x64])
        self.assertEqual(self.emu.word("midi_messages_in"), before)
        self.emu.cpu.midi_in(bytes([0x90, 0x3D, 0x64]))
        self.run_ms(5)
        self.assertEqual(self.lines("M")[-1], "M90 3D 64")

    def test_midi_clock_in_and_out_over_usb(self):
        self.start("clock")
        self.setting("clock_source_setting", 1)                          # follow MIDI clock (from USB here)
        self.out([0x0F, 0xFA, 0, 0])
        for _ in range(48):                                               # two beats at 100 BPM
            self.out([0x0F, 0xF8, 0, 0])
            self.run_ms(25 - 5)
        self.press(7)
        tempo, running = re.fullmatch(r"B(\d+) R(\d)", self.lines("B")[-1]).groups()
        self.assertEqual(running, "1")
        self.assertAlmostEqual(int(tempo) / 10, 100, delta=2)            # USB clock is read in the main loop
        self.press(6)
        n, gaps = re.fullmatch(r"N(\d+) G(\d+) L\d+", self.lines("N")[-1]).groups()
        self.assertEqual(gaps, "0")
        self.assertTrue(180 <= int(n) <= 192, n)
        self.out([0x0F, 0xFC, 0, 0])
        self.setting("clock_source_setting", 0)                          # the app's clock, sent out over USB too
        self.setting("clock_out_setting", 1)
        self.take()
        self.press(1)
        self.run_ms(200)
        data = self.take()
        packets = [data[i:i + 4] for i in range(0, len(data), 4)]
        self.assertEqual(packets[0], bytes([0x0F, 0xFA, 0, 0]))
        self.assertTrue(9 <= packets.count(bytes([0x0F, 0xF8, 0, 0])) <= 12)   # 48 a second

    def test_unplugging_and_plugging_back(self):
        self.start()
        self.emu.cpu.usb_detach()
        self.run_ms(20)
        self.press(1)                                                     # sending with no host is harmless
        self.assertEqual(self.emu.word("usb_tx_lost"), 0)
        self.assertEqual(self.emu.cpu.uart_sent(0)[0], bytes([0x90, 60, 100]))
        self.emu.cpu.usb_attach()
        self.emu.cpu.usb_enumerate()
        self.press(1)
        self.assertEqual(self.take(), bytes([0x09, 0x90, 60, 100]))       # nothing stale from the unplugged time

    def test_no_module_is_touched_while_powered_off(self):
        """No access to a peripheral whose PSC module is not enabled, through boot, USB (attach, enumeration, MIDI,
        audio streaming, suspend, detach), the input routes, power-off and a USB DRIVE session: TinyUSB's dcd_init
        used to touch the USB0 core before its PHY hook powered it (found by diffing against the stock's writes)."""
        import register_capture
        from emulate import Emulation
        with Emulation(HERE / "apps" / "midi") as emu:
            if not hasattr(emu.cpu, "psc_violations") or not hasattr(emu.cpu, "capture_start"):
                self.skipTest("the emulator has no PSC violation log")
        for scenario in ("main", "drive"):
            result = register_capture.capture(scenario)
            self.assertIsNone(result["fault"], scenario)
            for phase in result["phases"]:
                self.assertEqual(phase["psc_violations"], [], f'{scenario} {phase["name"]}')


if __name__ == "__main__":
    unittest.main()
