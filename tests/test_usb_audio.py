"""USB audio (E5 step 4) on the emulator's isochronous streams: USB Audio Class 1.0 descriptors beside MIDI and the
drive, the computer's audio as the app's input (ENAGA_INPUT_USB), the app's output to the computer, a computer clock
that runs fast or slow against the codec, stopping the streams and unplugging.

The host streams as a computer does, without gaps: IN tokens are asked for well ahead (wait=False) and a few OUT
packets are kept queued beyond what has been played, then the guest runs and the IN packets are collected. (Asking
the emulator for a set number of frames and stopping there would leave a frame with no packet and no token at each
stop, which looks like a 2000 ppm clock.)

The app is tests/apps/input, which plays its input to its output: STEP 10 chooses the USB input, so what the computer
sends comes back to it. The right channel carries a counter, which shows dropped, repeated or reordered frames.

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest tests.test_usb_audio
"""
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

BOOT = 300_000_000
MS = 372_000
STEP10, STEP11 = 32 + 9, 32 + 10
EP_OUT, EP_IN = 0x03, 0x83
ITF_OUT, ITF_IN = 2, 3


def packets(start, sizes):
    """One packet per entry of `sizes` (frames): a 1 kHz sine at -12 dB left, a counter right."""
    out, n = [], start
    for size in sizes:
        out.append(b"".join(struct.pack("<hh", int(8192 * math.sin(2 * math.pi * 1000 * (n + i) / 48000)),
                                        (n + i) % 30000 + 1) for i in range(size)))
        n += size
    return out, n


def frames(rows):
    data = b"".join(bytes(r["data"]) for r in rows)
    return [struct.unpack_from("<hh", data, i) for i in range(0, len(data) - 3, 4)]


