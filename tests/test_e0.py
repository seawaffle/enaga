"""E0: C++ apps built with tools/build_app.py run through the real loader and runtime on the emulator's CPU core.

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest discover -s tests -v

Needs the tic6x GCC (tools/build_tic6x_*.sh), host g++, and the SmplTrek emulator (README.md, "Testing without
a unit"). Without ENAGA_EMULATOR these tests are skipped.
"""
import struct
import subprocess
import sys
import unittest
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENAGA = HERE.parent
sys.path.insert(0, str(ENAGA / "tools"))

import build_app  # noqa: E402
import paths  # noqa: E402

WORK = paths.BUILD / "tests"
STACK_BASE, STACK_SIZE = 0xC0F00000, 0x10000
WINDOW_SIZE = 0x100000
RESULT = struct.Struct("<iiIIIIII")
LOAD = dict(ok=0, truncated=-1, magic=-2, header=-3, abi=-4, window=-5, crc=-6, entry=-7)


def build_harness():
    out = WORK / "harness"
    out.mkdir(parents=True, exist_ok=True)
    objs = [build_app.compile_source(src, out) for src in (HERE / "harness.c", ENAGA / "loader" / "loader.c")]
    libgcc = paths.libgcc()
    elf = out / "harness.elf"
    build_app.run([build_app.LD, "-EL", "-T", HERE / "harness.ld", "-nostdlib", "--no-warn-rwx-segments",
                   "--orphan-handling=error", "-o", elf, *objs, libgcc], out)
    undefined = subprocess.run([str(build_app.NM), "-u", str(elf)], capture_output=True, text=True, check=True).stdout
    if undefined.strip():
        raise RuntimeError("harness has undefined symbols: " + undefined)
    return elf, build_app.symbols(elf)


