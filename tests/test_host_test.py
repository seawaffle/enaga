"""tools/host_test.py, the PC tests for app developers: an app folder's tests built with its sources, the DSP
library and the stand-in host table, and run. A passing test, a failing one, a crash and one that does not compile
each give the status and report they should; the stand-in host's log, MIDI and clock work; and the sampler
example's own tests pass (with the sanitizers too). Needs only the PC's g++ and gcc.

    python3 -B -m unittest tests.test_host_test
"""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENAGA = HERE.parent
TOOL = ENAGA / "tools" / "host_test.py"

APP = """#include <enaga/app.hpp>
int twice(int x) { return 2 * x; }
class A : public enaga::App {};
ENAGA_APP(A)
"""

PASSING = """#include <enaga/testing.hpp>
#include <enaga/clock.hpp>
#include <enaga/midi.hpp>
using namespace enaga::testing;
int twice(int x);
int main() {
    Checks t;
    t.check("the app's code is linked in", twice(21) == 42);
    t.near("a sine's frequency", frequency(sine(440, frames(0.5))), 440, 0.01);
    t.near("its RMS", rms(sine(100, frames(1.0))), 0.70710678, 1e-4);
    t.check("an impulse arrives", first_above(impulse(100, 37), 0.5) == 37);
    Buffer stereo = render(1000, [](float *l, float *r, int n) { for (int i = 0; i < n; i++) { l[i] = 1; r[i] = -1; } });
    t.check("render gives interleaved stereo", stereo.size() == 2000 && channel(stereo, 1)[999] == -1.0f);
    t.check("a WAV out", write_wav(output("out.wav"), stereo));
    Buffer back;
    int channels = 0;
    uint32_t sample_rate = 0;
    t.check("and back", read_wav(output("out.wav"), back, channels, sample_rate) && channels == 2);
    t.same("the same samples", back, stereo);
    t.check("an input file", read_wav(input("in.wav"), back, channels, sample_rate) && sample_rate == 22050);
    enaga::log("HELLO");
    enaga::midi::note_on(2, 60, 100);
    t.check("the log", logged().size() == 1 && logged()[0] == "HELLO");
    t.check("the MIDI", midi_sent().size() == 1 && midi_sent()[0][0] == 0x92);
    run_clock(120);
    enaga::clock::Tick ticks[8];
    uint32_t count = 0;
    for (int block = 0; block < 750; block++) count += enaga::clock::ticks(ticks, 8);
    t.check("192 ticks a second at 120 BPM", count == 192);
    return t.finish();
}
"""

FAILING = """#include <enaga/testing.hpp>
int main() {
    enaga::testing::Checks t;
    t.near("one is two", 1, 2, 0.5);
    t.check("true", true);
    return t.finish();
}
"""

CRASHING = """#include <enaga/testing.hpp>
#include <cstdio>
int main() {
    std::printf("BEFORE THE CRASH\\n");
    volatile int *p = nullptr;
    return *p;
}
"""


def wav_22050():
    import struct
    data = bytes(400)
    fmt = struct.pack("<HHIIHH", 1, 1, 22050, 44100, 2, 16)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body


@unittest.skipUnless(shutil.which("g++") and shutil.which("gcc"), "needs the PC's g++ and gcc")
class HostTest(unittest.TestCase):
    def run_tool(self, app, *args):
        return subprocess.run([sys.executable, "-B", str(TOOL), str(app), *args], capture_output=True, text=True)

    def make_app(self, d, **tests):
        app = Path(d) / "app"
        (app / "tests").mkdir(parents=True)
        (app / "app.cpp").write_text(APP)
        (app / "tests" / "in.wav").write_bytes(wav_22050())
        for name, source in tests.items():
            (app / "tests" / f"{name}.cpp").write_text(source)
        return app

    def test_passing_failing_and_crashing_tests(self):
        with tempfile.TemporaryDirectory() as d:
            app = self.make_app(d, passing=PASSING, failing=FAILING, crashing=CRASHING)
            r = self.run_tool(app)
            self.assertEqual(r.returncode, 1, r.stdout + r.stderr)
            self.assertIn("ok: 12 checks", r.stdout)
            self.assertIn("FAIL one is two: got 1, want 2 (within 0.5)", r.stdout)
            self.assertIn("1 of 2 checks failed", r.stdout)
            self.assertIn("BEFORE THE CRASH", r.stdout)           # printed before the crash, not lost
            self.assertIn("crashing crashed (SIGSEGV)", r.stdout)
            self.assertIn("1 of 3 passed; failed: crashing, failing", r.stdout)
            self.assertTrue((app / "build" / "host-test" / "out.wav").is_file())
            r = self.run_tool(app, "passing")                     # one test by name
            self.assertEqual((r.returncode, r.stdout.count("== ")), (0, 1), r.stdout + r.stderr)

    def test_a_test_that_does_not_compile(self):
        with tempfile.TemporaryDirectory() as d:
            app = self.make_app(d, broken="int main() { return undefined_thing; }\n")
            r = self.run_tool(app)
            self.assertEqual(r.returncode, 2)
            self.assertIn("undefined_thing", r.stderr)
            self.assertIn("host_test: g++ failed", r.stderr)

    def test_no_tests(self):
        with tempfile.TemporaryDirectory() as d:
            app = self.make_app(d)
            r = self.run_tool(app)
            self.assertEqual(r.returncode, 2)
            self.assertIn("no tests in", r.stderr)

    def test_the_sampler_examples_tests(self):
        for args in ([], ["--sanitize"]):
            with self.subTest(args=args):
                r = self.run_tool(ENAGA / "examples" / "sampler", *args)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                self.assertIn("1 of 1 passed", r.stdout)


if __name__ == "__main__":
    unittest.main()
