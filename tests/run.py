#!/usr/bin/env python3
"""Run the test files side by side, each in its own process with its own build folder.

    python3 -B tests/run.py                       # every tests/test_*.py, as many at once as there are cores
    python3 -B tests/run.py --jobs 4
    python3 -B tests/run.py usb_debug e1          # only the files whose names contain one of these

Each file runs as `python3 -B -m unittest <file>` in tests/ (as discover runs them), with ENAGA_BUILD set to
build/test-runs/<file>/ so that no two write the same firmware, app or card files, and its output goes to
build/test-runs/<file>.log. The environment is passed on otherwise (ENAGA_EMULATOR, a TI compiler's path). Files
start longest first, by the times of the last run (build/test-runs/times.json). A file passes when its process
exits 0; a failing file's log is printed at the end.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / "tools"))
import paths  # noqa: E402

RUNS = paths.BUILD / "test-runs"
TIMES = RUNS / "times.json"


def run_file(name):
    """(name, exit code, seconds, summary) for one test file."""
    log = RUNS / f"{name}.log"
    env = dict(os.environ, ENAGA_BUILD=str(RUNS / name))
    start = time.monotonic()
    with log.open("w") as out:
        code = subprocess.run([sys.executable, "-B", "-m", "unittest", name], cwd=HERE, env=env,
                              stdout=out, stderr=subprocess.STDOUT).returncode
    seconds = time.monotonic() - start
    text = log.read_text(errors="replace")
    ran = re.findall(r"^Ran (\d+) tests?", text, re.M)
    skipped = re.findall(r"skipped=(\d+)", text)
    summary = f"{ran[-1] if ran else '?'} tests" + (f", {skipped[-1]} skipped" if skipped else "")
    return name, code, seconds, summary


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("only", nargs="*", help="parts of test file names (test_usb_debug.py: usb_debug)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args(argv)
    names = [p.stem for p in sorted(HERE.glob("test_*.py"))]
    if args.only:
        names = [n for n in names if any(part in n for part in args.only)]
    if not names:
        print("run.py: no test files match", file=sys.stderr)
        return 2
    RUNS.mkdir(parents=True, exist_ok=True)
    try:
        times = json.loads(TIMES.read_text())
    except (OSError, ValueError):
        times = {}
    names.sort(key=lambda n: -times.get(n, 1e9))      # unknown first, then longest first
    print(f"{len(names)} test files, {min(args.jobs, len(names))} at once; logs in {RUNS}", flush=True)
    start, failed = time.monotonic(), []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for future in as_completed([pool.submit(run_file, n) for n in names]):
            name, code, seconds, summary = future.result()
            times[name] = round(seconds, 1)
            print(f"{'ok  ' if code == 0 else 'FAIL'}  {name:28} {seconds:6.1f} s  {summary}", flush=True)
            if code:
                failed.append(name)
    TIMES.write_text(json.dumps(times, indent=1, sort_keys=True))
    for name in failed:
        print(f"\n==== {name} ({RUNS / (name + '.log')}) ====")
        print((RUNS / f"{name}.log").read_text(errors="replace")[-6000:])
    print(f"\n{len(names) - len(failed)} of {len(names)} files passed in {time.monotonic() - start:.0f} s"
          + (f"; failed: {', '.join(failed)}" if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
