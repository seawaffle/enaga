#!/usr/bin/env python3
"""SD card images for the emulator, through Enaga's own FatFs (FAT or exFAT, long names).

    python3 -B tools/card.py IMG make SIZE_MB [fat|exfat]
    python3 -B tools/card.py IMG put CARD_PATH LOCAL | get CARD_PATH LOCAL | ls CARD_PATH | free

Builds tools/cardtool.c once (host gcc) with firmware/third_party/fatfs and firmware/hal/ff_unicode.c under the
firmware's FatFs configuration, plus formatting. Images are 2 MiB to 1 GiB, a power of two (the emulator's
card). From Python: make(), put(), get(), listdir(), free().
"""
import hashlib
import re
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
FATFS = ROOT / "firmware" / "third_party" / "fatfs"
SOURCES = [FATFS / "ff.c", FATFS / "ff.h", FATFS / "diskio.h", ROOT / "firmware" / "hal" / "ff_unicode.c", HERE / "cardtool.c"]
sys.path.insert(0, str(HERE))
import paths  # noqa: E402


def tool():
    config = (FATFS / "ffconf.h").read_text()
    config, count = re.subn(r"(#define\s+FF_USE_MKFS\s+)0\b", r"\g<1>1", config)
    if count != 1:
        raise RuntimeError("expected FF_USE_MKFS 0 in the firmware's ffconf.h")
    digest = hashlib.sha256(config.encode() + b"".join(p.read_bytes() for p in SOURCES)).hexdigest()[:16]
    out = paths.BUILD / "cardtool" / digest
    exe = out / "cardtool"
    if not exe.exists():
        out.mkdir(parents=True, exist_ok=True)
        for p in SOURCES:
            (out / p.name).write_bytes(p.read_bytes())
        (out / "ffconf.h").write_text(config)
        subprocess.run(["gcc", "-O2", "-w", "-o", "cardtool.pending", "cardtool.c", "ff.c", "ff_unicode.c"], cwd=out, check=True)
        (out / "cardtool.pending").replace(exe)
    return exe


def run(image, *args, check=True):
    result = subprocess.run([str(tool()), str(image), *map(str, args)], capture_output=True, text=True)
    if check and result.returncode:
        raise RuntimeError(f"cardtool {' '.join(map(str, args))}: {result.stderr.strip() or result.returncode}")
    return result


def make(image, size_mb=64, fs="fat", files=None):
    """A new image; `files` maps card paths to bytes or local paths."""
    if size_mb not in (2, 4, 8, 16, 32, 64, 128, 256, 512, 1024):
        raise ValueError("card size must be a power of two from 2 to 1024 MiB")
    image = Path(image)
    image.parent.mkdir(parents=True, exist_ok=True)
    run(image, "make", size_mb, fs)
    for card_path, content in (files or {}).items():
        put(image, card_path, content)
    return image


def put(image, card_path, content):
    if isinstance(content, (bytes, bytearray)):
        with tempfile.NamedTemporaryFile(delete=False) as f:
            f.write(content)
        try:
            run(image, "put", card_path, f.name)
        finally:
            Path(f.name).unlink()
    else:
        run(image, "put", card_path, content)


def get(image, card_path):
    """The file's bytes, or None if it is not there."""
    with tempfile.TemporaryDirectory() as d:
        local = Path(d) / "out"
        if run(image, "get", card_path, local, check=False).returncode:
            return None
        return local.read_bytes()


def listdir(image, card_path):
    """[(name, size or None for a folder)], or None if the folder is not there."""
    result = run(image, "ls", card_path, check=False)
    if result.returncode:
        return None
    entries = []
    for line in result.stdout.splitlines():
        if line.startswith("D "):
            entries.append((line[2:], None))
        else:
            _, size, name = line.split(" ", 2)
            entries.append((name, int(size)))
    return entries


def free(image):
    return int(run(image, "free").stdout)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) >= 2 and argv[1] == "make":
        make(argv[0], int(argv[2]), argv[3] if len(argv) > 3 else "fat")
        return 0
    result = run(*argv, check=False)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
