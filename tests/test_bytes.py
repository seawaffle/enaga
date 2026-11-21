"""The byte helpers (include/enaga/bytes.hpp) compiled for the host: loads and stores in both byte orders at every
offset in a buffer, so at misaligned addresses too, without touching the bytes around them. Needs only a host g++.

    python3 -B -m unittest discover -s tests -p test_bytes.py -v
"""
import shutil
import subprocess
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent

SOURCE = r"""
#include <enaga/bytes.hpp>
#include <cstdio>
using namespace enaga::bytes;
static int failures;
static void check(bool ok, const char *what, int at) { if (!ok) { std::printf("FAIL %s at %d\n", what, at); failures++; } }
int main() {
    for (int at = 0; at < 8; at++) {
        alignas(8) uint8_t b[16];
        for (int i = 0; i < 16; i++) b[i] = uint8_t(0xA0 + i);
        check(load_le16(b + at) == uint16_t(b[at] | b[at + 1] << 8), "load_le16", at);
        check(load_be16(b + at) == uint16_t(b[at] << 8 | b[at + 1]), "load_be16", at);
        check(load_le32(b + at) == (uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 |
                                    uint32_t(b[at + 3]) << 24), "load_le32", at);
        check(load_be32(b + at) == (uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 |
                                    uint32_t(b[at + 3])), "load_be32", at);
        store_le32(b + at, 0x11223344u);
        check(b[at] == 0x44 && b[at + 1] == 0x33 && b[at + 2] == 0x22 && b[at + 3] == 0x11, "store_le32", at);
        check(load_le32(b + at) == 0x11223344u, "le32 round trip", at);
        store_be32(b + at, 0x11223344u);
        check(b[at] == 0x11 && b[at + 3] == 0x44 && load_be32(b + at) == 0x11223344u, "store_be32", at);
        store_le16(b + at + 4, 0xBEEF);
        check(b[at + 4] == 0xEF && b[at + 5] == 0xBE && load_le16(b + at + 4) == 0xBEEF, "store_le16", at);
        store_be16(b + at + 4, 0xBEEF);
        check(b[at + 4] == 0xBE && b[at + 5] == 0xEF && load_be16(b + at + 4) == 0xBEEF, "store_be16", at);
        for (int i = 0; i < 16; i++)                            // nothing outside the six bytes written
            if (i < at || i >= at + 6) check(b[i] == uint8_t(0xA0 + i), "neighbours", at);
    }
    std::printf(failures ? "failed\n" : "ok\n");
    return failures != 0;
}
"""


@unittest.skipUnless(shutil.which("g++"), "host g++ not found")
class Bytes(unittest.TestCase):
    def test_byte_helpers_at_every_offset(self):
        out = ROOT / "build" / "bytes"
        out.mkdir(parents=True, exist_ok=True)
        (out / "host.cpp").write_text(SOURCE)
        subprocess.run(["g++", "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", f"-I{ROOT / 'include'}",
                        "-o", out / "host", out / "host.cpp"], check=True)
        result = subprocess.run([out / "host"], capture_output=True, text=True)
        self.assertEqual((result.returncode, result.stdout.strip()), (0, "ok"), result.stdout)


if __name__ == "__main__":
    unittest.main()
