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


if __name__ == "__main__":
    unittest.main()
