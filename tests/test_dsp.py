"""The DSP library (include/enaga/dsp) and libm (runtime/libm.c) compiled for the host and checked by
tests/dsp/host.cpp: math accuracy (libm's against the host's),
oscillator pitch, filter response, envelope times, delay time and reverb decay. Needs only a host g++.
With the tic6x GCC and TI's cl6x, also that both compilers lay out every struct of <enaga/dsp.h> alike, which a
GCC app linking the library built by cl6x (the SDK's runtime/prebuilt) depends on.

    python3 -B -m unittest discover -s tests -p test_dsp.py -v
"""
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(ROOT / "tools"))

import build_app  # noqa: E402
import paths  # noqa: E402


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


def dsp_structs():
    """Every typedef'd struct in <enaga/dsp.h> with its field names, in order."""
    text = (ROOT / "include" / "enaga" / "dsp.h").read_text()
    structs = {}
    for m in re.finditer(r"typedef struct(?: \w+)? \{(.*?)\}\s*(\w+);", text, re.S):
        fields = []
        for decl in re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S).split(";"):
            decl = re.sub(r"\[.*?\]", "", decl.strip())
            if decl:
                fields += [re.findall(r"\w+", part)[-1] for part in decl.split(",")]
        structs[m.group(2)] = fields
    return structs


@unittest.skipUnless(paths.toolchain_present() and paths.ti_present(), "needs the tic6x GCC and TI's cl6x")
class Layout(unittest.TestCase):
    def test_both_compilers_lay_out_the_structs_alike(self):
        """GCC's size, alignment and field offsets for each struct, asserted at compile time under cl6x; one wrong
        number must fail, so the check itself is checked."""
        structs = dsp_structs()
        self.assertGreater(len(structs), 15)
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            items = [(f"sizeof({n})", f"_Alignof({n})", *(f"offsetof({n}, {f})" for f in fs)) for n, fs in structs.items()]
            items = [expr for group in items for expr in group]
            head = "#include <stddef.h>\n#include <enaga/dsp.h>\n"
            (d / "probe.c").write_text(head + "const unsigned int layout[] = {" + ", ".join(items) + "};\n")
            subprocess.run([build_app.GCC, *build_app.CFLAGS, "-c", "-o", d / "probe.o", d / "probe.c"], check=True)
            subprocess.run([build_app.OBJCOPY, "-O", "binary", "-j", ".const.layout", d / "probe.o", d / "layout.bin"],
                           check=True)
            raw = (d / "layout.bin").read_bytes()
            values = [int.from_bytes(raw[i:i + 4], "little") for i in range(0, len(raw), 4)]
            self.assertEqual(len(values), len(items))

            def cl6x(asserts):
                (d / "check.c").write_text(head + asserts)
                flags = [f for f in build_app.TIFLAGS if f != "--c99"]
                return subprocess.run([paths.TI_CGT / "bin" / "cl6x", *flags, "--c11", f"-I{paths.TI_CGT / 'include'}",
                                       f"--output_file={d / 'check.obj'}", "-c", d / "check.c"],
                                      cwd=d, capture_output=True, text=True)
            asserts = "".join(f'_Static_assert({e} == {v}, "{e}");\n' for e, v in zip(items, values))
            result = cl6x(asserts)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotEqual(cl6x(asserts.replace(" == ", " == 1 + ", 1)).returncode, 0)


if __name__ == "__main__":
    unittest.main()