@unittest.skipUnless(paths.emulator_present() and build_app.tools_present(), "needs ENAGA_EMULATOR and the toolchain")
class UsbAudio(unittest.TestCase):
    def setUp(self):
        from emulate import Emulation
        self.emu = Emulation(HERE / "apps" / "input")
        self.addCleanup(self.emu.close)
        self.assertIsNone(self.emu.run(BOOT)["error"])
        self.emu.cpu.usb_attach()
        self.enumeration = self.emu.cpu.usb_enumerate()
        self.sent = 0

    def run_ms(self, ms):
        self.assertIsNone(self.emu.run(int(ms * MS))["error"])

    def usb_input(self):
        self.emu.key(STEP10, True)
        self.run_ms(20)
        self.emu.key(STEP10, False)
        self.run_ms(20)
        self.assertEqual(self.emu.report()["log"][-1], "I0")

    def start(self, out=True, into=True):
        if out:
            self.assertEqual(self.emu.cpu.usb_set_interface(ITF_OUT, 1)["status"], "ok")
            out_packets, self.sent = packets(self.sent, [48] * 4)          # kept in hand: the host never runs dry
            self.emu.cpu.usb_iso_stream_out(EP_OUT, out_packets)
        if into:
            self.assertEqual(self.emu.cpu.usb_set_interface(ITF_IN, 1)["status"], "ok")
            self.emu.cpu.usb_iso_stream_in(EP_IN, 100_000, wait=False)       # tokens every frame from now on

    def stream(self, sizes):
        """Queue one OUT packet per entry of `sizes` and run that many frames; the IN rows meanwhile."""
        if sizes:
            out, self.sent = packets(self.sent, sizes)
            self.emu.cpu.usb_iso_stream_out(EP_OUT, out)
        self.run_ms(len(sizes))
        return self.emu.cpu.usb_iso_collect(EP_IN)

    def listen(self, ms):                        # IN only
        self.run_ms(ms)
        return self.emu.cpu.usb_iso_collect(EP_IN)

    def counter_steps(self, rows):
        right = [f[1] for f in frames(rows) if f[1]]
        steps = [(b - a) % 30000 for a, b in zip(right, right[1:])]
        return {s: steps.count(s) for s in set(steps)}

    def word(self, name):
        return self.emu.word(name)

    def test_descriptors(self):
        eps = [(e["address"], e["type"], e["max_packet"], e["alternate"]) for e in self.enumeration["endpoints"]]
        self.assertIn((EP_OUT, "isochronous", 196, 1), eps)
        self.assertIn((EP_IN, "isochronous", 196, 1), eps)
        config = self.enumeration["configuration"]
        self.assertEqual(config[4], 5)                                          # five interfaces
        self.assertIn(bytes([11, 0x24, 0x01, 0x00, 0x01]), config)              # UAC 1.0 header
        self.assertIn(bytes([11, 0x24, 0x02, 1, 2, 2, 16, 1, 0x80, 0xBB, 0x00]), config)   # PCM 2 x 16 bit, 48 kHz
        self.assertIn(bytes([9, 5, EP_OUT, 0x09]), config)                      # isochronous, adaptive
        self.assertIn(bytes([9, 5, EP_IN, 0x05]), config)                       # isochronous, asynchronous
        self.assertIn(bytes([9, 4, 4, 0, 2, 8, 6, 0x50]), config)               # the drive is interface 4

    def test_sampling_rate_requests(self):
        # What computers send as a stream starts (UAC1 5.2.3.2.3.1): the endpoint's sampling frequency, 3 bytes.
        rate = (48000).to_bytes(3, "little")
        for ep in (EP_OUT, EP_IN):
            self.assertEqual(self.emu.cpu.usb_control((0x22, 0x01, 0x0100, ep), rate)["status"], "ok")   # SET_CUR
            for request in (0x81, 0x82, 0x83):                                  # GET_CUR, GET_MIN, GET_MAX
                result = self.emu.cpu.usb_control((0xA2, request, 0x0100, ep), 3)
                self.assertEqual((result["status"], bytes(result["data"])), ("ok", rate), hex(request))
        self.assertEqual(self.emu.cpu.usb_control((0x22, 0x01, 0x0100, EP_OUT),
                                                  (44100).to_bytes(3, "little"))["status"], "ok")   # taken, but
        self.assertIn("USB REQ 22 01 0100 0003", self.emu.report()["log"])     # logged: 48 kHz is the only rate
        self.assertEqual(self.emu.cpu.usb_control((0x22, 0x01, 0x0200, EP_OUT), b"\0")["status"], "stall")  # pitch
        self.assertIn("USB REQ 22 01 0200 0003", self.emu.report()["log"])  # what is refused, logged
        self.usb_input()                                                        # and streams still start
        self.start()
        rows = self.stream([48] * 300)
        self.assertGreater(sum(1 for r in rows if r["status"] == "data"), 290)

    def test_the_computers_audio_comes_back_through_the_app(self):
        self.usb_input()
        self.start()
        self.assertEqual(self.emu.report()["log"][-2:], ["USB AUDIO OUT ON", "USB AUDIO IN ON"])
        self.stream([48] * 2000)                                                # 2 s to settle
        rows = self.stream([48] * 1000)
        self.assertEqual({r["status"] for r in rows}, {"data"})
        self.assertEqual({len(r["data"]) for r in rows}, {192})                 # 48 frames a packet: clocks agree
        steps = self.counter_steps(rows)
        self.assertGreater(steps.get(1, 0), 0.999 * sum(steps.values()), steps)   # in order, none lost
        left = [f[0] / 32768 for f in frames(rows)]
        rms = math.sqrt(sum(v * v for v in left) / len(left))
        self.assertAlmostEqual(rms, 0.25 / math.sqrt(2), delta=0.005)           # the sine, at its level
        ratio = struct.unpack("<f", self.emu.cpu.read(self.emu.names["usb_audio_out_ratio"], 4))[0]
        self.assertAlmostEqual(ratio, 1.0, delta=0.0002)
        for name in ("usb_audio_out_overruns", "usb_audio_out_underruns", "usb_audio_in_late", "usb_audio_in_underruns"):
            self.assertEqual(self.word(name), 0, name)
        s = self.emu.cpu.usb_state()
        self.assertEqual((s["iso_errors"], s["iso_overruns"], s["iso_underruns"]), (0, 0, 0))

    def test_a_computer_clock_fast_or_slow(self):
        self.usb_input()
        self.start()
        for extra, sign in ((49, 1), (47, -1)):                                 # +-1 frame in 20: about 1000 ppm
            sizes = [extra if i % 20 == 0 else 48 for i in range(4000)]
            self.stream(sizes)
            rows = self.stream(sizes[:1000])
            ratio = struct.unpack("<f", self.emu.cpu.read(self.emu.names["usb_audio_out_ratio"], 4))[0]
            self.assertAlmostEqual(ratio, 1 + sign * 1 / 960, delta=0.0003)    # the read follows the computer
            steps = self.counter_steps(rows)
            self.assertGreater(steps.get(1, 0), 0.99 * sum(steps.values()), steps)
        self.assertEqual((self.word("usb_audio_out_overruns"), self.word("usb_audio_out_underruns")), (0, 0))

    def test_a_heavy_callback_does_not_cost_packets(self):
        self.usb_input()
        self.emu.key(STEP11, True)                                              # the callback takes ~86% of a block,
        self.run_ms(20)                                                         # holding off the USB interrupt
        self.emu.key(STEP11, False)
        self.start()
        self.stream([48] * 1000)
        late = self.word("usb_audio_in_late")                                   # a start-up token may find none yet
        rows = self.stream([48] * 1000)
        self.assertEqual(self.emu.word("audio_overruns"), 0)
        self.assertEqual(({r["status"] for r in rows}, {len(r["data"]) for r in rows}), ({"data"}, {192}))
        steps = self.counter_steps(rows)
        self.assertGreater(steps.get(1, 0), 0.999 * sum(steps.values()), steps)
        self.assertEqual((self.word("usb_audio_in_late") - late, self.word("usb_audio_out_overruns")), (0, 0))

    def test_the_output_goes_to_the_computer_without_the_usb_input(self):
        self.start(out=False)                                                   # input stays the line: silence
        rows = self.listen(200)
        self.assertGreaterEqual(sum(1 for r in rows if r["status"] == "data"), 198)
        self.assertEqual({len(r["data"]) for r in rows[5:]}, {192})
        self.assertFalse(any(any(f) for f in frames(rows)))
        self.assertGreaterEqual(self.word("usb_audio_in_packets"), 200)

    def test_stopping_the_stream_silences_the_input(self):
        self.usb_input()
        self.start()
        self.stream([48] * 500)
        self.assertEqual(self.emu.cpu.usb_set_interface(ITF_OUT, 0)["status"], "ok")
        self.assertIn("USB AUDIO OUT OFF", self.emu.report()["log"])
        rows = self.listen(200)
        self.assertFalse(any(any(f) for f in frames(rows[20:])))                 # once the ring has played out
        self.assertEqual(self.emu.cpu.usb_set_interface(ITF_IN, 0)["status"], "ok")
        before = self.word("usb_audio_in_packets")
        self.run_ms(50)
        self.assertEqual(self.word("usb_audio_in_packets"), before)             # nothing loaded once stopped

    def test_each_way_of_feeding_the_in_endpoint(self):
        # The STATUS page's LEFT / RIGHT choose how IN packets are loaded (usb_audio_in_mode), for trying on the
        # unit; each must stream cleanly here, and the stream's end logs its mode and counts.
        self.usb_input()
        for mode in (1, 2, 3, 0):
            self.emu.cpu.write(self.emu.names["usb_audio_in_mode"], mode.to_bytes(4, "little"))
            self.sent = 0
            self.start()
            self.stream([48] * 300)
            rows = self.stream([48] * 500)
            self.assertEqual(({r["status"] for r in rows}, {len(r["data"]) for r in rows}), ({"data"}, {192}), mode)
            steps = self.counter_steps(rows)
            self.assertGreater(steps.get(1, 0), 0.999 * sum(steps.values()), (mode, steps))
            for itf in (ITF_IN, ITF_OUT):
                self.assertEqual(self.emu.cpu.usb_set_interface(itf, 0)["status"], "ok")
            self.assertTrue(any(line.startswith(f"IN M{mode} L") and line.endswith(" U0")
                                for line in self.emu.report()["log"][-3:]), self.emu.report()["log"][-3:])

    def test_unplugging_while_streaming(self):
        self.usb_input()
        self.start()
        self.stream([48] * 300)
        self.emu.cpu.usb_detach()
        self.run_ms(50)
        self.emu.cpu.usb_attach()
        self.emu.cpu.usb_enumerate()
        self.sent = 0
        self.start()
        rows = self.stream([48] * 1000)
        self.assertGreater(sum(1 for r in rows if r["status"] == "data"), 990)
        self.assertTrue(any(f[1] for f in frames(rows)))                        # the computer's audio again


if __name__ == "__main__":
    unittest.main()
