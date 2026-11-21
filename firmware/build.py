#!/usr/bin/env python3
"""Build the Enaga firmware with GCC tic6x-elf and wrap it as a SYSTEM update for each device of the family.

    python3 -B firmware/build.py        # build/firmware/{enaga.elf,enaga.map,enaga.dis,ENAGA_<version>.bin,<device>/<card name>}

Sources: start.s, link.ld, hal/*.c (drivers), kernel/*.c, usb/*.c, the package loader (loader/), FatFs and TinyUSB. The loadable
bytes (.text, .vectors, .rodata, .data, contiguous from 0xC0000000) become one section entered at `_start`,
wrapped by tools/sysimage.py into the update file the unit's bootloader installs (docs/hardware.md).
"""
import argparse
import hashlib
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tools"))
import paths  # noqa: E402
import sysimage  # noqa: E402

ENAGA = paths.ROOT
GCC, AS, LD, OBJCOPY, OBJDUMP, NM = (paths.tool(n) for n in ("gcc", "as", "ld", "objcopy", "objdump", "nm"))
OUT = paths.BUILD / "firmware"

VERSION = (0, 2, 0)
LOAD = 0xC0000000
# Every GCC frame must fit in the 16 KiB that Enaga's interrupt entry leaves below the interrupted stack pointer
# (firmware/start.s, "Interrupt stack gap"): GCC can release a frame a few cycles before its last reads of it.
CFLAGS = ["-march=c674x", "-mlittle-endian", "-O2", "-Werror=frame-larger-than=16384", "-Werror=vla", "-Werror=alloca", "-ffreestanding", "-fno-builtin", "-nostdlib",
          "-fno-asynchronous-unwind-tables", "-fno-exceptions", "-msdata=none", "-Wall", "-Wextra", "-Werror",
          "-Wno-main", f"-I{HERE / 'include'}", f"-I{ENAGA / 'include'}", f"-I{ENAGA / 'loader'}",
          f"-I{HERE / 'third_party' / 'fatfs'}", f"-I{HERE / 'usb'}", f"-I{HERE / 'third_party' / 'tinyusb' / 'src'}"]
TINYUSB = HERE / "third_party" / "tinyusb" / "src"
TINYUSB_SOURCES = ["tusb.c", "common/tusb_fifo.c", "device/usbd.c", "class/midi/midi_device.c",
                   "portable/mentor/musb/dcd_musb.c"]


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


def build(out=OUT):
    """Compile and link; returns {"elf", "image", "symbols", "code_size", ...}."""
    out.mkdir(parents=True, exist_ok=True)
    objs = [out / "start.o"]
    run([AS, "-mlittle-endian", "-march=c674x", "-o", objs[0], HERE / "start.s"], out)
    sources = (sorted((HERE / "hal").glob("*.c")) + sorted((HERE / "kernel").glob("*.c")) + sorted((HERE / "usb").glob("*.c"))
               + [ENAGA / "loader" / "loader.c"])
    for src in sources:
        obj = out / f"{src.parent.name}_{src.stem}.o"
        run([GCC, *CFLAGS, "-c", "-o", obj, src], out)
        objs.append(obj)
    for name in TINYUSB_SOURCES:                          # third-party code: not held to -Werror
        obj = out / ("tinyusb_" + name.replace("/", "_").replace(".c", ".o"))
        run([GCC, *[f for f in CFLAGS if f != "-Werror"], "-c", "-o", obj, TINYUSB / name], out)
        objs.append(obj)
    ff = HERE / "third_party" / "fatfs" / "ff.c"          # third-party code: not held to -Werror
    run([GCC, *[f for f in CFLAGS if f != "-Werror"], "-c", "-o", out / "ff.o", ff], out)
    objs.append(out / "ff.o")
    libgcc = paths.libgcc()
    elf, mapfile, raw = out / "enaga.elf", out / "enaga.map", out / "enaga.bin"
    run([LD, "-EL", "-T", HERE / "link.ld", "-nostdlib", f"-Map={mapfile}", "--no-warn-rwx-segments",
         "--orphan-handling=error", "-o", elf, *objs, libgcc], out)
    run([OBJCOPY, "-O", "binary", "-j", ".text", "-j", ".vectors", "-j", ".rodata", "-j", ".data", elf, raw], out)
    (out / "enaga.dis").write_text(subprocess.run([str(OBJDUMP), "-d", str(elf)], capture_output=True, text=True, check=True).stdout)
    undefined = subprocess.run([str(NM), "-u", str(elf)], capture_output=True, text=True, check=True).stdout.split()
    if undefined:
        raise RuntimeError("undefined symbols: " + " ".join(undefined))
    sym = symbols(elf)
    if sym["_start"] != LOAD:
        raise RuntimeError(f"entry {sym['_start']:#x} is not the load address")
    code = raw.read_bytes()
    # One image for the whole family; each device gets its own update file, named for the bootloader to find it.
    images = {}
    for device, d in sysimage.DEVICES.items():
        folder = out / device
        folder.mkdir(exist_ok=True)
        images[device] = folder / d["card_name"]
        images[device].write_bytes(sysimage.wrap(VERSION, sym["_start"], [(LOAD, code)], device))
    name = "ENAGA_" + "_".join(map(str, VERSION)) + ".bin"           # the SmplTrek's, under the release name
    (out / name).write_bytes(images["smpltrek"].read_bytes())
    return {"elf": elf, "map": mapfile, "image": (out / name).read_bytes(), "image_path": out / name,
            "images": images, "symbols": sym, "code_size": len(code)}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.parse_args(argv)
    if not tools_present():
        print(f"GCC tic6x-elf not found in {paths.TOOLCHAIN}; run tools/build_tic6x_binutils.sh and "
              "tools/build_tic6x_gcc.sh, or set ENAGA_TOOLCHAIN", file=sys.stderr)
        return 2
    b = build()
    print(f"wrote {b['image_path']}: {len(b['image'])} bytes, SHA-256 {hashlib.sha256(b['image']).hexdigest()}")
    for device, path in b["images"].items():
        print(f"  {sysimage.DEVICES[device]['label']:10}  {path.relative_to(ENAGA)}  "
              f"SHA-256 {hashlib.sha256(path.read_bytes()).hexdigest()[:16]}")
    print(f"code {b['code_size']} bytes at {LOAD:#x}, ELF {b['elf']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
