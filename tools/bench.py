#!/usr/bin/env python3
"""DSP library benchmark: builds tests/apps/bench with the library compiled by cl6x and by GCC, runs each on the
emulator's C674x (tests/harness.c), and prints cycles per 64-frame block for every item side by side.

    ENAGA_EMULATOR=/path/to/emulator python3 -B tools/bench.py [--json FILE]

The emulator models no caches or bus waits, so the figures are the core's work alone; on the unit, code and data
in DDR cost more. Compare them with each other, not with the 496,000-cycle block budget as such.
"""
import argparse
import json
import struct
import sys
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
ENAGA = HERE.parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(ENAGA / "tests"))

import build_app  # noqa: E402
import paths  # noqa: E402
from test_e0 import RESULT, STACK_BASE, STACK_SIZE, WINDOW_SIZE, WORK, build_harness  # noqa: E402

APP = ENAGA / "tests" / "apps" / "bench"


def run(ti, harness, sym):
    from cpu import CPU, EXECUTE, READ, WRITE
    with mock.patch.object(paths, "TI_CGT", paths.TI_CGT if ti else None):
        built = build_app.build(APP, WORK / ("bench-cl6x" if ti else "bench-gcc"))
    with CPU() as cpu:
        cpu.load_elf(harness)
        cpu.map(build_app.BASE, WINDOW_SIZE, READ | WRITE | EXECUTE)
        cpu.map(STACK_BASE, STACK_SIZE, READ | WRITE)
        cpu.write(sym["harness_package"], built["package"])
        cpu.write(sym["harness_package_size"], struct.pack("<I", len(built["package"])))
        cpu.reset(sym["harness_main"])
        cpu.set_register("b15", STACK_BASE + STACK_SIZE - 8)
        state = cpu.run(400_000_000)
        fields = RESULT.unpack(cpu.read(sym["harness_result"], RESULT.size))
        log = cpu.read(sym["harness_log"], fields[6]).decode("ascii").splitlines()
    if "done" not in log:
        raise RuntimeError(f"bench did not finish ({state['status']}): {log[-5:]}")
    return {name: int(value) for name, value in (line.split() for line in log if " " in line)}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--json", type=Path, help="also write the figures here")
    args = parser.parse_args()
    if not paths.emulator_present():
        sys.exit("ENAGA_EMULATOR is not set to the SmplTrek emulator directory")
    paths.use_emulator()
    harness, sym = build_harness()
    gcc = run(False, harness, sym)
    cl6x = run(True, harness, sym) if paths.ti_present() else {}
    print(f"{'item (64 frames)':24} {'GCC':>8} {'cl6x':>8} {'ratio':>6}")
    for name, cycles in gcc.items():
        other = cl6x.get(name)
        ratio = f"{cycles / other:6.1f}" if other else ""
        print(f"{name:24} {cycles:8} {other if other is not None else '':>8} {ratio}")
    if args.json:
        args.json.write_text(json.dumps({"gcc": gcc, "cl6x": cl6x}, indent=1))


if __name__ == "__main__":
    main()
