#!/usr/bin/env python3
"""Run Enaga and apps in the SmplTrek emulator's web panel (live display, controls and audio).

    ENAGA_EMULATOR=/path/to/emulator python3 -B tools/panel.py [app_dir ...] [--file CARD_PATH=LOCAL ...]
        [--card-mib N] [-- panel options]

Builds the firmware (firmware/build.py) and each app (tools/build_app.py; examples/synth if none is given), writes
the packages to /ENAGA/APPS/<NAME>/APP.ENA on a fresh card image (build/panel/card.img, 64 MiB unless --card-mib
says otherwise, built by tools/card.py), with any --file files, long names included, and starts the
emulator's panel, stock_ui.py, on the firmware's update file. Then open the address it prints (default
http://127.0.0.1:8766) and press Enable audio. Ctrl-C stops it. Enaga starts the first app it finds.

For example `tools/panel.py examples/synth --file "/SAMPLES/Kick long name.wav=kick.wav"`. Options after `--` go to
the panel unchanged, e.g. `tools/panel.py examples/tone -- --port 8770 --labels sampler`
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
import card  # noqa: E402
import paths  # noqa: E402
from emulate import same_card_path  # noqa: E402

# The emulator has its own `build` module; load the firmware's by path under a distinct name.
_spec = importlib.util.spec_from_file_location("enaga_firmware_build", ENAGA / "firmware" / "build.py")
firmware_build = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(firmware_build)

WORK = paths.BUILD / "panel"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 epilog="Options after -- are passed to the emulator's stock_ui.py.")
    ap.add_argument("app_dirs", type=Path, nargs="*", help="app directories (default: examples/synth)")
    ap.add_argument("--card-mib", type=int, default=64, help="card size: a power of two, 2-1024 MiB")
    ap.add_argument("--file", action="append", default=[], metavar="CARD_PATH=LOCAL",
                    help='put a local file on the card, as "/SAMPLES/Kick long name.wav=kick.wav" (can be repeated)')
    argv = sys.argv[1:] if argv is None else list(argv)
    split = argv.index("--") if "--" in argv else len(argv)
    args, panel_args = ap.parse_args(argv[:split]), argv[split + 1:]
    try:
        paths.use_emulator()
        extra = card.file_args(args.file)
    except (RuntimeError, ValueError, FileNotFoundError) as error:
        print(f"panel: {error}", file=sys.stderr)
        return 2

    firmware = firmware_build.build()
    try:
        image = build_card(args.app_dirs, extra, args.card_mib)
    except card.CardFull as error:
        print(f"panel: {error}; use a larger --card-mib", file=sys.stderr)
        return 2
    except (ValueError, FileNotFoundError) as error:
        print(f"panel: {error}", file=sys.stderr)
        return 2
    print(f"firmware {firmware['image_path']}, card {image}")

    command = [sys.executable, "-B", str(paths.EMULATOR / "stock_ui.py"),
               "--firmware", str(firmware["image_path"]), "--card", str(image), *panel_args]
    try:
        return subprocess.call(command)
    except KeyboardInterrupt:
        return 0


def build_card(app_dirs, extra, card_mib=64):
    """The panel's card: each app's package (examples/synth if none) and `extra` ({card path: local file})."""
    files = {}
    for app_dir in app_dirs or [ENAGA / "examples" / "synth"]:
        app_dir = app_dir.resolve()
        name = app_dir.name.upper()[:8]
        package = build_app.build(app_dir, paths.BUILD / "apps" / app_dir.name, name)["package"]
        local = WORK / f"{name}.ENA"
        local.parent.mkdir(parents=True, exist_ok=True)
        local.write_bytes(package)
        app_path = f"/ENAGA/APPS/{name}/APP.ENA"
        if same_card_path(app_path, extra):
            raise ValueError(f"--file collides with app {name}'s {app_path}")
        files[app_path] = local
        print(f"app {name}: {len(package)} bytes")
    return card.make(WORK / "card.img", card_mib, "fat", {**files, **extra})


if __name__ == "__main__":
    sys.exit(main())
