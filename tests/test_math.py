"""Math on the target: tests/apps/math runs every function of <cmath> (runtime/libm.c, built by cl6x when it is
installed) and some of enaga::dsp's math on the emulator's C674x, and its results are compared with Python's double
math: libm within 1 ulp, the fast functions within their stated accuracy. tests/test_dsp.py measures the same code
on the host far more widely; this checks the target's compilers and its double arithmetic. Also: a double call is
a compile error, as cmath promises.

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest discover -s tests -p test_math.py -v
"""
import math
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ENAGA = HERE.parent
sys.path.insert(0, str(ENAGA / "tools"))
sys.path.insert(0, str(HERE))

import build_app  # noqa: E402
import paths  # noqa: E402
from test_e0 import RESULT, STACK_BASE, STACK_SIZE, WINDOW_SIZE, WORK, build_harness  # noqa: E402

APP = HERE / "apps" / "math"


def f32(value):
    """value rounded to float, inf when it is beyond float range."""
    try:
        return struct.unpack("<f", struct.pack("<f", value))[0]
    except OverflowError:
        return math.copysign(math.inf, value)


def exact(function, *args):
    """function on doubles, with the standard's answers where Python raises."""
    try:
        return function(*args)
    except OverflowError:
        return math.inf
    except (ValueError, ZeroDivisionError):
        return math.nan


def ulps(got, want):
    if math.isnan(want):
        return 0 if math.isnan(got) else math.inf
    if math.isinf(want) or abs(want) > 3.4028234663852886e38:
        return 0 if got == f32(want) else math.inf
    if abs(want) < 1.1754943508222875e-38:              # the C674x flushes subnormals: allow 0 or the smallest normal
        return 0 if abs(got) <= 1.1754943508222875e-38 else math.inf
    return abs(got - want) / float(np.spacing(np.float32(abs(f32(want)))))


def inputs():
    """kWide and kUnit, read back from the app's source."""
    text = (APP / "app.cpp").read_text()
    lists = []
    for name in ("kWide", "kUnit"):
        body = text.split(f"constexpr float {name}[] = {{")[1].split("};")[0]
        lists.append([f32(float(v.strip().rstrip("f"))) for v in body.split(",")])
    return lists


def zero_pole(log):
    """A logarithm with the standard's -inf at 0, where Python raises."""
    return lambda x: -math.inf if x == 0 else log(x)


def fmod(x, y):
    return math.fmod(x, y) if y != 0 and not math.isinf(x) else math.nan


def power(x, y):
    """pow with the standard's answers at 0, where Python raises."""
    if y == 0 or x == 1:
        return 1.0
    odd = not math.isinf(y) and y == int(y) and abs(y) < 2 ** 24 and int(y) % 2
    if x == 0 and y < 0:
        return math.copysign(math.inf, x) if odd else math.inf
    try:
        return math.pow(x, y)
    except OverflowError:
        return -math.inf if x < 0 and odd else math.inf
    except ValueError:
        return math.nan


