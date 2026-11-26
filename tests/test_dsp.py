"""The DSP library (include/enaga/dsp) and libm (runtime/libm.c) compiled for the host and checked by
tests/dsp/host.cpp: math accuracy (libm's against the host's),
oscillator pitch, filter response, envelope times, delay time and reverb decay. Needs only a host g++.

    python3 -B -m unittest discover -s tests -p test_dsp.py -v
"""
import shutil
import subprocess
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


@unittest.skipUnless(shutil.which("g++"), "host g++ not found")
class Dsp(unittest.TestCase):
    def test_dsp_library_on_the_host(self):
        self.run_host(ROOT / "build" / "dsp", [])

    def test_dsp_library_on_the_host_in_cl6x_forms(self):
        """The same checks with the branch-free forms cl6x builds (ENAGA_DSP_SELECTS), which GCC builds otherwise
        skip: both forms must give the same results."""
        self.run_host(ROOT / "build" / "dsp-selects", ["-DENAGA_DSP_SELECTS"])

    def run_host(self, out, defines):
        out.mkdir(parents=True, exist_ok=True)
        objs = []
        for src in sorted((ROOT / "runtime" / "dsp").glob("*.c")):     # the block functions, in C
            objs.append(out / (src.stem + ".o"))
            subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", *defines,
                            f"-I{ROOT / 'include'}", "-c", "-o", objs[-1], src], check=True)
        objs.append(out / "libm.o")                                     # libm under enaga_host_ names
        subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-DENAGA_LIBM_HOST",
                        f"-I{ROOT / 'include'}", "-c", "-o", objs[-1], ROOT / "runtime" / "libm.c"], check=True)
        subprocess.run(["g++", "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                        f"-I{ROOT / 'include'}", "-o", out / "host",
                        HERE / "dsp" / "host.cpp", *objs], check=True)
        result = subprocess.run([out / "host"], capture_output=True, text=True)
        self.assertEqual((result.returncode, result.stdout.strip()), (0, "ok"), result.stdout)


if __name__ == "__main__":
    unittest.main()
