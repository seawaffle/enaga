#!/usr/bin/env python3
"""Make the Enaga SDK: what building an app needs, without the rest of the repository.

    python3 -B tools/make_sdk.py v0.4.0                  # -> build/dist/enaga-sdk-v0.4.0.zip
    python3 -B tools/make_sdk.py v0.4.0 --prebuilt       # a release: fails without cl6x

The zip holds enaga-sdk-<tag>/ with the headers (include/), the app runtime and the DSP library's sources
(runtime/), the build tool (tools/build_app.py, tools/paths.py), the host test tool (tools/host_test.py), the USB
debug port's tool (tools/usb_debug.py), the app template (template/), the examples, the getting-started guide, the
API reference and the debug port's guide (docs/). The compiler comes separately
(enaga-toolchain-<tag>-linux-x86_64.tar.xz, tools/package_toolchain.py), unpacked into the SDK's tools/ folder,
where tools/paths.py looks for it.

With TI's cl6x installed (tools/paths.py, TI_CGT), the SDK also holds the DSP library prebuilt by cl6x
(runtime/prebuilt: build_app.prebuilt_dsp_library), which the build tool links when the developer has no cl6x.
--prebuilt makes it required, as a release does.
"""
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build_app  # noqa: E402
import paths  # noqa: E402

ROOT = paths.ROOT
TREES = ["include", "runtime", "template", "examples/tone", "examples/widgets", "examples/synth", "examples/sampler",
         "examples/recorder"]
FILES = ["LICENSE", "tools/build_app.py", "tools/host_test.py", "tools/paths.py", "tools/usb_debug.py",
         "docs/getting-started.md", "docs/api.md", "docs/usb-debug.md"]
README = """Enaga SDK {tag}

Everything needed to build apps for Enaga, the replacement firmware for the Sonicware SmplTrek, Lofi-12 XT and
CyDrums. Start with docs/getting-started.md.

  include/      the API headers (<enaga/app.hpp> and the rest; docs/api.md)
  runtime/      the app runtime and the DSP library, compiled into every app; host/, for tests on a PC;
                prebuilt/, the DSP library compiled by TI's cl6x, which apps link unless its sources are edited
  tools/        build_app.py, which builds an app folder into APP.ENA (unpack the toolchain here, tools/tic6x);
                host_test.py, which runs an app's tests on a PC; usb_debug.py, which follows a unit's log, takes
                screenshots and presses its keys over USB (Windows, macOS or Linux; docs/usb-debug.md)
  template/     a small app to copy and start from
  examples/     tone (the smallest app), widgets (the UI toolkit), synth (a full instrument), sampler (WAV files
                on the pads, with tests on a PC), recorder (the input to WAV files)
  docs/         getting-started.md, api.md, usb-debug.md
"""


def files():
    for tree in TREES:
        for path in sorted((ROOT / tree).rglob("*")):
            if path.is_file() and "__pycache__" not in path.parts and "build" not in path.relative_to(ROOT / tree).parts:
                yield path.relative_to(ROOT)
    for name in FILES:
        yield Path(name)


def make(tag, target, prebuilt=None):
    """prebuilt: True requires the DSP library prebuilt by cl6x, False leaves it out, None includes it when cl6x is
    installed."""
    if prebuilt is None:
        prebuilt = paths.ti_present()
    top = f"enaga-sdk-{tag}"
    target.parent.mkdir(parents=True, exist_ok=True)
    extra = {}
    if prebuilt:
        lib = paths.BUILD / "dsplib" / "prebuilt"
        build_app.prebuilt_dsp_library(lib)
        extra = {Path("runtime/prebuilt") / name: (lib / name).read_bytes() for name in ("libenaga_dsp.a", "sources.txt")}
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(f"{top}/README.txt", README.format(tag=tag))
        for rel, data in [*((rel, (ROOT / rel).read_bytes()) for rel in files()), *extra.items()]:
            info = zipfile.ZipInfo(f"{top}/{rel.as_posix()}", (2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o755 if rel.suffix == ".py" else 0o644) << 16
            z.writestr(info, data)
    return target


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    required = "--prebuilt" in argv
    argv = [a for a in argv if a != "--prebuilt"]
    if len(argv) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    target = make(argv[0], paths.BUILD / "dist" / f"enaga-sdk-{argv[0]}.zip", True if required else None)
    print(f"wrote {target}: {target.stat().st_size / 1e6:.1f} MB, " +
          ("with the DSP library prebuilt by cl6x" if required or paths.ti_present() else
           "without a prebuilt DSP library (no cl6x)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