@unittest.skipUnless(build_app.tools_present(), "tic6x GCC not built (tools/build_tic6x_*.sh)")
@unittest.skipUnless(paths.emulator_present(), "ENAGA_EMULATOR not set to the SmplTrek emulator directory")
class TargetMath(unittest.TestCase):
    @classmethod
    def run_app(cls, ti):
        """The app's results with the library (runtime/libm.c among it) built by cl6x or by GCC."""
        from cpu import CPU, EXECUTE, READ, WRITE
        with mock.patch.object(paths, "TI_CGT", paths.TI_CGT if ti else None):
            built = build_app.build(APP, WORK / ("math-cl6x" if ti else "math-gcc"))
        app = build_app.symbols(built["elf"])
        with CPU() as cpu:
            cpu.load_elf(cls.harness)
            cpu.map(build_app.BASE, WINDOW_SIZE, READ | WRITE | EXECUTE)
            cpu.map(STACK_BASE, STACK_SIZE, READ | WRITE)
            cpu.write(cls.sym["harness_package"], built["package"])
            cpu.write(cls.sym["harness_package_size"], struct.pack("<I", len(built["package"])))
            cpu.reset(cls.sym["harness_main"])
            cpu.set_register("b15", STACK_BASE + STACK_SIZE - 8)
            state = cpu.run(400_000_000)
            fields = RESULT.unpack(cpu.read(cls.sym["harness_result"], RESULT.size))
            log = cpu.read(cls.sym["harness_log"], fields[6]).decode("ascii").splitlines()
            count = struct.unpack("<i", cpu.read(app["g_result_count"], 4))[0]
            return state, log, cpu.read(app["g_results"], 4 * count)

    @classmethod
    def setUpClass(cls):
        paths.use_emulator()
        cls.harness, cls.sym = build_harness()
        cls.state, cls.log, cls.raw = cls.run_app(paths.ti_present())
        cls.results = list(struct.unpack(f"<{len(cls.raw) // 4}f", cls.raw))

    def setUp(self):
        self.wide, self.unit = inputs()
        self.at = 0

    def take(self, count):
        out = self.results[self.at:self.at + count]
        self.at += count
        return out

    def check(self, name, function, values, limit, compare=ulps):
        worst, where = 0.0, None
        for args, got in zip(values, self.take(len(values))):
            args = args if isinstance(args, tuple) else (args,)
            error = compare(got, function(*args))
            if error > worst:
                worst, where = error, (args, got)
        self.assertLessEqual(worst, limit, f"{name}: {worst} at {where}")

    def test_every_function(self):
        self.assertEqual(self.state["status"], "returned")
        self.assertIn("math done", self.log)
        w, u = self.wide, self.unit
        pairs = [(x, y) for x in w[::3] for y in w[::5]]
        for name, function, values in (
                ("sinf", math.sin, w), ("cosf", math.cos, w), ("tanf", math.tan, w), ("asinf", math.asin, u),
                ("acosf", math.acos, u), ("atanf", math.atan, w), ("sinhf", lambda x: math.copysign(math.inf, x) if abs(x) > 710 else math.sinh(x), w), ("coshf", math.cosh, w),
                ("tanhf", math.tanh, w), ("expf", math.exp, w), ("exp2f", lambda x: 2.0 ** x, w),
                ("expm1f", math.expm1, w), ("logf", zero_pole(math.log), w), ("log2f", zero_pole(math.log2), w),
                ("log10f", zero_pole(math.log10), w), ("log1pf", lambda x: -math.inf if x == -1 else math.log1p(x), w), ("sqrtf", math.sqrt, w), ("cbrtf", math.cbrt, w)):
            self.check(name, lambda *a, f=function: exact(f, *a), values, 1)
        for name, function in (("floorf", math.floor), ("ceilf", math.ceil),
                               ("roundf", lambda x: math.copysign(math.floor(abs(x) + 0.5), x)),
                               ("rintf", round)):
            self.check(name, lambda x, f=function: x if math.isinf(x) or abs(x) >= 2 ** 23 else float(f(x)), w, 0)
        self.check("atan2f", lambda y, x: exact(math.atan2, y, x), pairs, 1)
        self.check("powf", power, pairs, 1)
        self.check("hypotf", lambda x, y: exact(math.hypot, x, y), pairs, 1)
        self.check("fmodf", fmod, pairs, 0)
        self.check("dsp::sin", lambda x: (math.sin(x), 4e-6 + 1e-7 * abs(x)) if abs(x) < 1e3 else None, w, 1,
                   lambda got, want: 0 if want is None else abs(got - want[0]) / want[1])
        self.check("dsp::tanh", math.tanh, w, 6e-7, lambda got, want: abs(got - want) / max(abs(want), 1e-30))
        self.check("dsp::sqrt", lambda x: math.sqrt(x) if x > 0 else 0.0, w, 3e-7,
                   lambda got, want: abs(got - want) / want if 0 < want < 1e18 else 0)
        self.check("dsp::log2", lambda x: math.log2(x) if x > 0 else None, w, 1,
                   lambda got, want: 0 if want is None else abs(got - want) / (2e-7 * max(1.0, abs(want))))
        self.check("dsp::atan", math.atan, w, 5e-7, lambda got, want: abs(got - want))
        self.check("dsp::asin", math.asin, u, 4e-4, lambda got, want: abs(got - want))
        self.check("from C", lambda x: math.sin(x) + math.sqrt(abs(x)), w, 1,     # two roundings, then a sum
                   lambda got, want: abs(got - want) / (3e-7 * max(1.0, abs(want))))
        self.assertEqual(self.at, len(self.results), "results left unchecked")

    @unittest.skipUnless(paths.ti_present(), "TI's cl6x not installed (tools/paths.py, TI_CGT)")
    def test_cl6x_and_gcc_builds_agree(self):
        # The same IEEE operations in the same order (no fused multiply-add on the C674x): the same bits.
        state, log, raw = self.run_app(False)
        self.assertIn("math done", log)
        self.assertEqual(raw, self.raw)

    def test_a_double_call_does_not_compile(self):
        with tempfile.TemporaryDirectory() as folder:
            (Path(folder) / "app.cpp").write_text("#include <cmath>\nfloat f(double x) { return float(std::sin(x)); }\n")
            result = subprocess.run([str(build_app.GXX), *build_app.CXXFLAGS, "-c", "-o", Path(folder) / "app.o",
                                     Path(folder) / "app.cpp"], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("use of deleted function", result.stderr)


if __name__ == "__main__":
    unittest.main()
