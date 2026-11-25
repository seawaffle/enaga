#!/usr/bin/env python3
"""Run an app's tests on this PC: each tests/*.cpp in the app's folder is a program (its own main) built with the
PC's compiler together with the app's sources, the DSP library (runtime/dsp) and a stand-in for Enaga's host table
(runtime/host/host.cpp), then run. <enaga/testing.hpp> has the helpers. No toolchain or unit needed: g++ and gcc.

    python3 -B tools/host_test.py examples/sampler       # -> build/apps/sampler/host-test/
    python3 -B tools/host_test.py ~/my-app               # an app outside Enaga's folder -> ~/my-app/build/host-test/
    python3 -B tools/host_test.py ~/my-app --sanitize    # with AddressSanitizer and UBSan: memory and UB errors
    python3 -B tools/host_test.py ~/my-app engine        # only tests/engine.cpp

Every *.cpp and *.c in the app's folder is compiled in (as build_app.py does; *.ti.c files by gcc), so tests can
call any of the app's code; the app's own ENAGA_APP entry is there but nothing calls it. Each test runs in the
build folder; testing::input(name) is a file in the app's tests/ folder and testing::output(name) one in the build
folder. The exit status is 1 if any test failed (a test fails by returning non-zero from main, as
Checks::finish() does, or by crashing), 2 if it did not build.

CC and CXX choose other compilers (clang, say).
"""
import argparse
import os
import shlex
import signal
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import paths  # noqa: E402

INCLUDE = paths.ROOT / "include"
RUNTIME = paths.ROOT / "runtime"
WORK = paths.BUILD / "apps"


def compilers():
    return shlex.split(os.environ.get("CC", "gcc")), shlex.split(os.environ.get("CXX", "g++"))


def flags(app, out, sanitize):
    common = ["-O1" if sanitize else "-O2", "-g", "-Wall", "-Wextra", f"-I{INCLUDE}",
              f'-DENAGA_TEST_INPUT="{app / "tests"}"', f'-DENAGA_TEST_OUTPUT="{out}"']
    if sanitize:
        common += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-sanitize-recover=undefined"]
    return common


def build(app, out, sanitize=False, only=None):
    """Builds every test program; returns their paths. Raises CalledProcessError if a compile fails."""
    app, out = Path(app).resolve(), Path(out)
    tests = sorted((app / "tests").glob("*.cpp"))
    if only:
        tests = [t for t in tests if t.stem in only]
    if not tests:
        raise ValueError(f"no tests in {app / 'tests'}" + (f" named {', '.join(only)}" if only else "") +
                         " (each tests/*.cpp is a program with its own main)")
    cc, cxx = compilers()
    common = flags(app, out, sanitize)
    objects = out / "obj"
    objects.mkdir(parents=True, exist_ok=True)
    shared = []
    sources = [*sorted((RUNTIME / "dsp").glob("*.c")), RUNTIME / "host" / "host.cpp",
               *sorted(app.glob("*.c")), *sorted(app.glob("*.cpp"))]
    for i, src in enumerate(sources):
        obj = objects / f"{i:03d}-{src.name}.o"
        if src.suffix == ".c":
            cmd = [*cc, "-std=c11", *common, "-c", str(src), "-o", str(obj)]
        else:
            cmd = [*cxx, "-std=c++20", *common, "-c", str(src), "-o", str(obj)]
        subprocess.run(cmd, check=True)
        shared.append(obj)
    programs = []
    for test in tests:
        program = out / test.stem
        subprocess.run([*cxx, "-std=c++20", *common, str(test), *map(str, shared), "-lm", "-o", str(program)],
                       check=True)
        programs.append(program)
    return programs


def run(programs, out):
    """Runs each program in the build folder; returns {name: exit status}."""
    results = {}
    for program in programs:
        print(f"== {program.name}", flush=True)
        status = subprocess.run([str(program)], cwd=out).returncode
        if status < 0:
            print(f"host_test: {program.name} crashed ({signal.Signals(-status).name}); --sanitize may say where")
        results[program.name] = status
    return results


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("app_dir", type=Path)
    ap.add_argument("tests", nargs="*", help="run only these tests (file names in tests/ without .cpp)")
    ap.add_argument("--sanitize", action="store_true", help="build with AddressSanitizer and UBSan")
    args = ap.parse_args(argv)
    app = args.app_dir.resolve()
    out = (WORK / app.name if app.is_relative_to(paths.ROOT) else app / "build") / "host-test"
    try:
        programs = build(app, out, args.sanitize, args.tests)
    except ValueError as e:
        print(f"host_test: {e}", file=sys.stderr)
        return 2
    except subprocess.CalledProcessError as e:     # the compiler has said why
        print(f"host_test: {Path(str(e.cmd[0])).name} failed", file=sys.stderr)
        return 2
    results = run(programs, out)
    failed = [name for name, status in results.items() if status != 0]
    print(f"host_test: {len(results) - len(failed)} of {len(results)} passed" +
          (f"; failed: {', '.join(failed)}" if failed else "") + f" (output in {out})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
