#!/usr/bin/env python3
"""Run Enaga and apps in the SmplTrek emulator's web panel (live display, controls and audio).

    ENAGA_EMULATOR=/path/to/emulator python3 -B tools/panel.py [app_dir ...] [-- panel options]

Builds the firmware (firmware/build.py) and each app (tools/build_app.py; examples/synth if none is given), writes
the packages to /ENAGA/APPS/<NAME>/APP.ENA on a fresh 64 MiB card image (build/panel/card.img), and starts the
emulator's panel, stock_ui.py, on the firmware's update file. Then open the address it prints (default
http://127.0.0.1:8766) and press Enable audio. Ctrl-C stops it. Enaga starts the first app it finds.

Options after `--` go to the panel unchanged, e.g. `tools/panel.py examples/tone -- --port 8770 --labels sampler`
(`stock_ui.py --help` lists them).
"""
import argparse
import importlib.util
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENAGA = HERE.parent
sys.path.insert(0, str(HERE))

import build_app  # noqa: E402
import paths  # noqa: E402

# The emulator has its own `build` module; load the firmware's by path under a distinct name.
_spec = importlib.util.spec_from_file_location("enaga_firmware_build", ENAGA / "firmware" / "build.py")
firmware_build = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(firmware_build)

WORK = paths.BUILD / "panel"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 epilog="Options after -- are passed to the emulator's stock_ui.py.")
    ap.add_argument("app_dirs", type=Path, nargs="*", help="app directories (default: examples/synth)")
    argv = sys.argv[1:] if argv is None else list(argv)
    split = argv.index("--") if "--" in argv else len(argv)
    args, panel_args = ap.parse_args(argv[:split]), argv[split + 1:]
    try:
        paths.use_emulator()
    except RuntimeError as error:
        print(f"panel: {error}", file=sys.stderr)
        return 2
    from media import build_card

    firmware = firmware_build.build()
    files = {}
    for app_dir in args.app_dirs or [ENAGA / "examples" / "synth"]:
        app_dir = app_dir.resolve()
        name = app_dir.name.upper()[:8]
        package = build_app.build(app_dir, paths.BUILD / "apps" / app_dir.name, name)["package"]
        local = WORK / f"{name}.ENA"
        local.parent.mkdir(parents=True, exist_ok=True)
        local.write_bytes(package)
        files[f"/ENAGA/APPS/{name}/APP.ENA"] = local
        print(f"app {name}: {len(package)} bytes")
    card = build_card(WORK / "card.img", files, 64)
    print(f"firmware {firmware['image_path']}, card {card}")

    command = [sys.executable, "-B", str(paths.EMULATOR / "stock_ui.py"),
               "--firmware", str(firmware["image_path"]), "--card", str(card), *panel_args]
    try:
        return subprocess.call(command)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
