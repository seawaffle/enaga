"""FAT32's FSInfo sector through Enaga's FatFs (firmware/third_party/fatfs, the same code as tools/card.py): it
trusts a well-formed sector's free cluster count, and what it writes back is well formed. The sector ends with the
signature 0xAA550000 at offset 508 (Microsoft's FAT specification, "FSInfo Sector Structure"); FatFs R0.15a had
it at 498, so it ignored good sectors and wrote ones that every other system had to distrust. The card is made
by mkfs.fat, which follows the specification, so the test does not grade FatFs against itself.

    python3 -B -m unittest tests.test_fsinfo
"""
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tools"))

import card  # noqa: E402

MKFS = shutil.which("mkfs.fat") or shutil.which("mkfs.fat", path="/usr/sbin:/sbin")
FSCK = shutil.which("fsck.fat") or shutil.which("fsck.fat", path="/usr/sbin:/sbin")


def fsinfo_offset(image):
    with open(image, "rb") as f:
        vbr = f.read(512)
    return struct.unpack_from("<H", vbr, 48)[0] * 512, struct.unpack_from("<B", vbr, 13)[0] * 512


@unittest.skipUnless(MKFS and shutil.which("gcc"), "needs mkfs.fat (dosfstools) and gcc for tools/cardtool.c")
class FsInfo(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.image = Path(self.dir.name) / "card.img"
        subprocess.run([MKFS, "-F", "32", "-C", str(self.image), str(64 * 1024)], check=True, capture_output=True)
        self.at, self.cluster = fsinfo_offset(self.image)

    def sector(self):
        with open(self.image, "rb") as f:
            f.seek(self.at)
            return f.read(512)

    def test_a_good_sector_is_trusted(self):
        data = bytearray(self.sector())
        self.assertEqual(data[508:512], b"\x00\x00\x55\xaa")              # mkfs.fat's, at the specified place
        struct.pack_into("<I", data, 488, 1234)                          # a free count that only FSInfo says
        with open(self.image, "r+b") as f:
            f.seek(self.at)
            f.write(data)
        self.assertEqual(card.free(self.image), 1234 * self.cluster)

    def test_what_it_writes_is_well_formed(self):
        free = struct.unpack_from("<I", self.sector(), 488)[0]
        card.put(self.image, "/A.TXT", b"x" * (3 * self.cluster))
        data = self.sector()
        self.assertEqual((data[0:4], data[484:488]), (b"RRaA", b"rrAa"))
        self.assertEqual(data[508:512], b"\x00\x00\x55\xaa")
        self.assertEqual(data[496:508], bytes(12))                       # reserved: nothing at 498
        self.assertEqual(struct.unpack_from("<I", data, 488)[0], free - 3)
        if FSCK:
            check = subprocess.run([FSCK, "-n", str(self.image)], capture_output=True, text=True)
            self.assertEqual(check.returncode, 0, check.stdout + check.stderr)


if __name__ == "__main__":
    unittest.main()
