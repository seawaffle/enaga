#!/usr/bin/env python3
"""Make the Enaga SDK: what building an app needs, without the rest of the repository.

    python3 -B tools/make_sdk.py v0.3.0                  # -> build/dist/enaga-sdk-v0.3.0.zip

The zip holds enaga-sdk-<tag>/ with the headers (include/), the app runtime and the DSP library's sources
(runtime/), the build tool (tools/build_app.py, tools/paths.py), the app template (template/), the examples, the
getting-started guide and the API reference (docs/). The compiler comes separately
(enaga-toolchain-<tag>-linux-x86_64.tar.xz, tools/package_toolchain.py), unpacked into the SDK's tools/ folder,
where tools/paths.py looks for it.
"""
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import paths  # noqa: E402

ROOT = paths.ROOT
TREES = ["include", "runtime", "template", "examples/tone", "examples/widgets", "examples/synth"]
FILES = ["LICENSE", "tools/build_app.py", "tools/paths.py", "docs/getting-started.md", "docs/api.md"]
README = """Enaga SDK {tag}

Everything needed to build apps for Enaga, the replacement firmware for the Sonicware SmplTrek, Lofi-12 XT and
CyDrums. Start with docs/getting-started.md.

  include/      the API headers (<enaga/app.hpp> and the rest; docs/api.md)
  runtime/      the app runtime and the DSP library, compiled into every app
  tools/        build_app.py, which builds an app folder into APP.ENA; unpack the toolchain here (tools/tic6x)
  template/     a small app to copy and start from
  examples/     tone (the smallest app), widgets (the UI toolkit), synth (a full instrument)
  docs/         getting-started.md, api.md
"""


def files():
    for tree in TREES:
        for path in sorted((ROOT / tree).rglob("*")):
            if path.is_file() and "__pycache__" not in path.parts and "build" not in path.relative_to(ROOT / tree).parts:
                yield path.relative_to(ROOT)
    for name in FILES:
        yield Path(name)


def make(tag, target):
    top = f"enaga-sdk-{tag}"
    target.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(f"{top}/README.txt", README.format(tag=tag))
        for rel in files():
            info = zipfile.ZipInfo(f"{top}/{rel.as_posix()}", (2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o755 if rel.suffix == ".py" else 0o644) << 16
            z.writestr(info, (ROOT / rel).read_bytes())
    return target


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    target = make(argv[0], paths.BUILD / "dist" / f"enaga-sdk-{argv[0]}.zip")
    print(f"wrote {target}: {target.stat().st_size / 1e6:.1f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
