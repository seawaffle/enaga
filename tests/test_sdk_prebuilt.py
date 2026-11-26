"""The SDK's DSP library prebuilt by cl6x (runtime/prebuilt, tools/make_sdk.py): linked by the SDK's build tool when
the developer has no cl6x, and GCC taking over once the library's sources are edited. Needs the toolchain and
TI's cl6x, which makes the archive. A file of its own so that tests/run.py runs it beside test_sdk.py.

    python3 -B -m unittest tests.test_sdk_prebuilt
"""
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENAGA = HERE.parent
sys.path.insert(0, str(ENAGA / "tools"))
sys.path.insert(0, str(HERE))

import build_app  # noqa: E402
import make_sdk  # noqa: E402
import paths  # noqa: E402
from test_sdk import SdkBuilds  # noqa: E402


@unittest.skipUnless(build_app.tools_present(), "needs the toolchain")
class SdkPrebuilt(SdkBuilds, unittest.TestCase):
    @unittest.skipUnless(paths.ti_present(), "needs TI's cl6x to make the prebuilt library")
    def test_prebuilt_dsp_library_without_cl6x(self):
        """An SDK made with cl6x holds the DSP library prebuilt (no debug information, nothing needed but libgcc's
        helpers). Without cl6x its build tool links that archive, giving the bytes the repository builds with cl6x;
        once a library source is edited it builds the library with GCC, giving the repository's GCC bytes."""
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            with zipfile.ZipFile(make_sdk.make("vtest", d / "sdk.zip", prebuilt=True)) as z:
                z.extractall(d)
            sdk = d / "enaga-sdk-vtest"
            self.assertTrue((sdk / "runtime" / "prebuilt" / "libenaga_dsp.a").is_file())
            no_cl6x = str(d / "no-cl6x")
            for name in ("synth", "sampler"):
                with self.subTest(app=name):
                    package = self.build_with_sdk(sdk, sdk / "examples" / name, paths.TOOLCHAIN, d, no_cl6x,
                                                  "built with GCC, with the SDK's DSP library prebuilt by cl6x")
                    here = self.build_here(ENAGA / "examples" / name, paths.TOOLCHAIN, d / "cl6x" / name,
                                           str(paths.TI_CGT))
                    self.assertEqual(package[:40] + package[64:], here[:40] + here[64:])
            filter_c = sdk / "runtime" / "dsp" / "filter.c"
            filter_c.write_text(filter_c.read_text() + "/* edited */\n")
            package = self.build_with_sdk(sdk, sdk / "examples" / "synth", paths.TOOLCHAIN, d, no_cl6x,
                                          "built with GCC only: the DSP library's sources differ")
            here = self.build_here(ENAGA / "examples" / "synth", paths.TOOLCHAIN, d / "gcc" / "synth")
            self.assertEqual(package[:40] + package[64:], here[:40] + here[64:])


if __name__ == "__main__":
    unittest.main()
