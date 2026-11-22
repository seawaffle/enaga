#!/usr/bin/env python3
"""Stage the SD card files for a session on a unit (docs/hardware.md "Checks on a unit").

    python3 -B tools/unit_card.py        # -> build/unit-card/{install,app}/

install/SmplTrek.bin            the Enaga update; alone on the card for the install and the first start-up
app/ENAGA/APPS/CHECK/APP.ENA    the CHECK app (tests/unit/check); copy the ENAGA folder to the card afterwards
synth/ENAGA/APPS/SYNTH/APP.ENA  the synth (examples/synth), for the last step: replaces the card's ENAGA folder
widgets/ENAGA/APPS/WIDGETS/APP.ENA  the UI toolkit's example (examples/widgets): copy its WIDGETS folder into the
                                card's /ENAGA/APPS beside the others
app/ENAGA/SDTEST.BIN (and synth/)  4 KiB of zeros: with it on the card each boot tests SD writes (CMD24, CMD25)

Prints each file's size and SHA-256 so a copy on the card can be compared.
"""
import hashlib
import importlib.util
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_app  # noqa: E402
import paths  # noqa: E402

_spec = importlib.util.spec_from_file_location("enaga_firmware_build", paths.ROOT / "firmware" / "build.py")
firmware_build = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(firmware_build)

OUT = paths.BUILD / "unit-card"


def main():
    if not build_app.tools_present():
        print("tic6x GCC not built (tools/build_tic6x_*.sh)", file=sys.stderr)
        return 2
    firmware = firmware_build.build()
    package = build_app.build(paths.ROOT / "tests" / "unit" / "check", paths.BUILD / "apps" / "check", "CHECK")["package"]
    synth = build_app.build(paths.ROOT / "examples" / "synth", paths.BUILD / "apps" / "synth", "SYNTH")["package"]
    widgets = build_app.build(paths.ROOT / "examples" / "widgets", paths.BUILD / "apps" / "widgets", "WIDGETS")["package"]
    shutil.rmtree(OUT, ignore_errors=True)
    files = {OUT / "install" / "SmplTrek.bin": firmware["image_path"].read_bytes(),
             OUT / "app" / "ENAGA" / "APPS" / "CHECK" / "APP.ENA": package,
             OUT / "synth" / "ENAGA" / "APPS" / "SYNTH" / "APP.ENA": synth,
             OUT / "widgets" / "ENAGA" / "APPS" / "WIDGETS" / "APP.ENA": widgets,
             # The write probe's file (files.c files_write_probe): its sectors take the SD write checks at each boot.
             OUT / "app" / "ENAGA" / "SDTEST.BIN": bytes(4096),
             OUT / "synth" / "ENAGA" / "SDTEST.BIN": bytes(4096)}
    for path, data in files.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        print(f"{path.relative_to(OUT)}  {len(data)} bytes  sha256 {hashlib.sha256(data).hexdigest()}")
    print(f"staged in {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
