"""The SDK as an outside developer gets it (tools/make_sdk.py, tools/package_toolchain.py): the SDK zip unpacked
on its own, the template copied to a folder of the developer's, and every app built with the SDK's build tool
into the developer's folder, byte for byte as the repository builds them; the packaged (stripped) toolchain
building the same bytes as the full one; the build tool's object names and DSP library rebuilds; and the template
app playing a MIDI note on the emulator.

    python3 -B -m unittest tests.test_sdk
"""
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENAGA = HERE.parent
sys.path.insert(0, str(ENAGA / "tools"))

import build_app  # noqa: E402
import make_sdk  # noqa: E402
import paths  # noqa: E402

BOOT = 300_000_000
MS = 372_000


@unittest.skipUnless(build_app.tools_present(), "needs the toolchain")
class Sdk(unittest.TestCase):
    def build_with_sdk(self, sdk, app, toolchain, scratch):
        """The SDK's build_app.py with no options: an app outside the SDK builds into its own build/, one inside
        (the examples) into the SDK's build/apps/."""
        env = dict(os.environ, ENAGA_TOOLCHAIN=str(toolchain), ENAGA_TI_CGT="none")
        env.pop("ENAGA_BUILD", None)
        subprocess.run([sys.executable, "-B", str(sdk / "tools" / "build_app.py"), str(app)], env=env, check=True,
                       capture_output=True, cwd=scratch)
        inside = app.is_relative_to(sdk)
        return (sdk / "build" / "apps" / app.name if inside else app / "build").joinpath("APP.ENA").read_bytes()

    def build_here(self, app, toolchain, work):    # the repository's build tool, GCC only as above
        env = dict(os.environ, ENAGA_TOOLCHAIN=str(toolchain), ENAGA_TI_CGT="none", ENAGA_BUILD=str(work))
        out = Path(work) / "APP.ENA"
        subprocess.run([sys.executable, "-B", str(ENAGA / "tools" / "build_app.py"), str(app), "-o", str(out)],
                       env=env, check=True, capture_output=True)
        return out.read_bytes()

    def test_template_and_examples_build_from_the_sdk_alone(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            with zipfile.ZipFile(make_sdk.make("vtest", d / "sdk.zip")) as z:
                z.extractall(d)
            sdk = d / "enaga-sdk-vtest"
            mine = d / "projects" / "my-synth"
            shutil.copytree(sdk / "template", mine)
            for app, source in [(mine, ENAGA / "template")] + [
                    (sdk / "examples" / n, ENAGA / "examples" / n) for n in ("tone", "widgets", "synth", "sampler")]:
                with self.subTest(app=source.name):
                    package = self.build_with_sdk(sdk, app, paths.TOOLCHAIN, d)
                    here = self.build_here(source, paths.TOOLCHAIN, d / "here" / source.name)
                    name = build_app.HEADER.unpack_from(package)[10].rstrip(b"\0").decode()
                    self.assertEqual(name, app.name)          # the display name: the folder's, by default
                    self.assertEqual(package[:40] + package[64:], here[:40] + here[64:])
                    if app == mine:
                        self.assertEqual(build_app.HEADER.unpack_from(package)[9], 1)   # with the template's icon

    @unittest.skipUnless(shutil.which("g++") and shutil.which("gcc"), "needs the PC's g++ and gcc")
    def test_host_tests_run_from_the_sdk_alone(self):
        """The sampler's tests on the PC with the SDK's host_test.py, from a copy outside the SDK."""
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            with zipfile.ZipFile(make_sdk.make("vtest", d / "sdk.zip")) as z:
                z.extractall(d)
            sdk = d / "enaga-sdk-vtest"
            mine = d / "projects" / "my-sampler"
            shutil.copytree(sdk / "examples" / "sampler", mine)
            env = dict(os.environ)
            env.pop("ENAGA_BUILD", None)
            result = subprocess.run([sys.executable, "-B", str(sdk / "tools" / "host_test.py"), str(mine)], env=env,
                                    capture_output=True, text=True, cwd=d)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("1 of 1 passed", result.stdout)
            self.assertTrue((mine / "build" / "host-test" / "engine.wav").is_file())

    def test_packaged_toolchain_builds_the_same_bytes(self):
        import package_toolchain
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            env = dict(os.environ, ENAGA_BUILD=str(d))
            subprocess.run([sys.executable, "-B", str(ENAGA / "tools" / "package_toolchain.py"), "vtest"], env=env,
                           check=True, capture_output=True)
            with tarfile.open(d / "dist" / "enaga-toolchain-vtest-linux-x86_64.tar.xz") as tar:
                tar.extractall(d / "unpacked", filter="tar")
            names = tarfile.open(d / "dist" / "enaga-toolchain-vtest-source.tar").getnames()
            for name, *_ in package_toolchain.SOURCES:
                self.assertIn(f"enaga-toolchain-vtest-source/{name}", names)
            for app in (ENAGA / "template", ENAGA / "examples" / "synth"):
                with self.subTest(app=app.name):
                    self.assertEqual(self.build_here(app, d / "unpacked" / "tic6x", d / "a" / app.name),
                                     self.build_here(app, paths.TOOLCHAIN, d / "b" / app.name))

    def test_objects_of_like_names_and_a_new_compiler(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            app = d / "names"
            app.mkdir()
            (app / "app.cpp").write_text('#include <enaga/app.hpp>\nextern "C" int from_c(void);\nint from_cpp();\n'
                                         'class A : public enaga::App {\n'
                                         '    bool init() override { return from_c() + from_cpp() == 3; }\n};\n'
                                         'ENAGA_APP(A)\n')
            (app / "foo.c").write_text("int from_c(void) { return 1; }\n")          # foo.c and foo_c.cpp: two objects
            (app / "foo_c.cpp").write_text("int from_cpp() { return 2; }\n")
            (app / "runtime.cpp").write_text("int app_runtime = 0;\n")              # and none the runtime's
            saved = paths.BUILD, paths.TI_CGT
            paths.BUILD, paths.TI_CGT = d / "build", None                # GCC only: quicker
            try:
                self.assertGreater(build_app.build(app, d / "out")["payload_size"], 0)
                archive = build_app.dsp_library()
                stamp = archive.parent / "compiler.txt"
                built = archive.stat().st_mtime_ns
                self.assertEqual(build_app.dsp_library().stat().st_mtime_ns, built)     # nothing changed
                stamp.write_text("another compiler\n")
                self.assertNotEqual(build_app.dsp_library().stat().st_mtime_ns, built)  # rebuilt
                self.assertNotEqual(stamp.read_text(), "another compiler\n")
            finally:
                paths.BUILD, paths.TI_CGT = saved

    @unittest.skipUnless(paths.emulator_present(), "needs ENAGA_EMULATOR")
    def test_template_plays_a_midi_note(self):
        from emulate import Emulation
        with Emulation(ENAGA / "template") as emu:
            self.assertIsNone(emu.run(BOOT)["error"])
            def peak():
                raw, _ = emu.cpu.audio_output()
                words = struct.unpack(f"<{len(raw) // 4}i", raw)[-4096:]
                return max(abs(w) for w in words)
            self.assertEqual(peak(), 0)
            emu.cpu.uart_receive(0, bytes([0x90, 60, 100]))   # note on, C4
            self.assertIsNone(emu.run(60 * MS)["error"])
            self.assertGreater(peak(), 1 << 20)
            emu.cpu.uart_receive(0, bytes([0x80, 60, 0]))     # note off: the release (400 ms) dies away
            self.assertIsNone(emu.run(1500 * MS)["error"])
            self.assertLess(peak(), 1 << 12)


if __name__ == "__main__":
    unittest.main()
