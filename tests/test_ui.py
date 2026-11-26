"""The UI toolkit (include/enaga/ui.hpp) compiled for the host and checked by tests/ui/host.cpp: list movement and
scrolling, the parameter page's knobs, groups and resets, the dialog and text entry, and what each draws; and
enaga::Settings (settings.hpp) on a one-file stand-in. Needs only a host g++.

    python3 -B -m unittest discover -s tests -p test_ui.py -v
"""
import shutil
import subprocess
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent


@unittest.skipUnless(shutil.which("g++"), "host g++ not found")
class Ui(unittest.TestCase):
    def test_ui_toolkit_on_the_host(self):
        out = ROOT / "build" / "ui"
        out.mkdir(parents=True, exist_ok=True)
        subprocess.run(["g++", "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", f"-I{ROOT / 'include'}",
                        "-o", out / "host", HERE / "ui" / "host.cpp"], check=True)
        result = subprocess.run([out / "host"], capture_output=True, text=True)
        self.assertEqual((result.returncode, result.stdout.strip()), (0, "ok"), result.stdout)


if __name__ == "__main__":
    unittest.main()
