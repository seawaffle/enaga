"""examples/recorder on the emulator: a known pattern on the input, recorded with OK to /RECORD/TAKE001.WAV and on
(the first free name each time) in each of its formats, F1 choosing between takes: every sample in the file is the
input's, in order on each channel, converted as wav::Writer converts (24-bit: the word's top 24 bits; float: the word / 2^31;
16-bit: rounded to the top 16), with the header's size matching. A take running when the unit switches off is closed
by stop(), a whole WAV file.

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest tests.test_recorder_example
"""
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
F1, OK = 19, 23


def word(v):                                    # a 24-bit value as the codec's 32-bit word (low byte zero)
    return (v & 0xFFFFFF) << 8


# 64 frames, each channel different and covering both signs; words with a zero low byte, so the 24-bit file holds
# them exactly. The extremes are in: -2^23 and 2^23 - 1.
LEFT = [((k * 0x2F1D3 + 0x10000) & 0xFFFFFF) for k in range(64)]
RIGHT = [((k * 0x51A07 + 0x7F0000) & 0xFFFFFF) for k in range(64)]
LEFT[5], RIGHT[9] = 0x800000, 0x7FFFFF
SIGNED = lambda v: v - (1 << 24) if v & 0x800000 else v  # noqa: E731
PATTERN = [(SIGNED(l), SIGNED(r)) for l, r in zip(LEFT, RIGHT)]


def parse(data):
    """(format tag, channels, rate, bits, samples as (left, right) tuples of ints or floats)."""
    assert data[:4] == b"RIFF" and data[8:16] == b"WAVEfmt " and data[36:40] == b"data", data[:44]
    tag, channels, rate, _, _, bits = struct.unpack("<HHIIHH", data[20:36])
    size = struct.unpack("<I", data[40:44])[0]
    assert struct.unpack("<I", data[4:8])[0] == 36 + size
    body = data[44:44 + size]
    assert len(body) == size, (len(body), size)
    width = bits // 8
    values = []
    for i in range(0, size, width):
        chunk = body[i:i + width]
        if tag == 3:
            values.append(struct.unpack("<f", chunk)[0])
        else:
            values.append(int.from_bytes(chunk, "little", signed=True))
    return tag, channels, rate, bits, list(zip(values[0::2], values[1::2]))


def expected(bits, tag):
    if tag == 3:
        return [(l * 256 / 2 ** 31, r * 256 / 2 ** 31) for l, r in PATTERN]
    if bits == 24:
        return PATTERN
    def to16(v):                                # x 32768 rounded to nearest: v / 256, half up, saturated
        return max(-32768, min(32767, (v + 128) >> 8))
    return [(to16(l), to16(r)) for l, r in PATTERN]


@unittest.skipUnless(paths.emulator_present() and build_app.tools_present(), "needs ENAGA_EMULATOR and the toolchain")
class RecorderExample(unittest.TestCase):
    def setUp(self):
        from emulate import Emulation
        self.emu = Emulation(ENAGA / "examples" / "recorder")
        self.addCleanup(self.emu.close)
        frames = b"".join(struct.pack("<II", word(r), word(l)) for l, r in zip(LEFT, RIGHT))   # (right, left) a frame
        self.emu.cpu.audio_input(frames, loop=True)
        self.assertIsNone(self.emu.run(BOOT)["error"])
        self.assertIn("recorder ready", self.emu.report()["log"])

    def run_ms(self, ms):
        self.assertIsNone(self.emu.run(int(ms * MS))["error"])

    def press(self, key):
        self.emu.key(key, True)
        self.run_ms(20)
        self.emu.key(key, False)
        self.run_ms(20)

    def check_take(self, name, bits, tag, at_least_s):
        data = self.emu.card_file(f"/RECORD/{name}")
        self.assertIsNotNone(data, name)
        got_tag, channels, rate, got_bits, samples = parse(data)
        self.assertEqual((got_tag, channels, rate, got_bits), (tag, 2, 48000, bits), name)
        self.assertGreater(len(samples), at_least_s * 48000, name)
        want = expected(bits, tag)
        # Each channel, in order from where the take began. Enaga's left is the codec's slot 1 and its right the
        # next codec frame's slot 0 (docs/hardware.md "Channels"), so on the emulator the right channel is one
        # input frame ahead of the left.
        for c in (0, 1):
            got, ref = [s[c] for s in samples], [w[c] for w in want]
            start = ref.index(got[0])
            if c == 0:
                left_start = start
            else:
                self.assertEqual(start, (left_start + 1) % 64, f"{name}: the right channel's place")
            wrong = [i for i, v in enumerate(got) if v != ref[(start + i) % 64]]
            self.assertEqual(wrong[:5], [], f"{name} channel {c}: {len(wrong)} of {len(got)} samples differ")
        return len(samples)

    def test_takes_in_each_format(self):
        for name, f1_presses, bits, tag in (("TAKE001.WAV", 0, 24, 1), ("TAKE002.WAV", 1, 32, 3),
                                            ("TAKE003.WAV", 1, 16, 1)):
            with self.subTest(take=name):
                for _ in range(f1_presses):
                    self.press(F1)
                self.press(OK)
                self.run_ms(500)
                self.press(OK)
                self.run_ms(50)
                self.check_take(name, bits, tag, 0.4)
        self.assertIn("TAKE003.WAV", self.emu.report()["log"])

    def test_switching_off_closes_the_take(self):
        self.press(OK)
        self.run_ms(300)
        self.emu.power_off()
        self.check_take("TAKE001.WAV", 24, 1, 0.2)


if __name__ == "__main__":
    unittest.main()
