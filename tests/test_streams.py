"""Streams on the emulator: tests/apps/streams plays four streams from one WAV file and one from a short one,
checking every word in audio(), and records /RECORD/TAKE.WAV from audio() for 1.5 s; this reads its RESULTS.TXT
and the recording back from the card image (tools/card.py).

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest tests.test_streams
"""
import io
import struct
import sys
import tempfile
import unittest
import wave
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENAGA = HERE.parent
sys.path.insert(0, str(ENAGA / "tools"))

import build_app  # noqa: E402
import card  # noqa: E402
import paths  # noqa: E402

MS = 372_000
BLOCKS, FRAMES = 1125, 64


def played(words):
    return struct.pack(f"<{words}H", *((j * 7 + (j >> 11)) & 0xFFFF for j in range(words)))


def wav_file(data, extra=b""):
    """16-bit stereo 48 kHz, with `extra` chunks between "fmt " and "data"."""
    fmt = struct.pack("<HHIIHH", 1, 2, 48000, 48000 * 4, 4, 16)
    body = b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt + extra + b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body


LONG = played(1 << 19)                                      # 1 MiB
LIST = b"LIST" + struct.pack("<I", 27) + b"INFOISFT" + struct.pack("<I", 15) + b"Enaga test wav\0" + b"\0"


@unittest.skipUnless(paths.emulator_present() and build_app.tools_present(), "needs ENAGA_EMULATOR and the toolchain")
class Streams(unittest.TestCase):
    def test_play_four_record_one(self):
        from emulate import Emulation
        with tempfile.TemporaryDirectory() as d:
            image = Path(d) / "card.img"
            package = build_app.build(HERE / "apps" / "streams", paths.BUILD / "apps" / "streams", "STREAMS")["package"]
            card.make(image, 64, "fat", {"/SAMPLES/LONG.WAV": wav_file(LONG, LIST),
                                         "/SAMPLES/SHORT.WAV": wav_file(LONG[:6000]),
                                         "/ENAGA/APPS/STREAMS/APP.ENA": package})
            with Emulation() as emu:
                emu.cpu.sd_load(image.read_bytes())
                log = []
                for _ in range(60):                                       # up to 6 s of emulated time
                    self.assertIsNone(emu.run(100 * MS)["error"])
                    log = emu.report()["log"]
                    if "STREAMS DONE" in log:
                        break
                self.assertIn("STREAMS DONE", log, log)
                self.assertEqual(emu.word("audio_overruns"), 0)
                image.write_bytes(emu.cpu.sd_read())
            results = {}
            for line in card.get(image, "/ENAGA/DATA/STREAMS/RESULTS.TXT").decode().splitlines():
                name, value = line.rsplit(" ", 1)
                results.setdefault(name, []).append(int(value))
            take = card.get(image, "/RECORD/TAKE.WAV")
        block = FRAMES * 4
        expected = {
            "parse": [1], "data_offset": [44 + len(LIST)], "data_bytes": [len(LONG)],
            "open_long": [0] * 4, "open_short": [0], "create_take": [0],
            "bad_ring": [-10], "missing": [-2], "denied": [-5], "write_on_playback": [0], "read_on_recording": [0],
            "long_mismatches": [0] * 4, "long_underruns": [0] * 4, "long_position": [BLOCKS * block] * 4,
            "long_state": [1] * 4,
            "short_mismatches": [0], "short_position": [6000], "short_underruns": [0], "short_state": [2],
            "take_underruns": [0], "take_position": [BLOCKS * block], "take_close": [0],
        }
        for name, value in expected.items():
            self.assertEqual(results.get(name), value, name)
        with wave.open(io.BytesIO(take)) as w:
            self.assertEqual((w.getnchannels(), w.getsampwidth(), w.getframerate()), (2, 2, 48000))
            self.assertEqual(w.getnframes(), BLOCKS * FRAMES)
            data = w.readframes(w.getnframes())
        words = BLOCKS * FRAMES * 2
        self.assertEqual(data, struct.pack(f"<{words}H", *((j * 13 + 5) & 0xFFFF for j in range(words))))
        self.assertEqual(len(take), 44 + BLOCKS * block)

    def test_writer_with_its_ring_full(self):
        """tests/apps/writer: wav::Writer's 24-bit frames (6 bytes) into a 4 KiB ring kept full, through write()
        and write_bytes(): whatever was lost, each file holds whole frames in order (frame k: k left, -k right),
        as many as frames() says, with the header's size matching."""
        from emulate import Emulation
        with Emulation(HERE / "apps" / "writer", "WRITER") as emu:
            for _ in range(60):
                self.assertIsNone(emu.run(100 * MS)["error"])
                if "WRITER DONE" in emu.report()["log"]:
                    break
            self.assertIn("WRITER DONE", emu.report()["log"])
            results = dict(line.rsplit(" ", 1) for line in emu.card_file("/ENAGA/DATA/WRITER/RESULTS.TXT").decode().splitlines())
            results = {k: int(v) for k, v in results.items()}
            self.assertEqual((results["create_full"], results["create_bytes"], results["create_12_bit"]), (0, 0, -64))
            self.assertEqual(results["closed_again"], 0)
            for name in ("full", "bytes"):
                with self.subTest(file=name):
                    frames, lost = results[f"{name}_frames"], results[f"{name}_lost"]
                    self.assertEqual(results[f"{name}_close"], 0)
                    self.assertEqual(results[f"{name}_taken"], frames)
                    self.assertEqual(frames + lost, 42 * 700)
                    self.assertGreater(lost, 0)                   # the ring did fill
                    data = emu.card_file(f"/RECORD/{name.upper()}.WAV")
                    self.assertEqual(data[:4] + data[8:12], b"RIFFWAVE")
                    size = struct.unpack("<I", data[40:44])[0]
                    self.assertEqual((size, len(data)), (6 * frames, 44 + 6 * frames))
                    values = [int.from_bytes(data[i:i + 3], "little", signed=True) for i in range(44, len(data), 3)]
                    left, right = values[0::2], values[1::2]
                    self.assertEqual(right, [-v for v in left])     # never a frame's halves from two frames
                    self.assertTrue(all(b > a for a, b in zip(left, left[1:])), "frames in order")
                    self.assertEqual(left[0], 0)


if __name__ == "__main__":
    unittest.main()
