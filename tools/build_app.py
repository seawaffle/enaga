#!/usr/bin/env python3
"""Build an Enaga app (C++ and/or C sources) into a package for the card.

    python3 -B tools/build_app.py examples/tone          # -> build/apps/tone/APP.ENA
    python3 -B tools/build_app.py ~/my-app               # an app outside Enaga's folder -> ~/my-app/build/APP.ENA
    python3 -B tools/build_app.py DIR -o /media/card/ENAGA/APPS/NAME/APP.ENA

All *.cpp and *.c files in the directory are compiled with GCC tic6x-elf (C++20, -fno-exceptions -fno-rtti,
freestanding libstdc++ headers), linked with the Enaga runtime (runtime/) and the DSP library's block functions
(runtime/dsp and runtime/libm.c, built once into build/dsplib) at ENAGA_APP_BASE with libgcc, and
wrapped as enaga_package_header (include/enaga/abi.h) + payload (+ icon). No libc. The package format is provisional
until 1.0.

An optional icon.txt in the directory is the app's icon in the Enaga menu: 16 lines of 16 greys, each a hex digit
0 (black) to F (white) or "." for 0; lines starting with "#" are comments.

Files named *.ti.c, and the DSP library, are built with TI's cl6x instead when it is installed (tools/paths.py, TI_CGT): its
software pipelining makes inner DSP loops several times faster than GCC's code. They must be C that GCC also
compiles (the fallback) and export plain C functions, since C++ objects don't mix between the compilers.
"""
import argparse
import re
import struct
import subprocess
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import paths  # noqa: E402

GCC, GXX, LD, OBJCOPY, NM, AR = (paths.tool(name) for name in ("gcc", "g++", "ld", "objcopy", "nm", "ar"))
RUNTIME = paths.ROOT / "runtime"
INCLUDE = paths.ROOT / "include"
WORK = paths.BUILD / "apps"

ABI_VERSION = 0
MAGIC = 0x41474E45
BASE = 0xC0400000
LIMIT = 0xC8000000
HEADER = struct.Struct("<10I24s")
FLAG_ICON = 1
ICON_SIZE = 16
# Every GCC frame must fit in the 16 KiB that Enaga's interrupt entry leaves below the interrupted stack pointer
# (firmware/start.s, "Interrupt stack gap"): GCC can release a frame a few cycles before its last reads of it.
COMMON = ["-march=c674x", "-mlittle-endian", "-O2", "-Werror=frame-larger-than=16384", "-Werror=vla", "-Werror=alloca", "-ffreestanding", "-nostdlib", "-fno-asynchronous-unwind-tables",
          "-fno-exceptions", "-msdata=none", "-funroll-loops", "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra", "-Werror",
          f"-I{INCLUDE}", f"-isystem{INCLUDE / 'libm'}"]   # <math.h> and <cmath>: runtime/libm.c
CFLAGS = ["-std=c11", *COMMON]
CXXFLAGS = ["-std=c++20", "-fno-rtti", *COMMON]
# cl6x for *.ti.c: far data (Enaga apps have no data page pointer), a section per function for --gc-sections.
TIFLAGS = ["-mv6740", "--abi=eabi", "--c99", "-O3", "--mem_model:data=far", "--mem_model:const=far",
           "--gen_func_subsections=on", "--emit_warnings_as_errors", "--display_error_number", "--quiet",
           f"-I{INCLUDE}"]


def tools_present():
    return paths.toolchain_present()


def run(args, cwd):
    subprocess.run([str(a) for a in args], cwd=cwd, check=True)


