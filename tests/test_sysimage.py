"""The update files for the SmplTrek, Lofi-12 XT and CyDrums (tools/sysimage.py), on the host.

    python3 -B -m unittest tests.test_sysimage
"""
import hashlib
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import sysimage  # noqa: E402

CODE = bytes(range(256)) * 20


class SysImage(unittest.TestCase):
    def test_smpltrek_file_unchanged(self):         # the bytes the SmplTrek has installed since session 1
        data = sysimage.wrap((0, 1, 0), 0xC0000000, [(0xC0000000, CODE)])
        self.assertEqual(hashlib.sha256(data).hexdigest(),
                         "66861b363168d482fe45ce4f83dbca454d82c578dc1349517147fde87ef39f29")

    def test_each_device_verifies_only_as_itself(self):
        for device, d in sysimage.DEVICES.items():
            data = sysimage.wrap((0, 1, 0), 0xC0000000, [(0xC0000000, CODE)], device)
            self.assertEqual(struct.unpack_from("<I", data, 16)[0], d["family"])
            for other in sysimage.DEVICES:
                self.assertEqual(sysimage.verify(data, other), other == device, (device, other))


if __name__ == "__main__":
    unittest.main()