@unittest.skipUnless(build_app.tools_present(), "tic6x GCC not built (tools/build_tic6x_*.sh)")
@unittest.skipUnless(paths.emulator_present(), "ENAGA_EMULATOR not set to the SmplTrek emulator directory")
class E0(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        paths.use_emulator()
        from cpu import CPU
        cls.CPU = CPU
        cls.harness, cls.sym = build_harness()
        cls.cxxcheck = build_app.build(ENAGA / "examples" / "cxxcheck", WORK / "cxxcheck")["package"]
        cls.oom = build_app.build(HERE / "apps" / "oom", WORK / "oom")["package"]

    def execute(self, package, cycles=20_000_000):
        from cpu import READ, WRITE, EXECUTE
        with self.CPU() as cpu:
            cpu.load_elf(self.harness)
            cpu.map(build_app.BASE, WINDOW_SIZE, READ | WRITE | EXECUTE)
            cpu.map(STACK_BASE, STACK_SIZE, READ | WRITE)
            cpu.write(self.sym["harness_package"], package)
            cpu.write(self.sym["harness_package_size"], struct.pack("<I", len(package)))
            cpu.reset(self.sym["harness_main"])
            cpu.set_register("b15", STACK_BASE + STACK_SIZE - 8)
            state = cpu.run(cycles)
            fields = RESULT.unpack(cpu.read(self.sym["harness_result"], RESULT.size))
            result = dict(zip(("load", "init", "updates", "stopped", "fatal", "done", "log_length", "name"), fields))
            result["log"] = cpu.read(self.sym["harness_log"], result["log_length"]).decode("ascii").splitlines()
            result["status"] = state["status"]
            result["cycles"] = state["cycles"]
            if result["name"]:
                result["name"] = cpu.read(result["name"], 32).split(b"\0")[0].decode("ascii")
            return result

    def test_cxx_app_runs(self):
        result = self.execute(self.cxxcheck)
        big = 123456789 * 1000
        self.assertEqual(result["status"], "returned")
        self.assertEqual((result["load"], result["init"], result["updates"], result["stopped"], result["fatal"]),
                         (LOAD["ok"], 0, 3, 1, 0))
        self.assertEqual(result["name"], "CxxCheck")
        self.assertEqual(result["log"], [
            "global constructed", "global 42", "virtual -32", "heap 3969", "static 303", "static constructions 1",
            "sorted first -4", "sorted last 17", "sum 33", "optional 5", "max 17",
            f"int64 div {big // 977}", f"int64 mod {big % 977}", f"int64 negative div {-(big // 977)}",
            f"int64 negative mod {-(big % 977)}", f"uint64 div {0xFEDCBA9876543210 // 0x123456789}",
            f"uint64 mod {0xFEDCBA9876543210 % 0x123456789}", "strcmp 1", "strncmp 1", "strchr 7", "strrchr 12",
            "strchr end 1", "float milli 666", "double micro 142857",
            "update 10", "update 20", "update 30", "stop after updates 3"])

    def test_package_header(self):
        package = self.cxxcheck
        magic, header_size, abi, base, size, bss, entry, crc, version, flags, name = build_app.HEADER.unpack_from(package)
        self.assertEqual((magic, header_size, abi, base, flags), (0x41474E45, 64, 0, build_app.BASE, 0))
        self.assertEqual(len(package), header_size + size)
        self.assertEqual(crc, zlib.crc32(package[64:]) & 0xFFFFFFFF)
        self.assertEqual(name.rstrip(b"\0"), b"cxxcheck")
        self.assertLess(entry, size)
        self.assertEqual(version, 1)
        self.assertGreater(bss, 0)

    def rejected(self, package, status):
        result = self.execute(package)
        self.assertEqual(result["status"], "returned")
        self.assertEqual(result["load"], LOAD[status])
        self.assertEqual(result["log"], [], "no app code may run for a rejected package")

    def patched(self, offset, value):
        package = bytearray(self.cxxcheck)
        struct.pack_into("<I", package, offset, value)
        return bytes(package)

    def test_rejects_bad_packages(self):
        self.rejected(self.cxxcheck[:40], "truncated")
        self.rejected(self.cxxcheck[:-4], "truncated")
        self.rejected(self.patched(0, 0x12345678), "magic")
        self.rejected(self.patched(4, 60), "header")
        self.rejected(self.patched(36, 2), "header")
        self.rejected(self.patched(36, 1), "truncated")      # an icon declared, none after the payload
        with_icon = bytearray(self.patched(36, 1)) + bytes(range(128))
        self.rejected(bytes(with_icon[:-1]), "truncated")
        self.rejected(self.patched(8, 1), "abi")
        self.rejected(self.patched(12, 0xC2000000), "window")
        self.rejected(self.patched(24, len(self.cxxcheck)), "window")
        self.rejected(self.patched(20, 0x7FFFFFFF), "window")
        corrupt = bytearray(self.cxxcheck)
        corrupt[100] ^= 1
        self.rejected(bytes(corrupt), "crc")

    def test_package_with_an_icon_loads(self):
        package = bytearray(self.cxxcheck) + bytes(range(128))
        struct.pack_into("<I", package, 36, 1)
        result = self.execute(bytes(package))
        self.assertEqual((result["status"], result["load"], result["name"]), ("returned", LOAD["ok"], "CxxCheck"))

    def test_icon_txt_goes_after_the_payload(self):
        tone = build_app.build(ENAGA / "examples" / "tone", WORK / "tone")
        package = tone["package"]
        flags = build_app.HEADER.unpack_from(package)[9]
        self.assertEqual(flags, 1)
        self.assertEqual(len(package), 64 + tone["payload_size"] + 128)
        rows = [r for r in (ENAGA / "examples" / "tone" / "icon.txt").read_text().splitlines() if not r.startswith("#")]
        icon = package[-128:]
        for y, row in enumerate(rows):
            for x, c in enumerate(row):
                pixel = icon[(y * 16 + x) // 2] >> (0 if x & 1 else 4) & 15
                self.assertEqual(pixel, 0 if c == "." else int(c, 16), (x, y))

    def test_out_of_memory_is_fatal(self):
        result = self.execute(self.oom)
        self.assertEqual(result["status"], "budget")    # fatal() never returns
        self.assertEqual(result["fatal"], 1)
        self.assertEqual(result["log"], ["allocating", "FATAL", "out of memory"])


if __name__ == "__main__":
    unittest.main()