def symbols(elf):
    out = {}
    for line in subprocess.run([str(NM), str(elf)], capture_output=True, text=True, check=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            out[parts[2]] = int(parts[0], 16)
    return out


def compile_ti(src, obj, cwd):
    run([paths.TI_CGT / "bin" / "cl6x", *TIFLAGS, f"-I{paths.TI_CGT / 'include'}", f"--output_file={obj}", "-c", src],
        cwd)
    return obj


def compile_source(src, out):
    if src.name.endswith(".ti.c") and paths.ti_present():
        return compile_ti(src, out / (src.name[:-len(".ti.c")] + "_ti.o"), out)
    obj = out / (src.stem + ("_c" if src.suffix == ".c" else "") + ".o")
    compiler, flags = (GCC, CFLAGS) if src.suffix == ".c" else (GXX, CXXFLAGS)
    run([compiler, *flags, "-c", "-o", obj, src], out)
    return obj


def dsp_library():
    """The DSP library's block functions (runtime/dsp/*.c, include/enaga/dsp.h) and libm (runtime/libm.c) as an
    archive: built by cl6x when it is installed, by GCC otherwise, and again only when a source, a header or this
    script changes."""
    ti = paths.ti_present()
    lib = paths.BUILD / "dsplib" / ("cl6x" if ti else "gcc")
    archive = lib / "libenaga_dsp.a"
    sources = [*sorted((RUNTIME / "dsp").glob("*.c")), RUNTIME / "libm.c"]
    inputs = [*sources, INCLUDE / "enaga" / "dsp.h", *(INCLUDE / "enaga" / "dsp").glob("*.h"), Path(__file__)]
    if archive.exists() and archive.stat().st_mtime >= max(p.stat().st_mtime for p in inputs):
        return archive
    lib.mkdir(parents=True, exist_ok=True)
    objs = []
    for src in sources:
        obj = lib / (src.stem + ".o")
        if ti:
            compile_ti(src, obj, lib)
        else:
            run([GCC, *CFLAGS, "-c", "-o", obj, src], lib)
        objs.append(obj)
    archive.unlink(missing_ok=True)
    run([AR, "rcs", archive, *objs], lib)
    return archive


def read_icon(path):
    """icon.txt -> the package's icon bytes (ENAGA_ICON_BYTES: two pixels a byte, the left one high)."""
    rows = [line.strip() for line in path.read_text().splitlines()]
    rows = [row for row in rows if row and not row.startswith("#")]
    if len(rows) != ICON_SIZE or any(not re.fullmatch(r"[0-9A-Fa-f.]{16}", row) for row in rows):
        raise ValueError(f"{path}: an icon is {ICON_SIZE} lines of {ICON_SIZE} hex digits (0-F, or . for 0)")
    greys = [0 if c == "." else int(c, 16) for row in rows for c in row]
    return bytes(greys[i] << 4 | greys[i + 1] for i in range(0, len(greys), 2))


def build(app_dir, out, name=None, app_version=1):
    """Compile, link and package; returns {"package": bytes, "elf", "payload_size", "bss_size", "entry"}."""
    app_dir, out = Path(app_dir).resolve(), Path(out).resolve()
    name = name or app_dir.name
    encoded = name.encode("ascii")
    if not encoded or len(encoded) >= 24 or not re.fullmatch(rb"[ -~]+", encoded):
        raise ValueError("app name must be 1-23 printable ASCII characters")
    sources = sorted(app_dir.glob("*.cpp")) + sorted(app_dir.glob("*.c"))
    if not sources:
        raise ValueError(f"no .cpp or .c sources in {app_dir}")
    out.mkdir(parents=True, exist_ok=True)
    objs = [compile_source(src, out) for src in sources]
    objs.append(compile_source(RUNTIME / "runtime.cpp", out))
    libgcc = paths.libgcc()
    elf = out / "app.elf"
    run([LD, "-EL", "-T", RUNTIME / "app.ld", "-nostdlib", "--gc-sections", "-u", "enaga_entry",
         "--no-warn-rwx-segments", "--orphan-handling=error", "-Map", out / "app.map", "-o", elf, *objs, dsp_library(),
         libgcc], out)
    undefined = subprocess.run([str(NM), "-u", str(elf)], capture_output=True, text=True, check=True).stdout.split()
    if undefined:
        raise RuntimeError("app references symbols Enaga does not provide: " + " ".join(sorted(set(undefined) - {"U"})))
    # --gc-sections drops unreferenced sections before orphan checks run: make sure no constructor table
    # was lost (app.ld KEEPs .init_array and .ctors; anything else would be a silent miss).
    lost = re.findall(r"^\s*(\.(?:init_array|ctors|preinit_array)\S*)\s+0x[0-9a-f]+\s+0x([0-9a-f]+)\s+(\S+)$",
                      (out / "app.map").read_text().split("Memory Configuration")[0], re.M)
    lost = [entry for entry in lost if int(entry[1], 16)]
    if lost:
        raise RuntimeError("constructor tables discarded by the link: " + ", ".join(f"{s} in {o}" for s, _, o in lost))
    raw = out / "app.raw"
    run([OBJCOPY, "-O", "binary", "-j", ".text", "-j", ".rodata", "-j", ".init_array", "-j", ".data", elf, raw], out)
    payload = raw.read_bytes()
    sym = symbols(elf)
    # The loader zeroes from the end of the payload to __bss_end, alignment gap included.
    if not BASE + len(payload) <= sym["__bss_start"] <= sym["__bss_end"] <= LIMIT:
        raise RuntimeError("bss does not follow the payload inside the app window")
    bss = sym["__bss_end"] - (BASE + len(payload))
    entry = sym["enaga_entry"] - BASE
    icon = read_icon(app_dir / "icon.txt") if (app_dir / "icon.txt").exists() else b""
    header = HEADER.pack(MAGIC, HEADER.size, ABI_VERSION, BASE, len(payload), bss, entry,
                         zlib.crc32(payload) & 0xFFFFFFFF, app_version, FLAG_ICON if icon else 0, encoded)
    ti_sources = [src.name for src in sources if src.name.endswith(".ti.c")] if paths.ti_present() else []
    return {"package": header + payload + icon, "elf": elf, "payload_size": len(payload), "bss_size": bss, "entry": entry,
            "ti_sources": ti_sources}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("app_dir", type=Path)
    ap.add_argument("-o", "--output", type=Path)
    ap.add_argument("--name", help="display name (default: the directory name)")
    ap.add_argument("--app-version", type=int, default=1)
    args = ap.parse_args(argv)
    if not tools_present():
        print(f"the tic6x compiler is not in {paths.TOOLCHAIN}: unpack enaga-toolchain-<version>-linux-x86_64.tar.xz "
              f"into {paths.ROOT / 'tools'}, or set ENAGA_TOOLCHAIN to where it is "
              "(docs/getting-started.md, step 1)", file=sys.stderr)
        return 2
    app = args.app_dir.resolve()
    out = WORK / app.name if app.is_relative_to(paths.ROOT) else app / "build"
    try:
        b = build(args.app_dir, out, args.name, args.app_version)
    except (ValueError, RuntimeError) as e:
        print(f"build_app: {e}", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as e:     # the compiler or linker has said why
        print(f"build_app: {Path(str(e.cmd[0])).name} failed", file=sys.stderr)
        return 1
    target = args.output or out / "APP.ENA"
    target.write_bytes(b["package"])
    print(f"wrote {target}: {len(b['package'])} bytes (payload {b['payload_size']}, bss {b['bss_size']}, entry +{b['entry']:#x})")
    print("built with cl6x: " + ", ".join(["the DSP library", *b["ti_sources"]]) if paths.ti_present() else
          "built with GCC only (TI's cl6x not found: docs/getting-started.md, step 3)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
