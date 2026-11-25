"""enaga::wav::Loader and enaga::dsp::SamplePlayer on the emulator: tests/apps/sampler loads WAV files of every
format from the card (16-, 24- and 32-bit, 8-bit unsigned, float; mono and stereo; 22.05, 44.1, 48 and 96 kHz,
converted to 48 kHz or kept; a long chunk before the data; a header still saying 0 bytes; not a WAV; missing) and
logs what it got, then plays one; the test made the files, so it knows every frame.

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest tests.test_sampler
"""
import math
import struct
import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENAGA = HERE.parent
sys.path.insert(0, str(ENAGA / "tools"))
sys.path.insert(0, str(HERE))

import build_app  # noqa: E402
import paths  # noqa: E402
from test_e1 import component, samples  # noqa: E402

MS = 372_000
BOOT = 300_000_000
WORK = paths.BUILD / "sampler"


def signal(frame, rate, channel):
    """The files' content: 1 kHz, 0.5 sine on the left (or the only channel), 0.25 cosine on the right."""
    phase = 2 * math.pi * 1000 * frame / rate
    return 0.5 * math.sin(phase) if channel == 0 else 0.25 * math.cos(phase)


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def encode(value, bits, floating):
    """(bytes, the value read back as Enaga's conversion gives it)."""
    if floating:
        return struct.pack("<f", value), f32(value)
    if bits == 8:
        u = max(0, min(255, round(value * 128) + 128))
        return bytes([u]), (u - 128) / 128
    scale = 1 << (bits - 1)
    i = max(-scale, min(scale - 1, round(value * scale)))
    return (i & ((1 << bits) - 1)).to_bytes(bits // 8, "little"), i / scale


def make_wav(channels, rate, bits, seconds, floating=False, junk=0, size_field=None, trailing=b""):
    """(file bytes, frames as read back: a list of per-channel values)."""
    frames = int(rate * seconds)
    data, decoded = bytearray(), []
    for f in range(frames):
        row = []
        for c in range(channels):
            raw, back = encode(signal(f, rate, c), bits, floating)
            data += raw
            row.append(back)
        decoded.append(row)
    block = channels * bits // 8
    fmt = struct.pack("<HHIIHH", 3 if floating else 1, channels, rate, rate * block, block, bits)
    chunks = b""
    if junk:
        chunks += b"JUNK" + struct.pack("<I", junk) + bytes(junk)
    chunks += b"fmt " + struct.pack("<I", len(fmt)) + fmt
    chunks += b"data" + struct.pack("<I", len(data) if size_field is None else size_field) + bytes(data) + trailing
    return b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks, decoded


# n: (file, expected rate in the sample, resampled?)
CASES = {
    0: (dict(channels=2, rate=44100, bits=16, seconds=0.5), 48000, True),
    1: (dict(channels=1, rate=48000, bits=24, seconds=0.25), 48000, False),
    2: (dict(channels=2, rate=96000, bits=32, seconds=0.25, floating=True), 48000, True),
    3: (dict(channels=1, rate=22050, bits=8, seconds=0.5), 48000, True),
    6: (dict(channels=1, rate=48000, bits=16, seconds=0.25, junk=40000), 48000, False),
    7: (dict(channels=1, rate=44100, bits=16, seconds=0.5), 44100, False),    # loaded keeping its rate
    8: (dict(channels=2, rate=48000, bits=32, seconds=0.25, size_field=0, trailing=b"\x01\x02\x03"), 48000, False),
}


@unittest.skipUnless(paths.emulator_present() and build_app.tools_present(), "needs ENAGA_EMULATOR and the toolchain")
class Sampler(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from emulate import Emulation
        WORK.mkdir(parents=True, exist_ok=True)
        files, cls.decoded = {}, {}
        for n, (spec, _, _) in CASES.items():
            data, cls.decoded[n] = make_wav(**spec)
            (WORK / f"{n}.wav").write_bytes(data)
            files[f"/SAMPLES/{n}.WAV"] = WORK / f"{n}.wav"
        (WORK / "4.wav").write_bytes(bytes(range(256)) * 4)                    # not a WAV
        files["/SAMPLES/4.WAV"] = WORK / "4.wav"                               # and no 5.WAV at all
        with Emulation(HERE / "apps" / "sampler", card_files=files, card_mib=8) as emu:
            cls.log = []

            def collect():                                  # the report keeps only the latest lines
                for line in emu.report()["log"]:
                    if line not in cls.log:
                        cls.log.append(line)
            cls.error = None
            for _ in range(1000):                           # from reset: the loads may end during boot
                collect()
                if "PLAYING" in cls.log or cls.error:
                    break
                cls.error = emu.run(10 * MS)["error"]
            cls.error = cls.error or emu.run(300 * MS)["error"]
            cls.left = samples(emu)[0::2][-9600:]

    def lines(self, n):
        load = [line.split() for line in self.log if line.startswith(f"L{n} ")]
        values = {(int(f), int(c)): int(v) for _, f, c, v in
                  (line.split() for line in self.log if line.startswith(f"V{n} "))}
        self.assertEqual(len(load), 1, f"file {n}: {self.log}")
        return [int(v) for v in load[0][1:]], values

    def test_every_file_loads_as_it_should(self):
        self.assertIsNone(self.error)
        self.assertIn("PLAYING", self.log)
        for n, (spec, rate, resampled) in CASES.items():
            with self.subTest(file=n):
                (result, channels, got_rate), values = self.lines(n)
                frames_in = int(spec["rate"] * spec["seconds"])
                self.assertEqual((channels, got_rate), (spec["channels"], rate))
                if resampled:
                    self.assertLessEqual(abs(result - frames_in * 48000 / spec["rate"]), 2, result)
                    tolerance = 0.015 if spec["bits"] == 8 else 2e-3       # the 8-bit files' own steps
                    for (frame, c), v in values.items():
                        if frame > 0:                                      # the window needs inputs before the start
                            self.assertAlmostEqual(v / 1e6, signal(frame, 48000, c), delta=tolerance,
                                                   msg=f"file {n} frame {frame} channel {c}")
                else:
                    self.assertEqual(result, frames_in)
                    for (frame, c), v in values.items():
                        want = round(f32(f32(self.decoded[n][frame][c]) * f32(1e6)))
                        self.assertLessEqual(abs(v - want), 1, f"file {n} frame {frame} channel {c}")
                self.assertTrue(values, f"file {n} logged no frames")

    def test_a_cancelled_load_calls_nothing_and_frees_the_loader(self):
        self.assertLess(self.log.index("CANCEL BUSY"), self.log.index("CANCELLED"))
        self.assertLess(self.log.index("CANCELLED"), self.log.index(next(l for l in self.log if l.startswith("L0 "))))

    def test_errors(self):
        self.assertEqual(self.lines(4)[0][0], -64)              # wav::not_wav, through the callback
        self.assertEqual(self.lines(5)[0][0], -2)               # ENAGA_FILE_NOT_FOUND, from load() at once

    def test_the_player_plays_it_at_1_khz(self):
        crossings = [i for i in range(1, len(self.left)) if self.left[i - 1] < 0 <= self.left[i]]
        self.assertAlmostEqual((len(crossings) - 1) * 48000 / (crossings[-1] - crossings[0]), 1000, delta=2)
        self.assertGreater(component(self.left, 48), 0.05)      # a sine, after the volume


if __name__ == "__main__":
    unittest.main()
