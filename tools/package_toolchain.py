#!/usr/bin/env python3
"""Package the tic6x toolchain (tools/tic6x, from tools/build_tic6x_*.sh) for a release, with its sources.

    python3 -B tools/package_toolchain.py v0.4.0          # -> build/dist/enaga-toolchain-v0.4.0-*.tar.xz

Writes two archives:
  enaga-toolchain-<tag>-linux-x86_64.tar.xz   tic6x/: what building an app needs. Host programs stripped, target
                                              libraries without debug information; LTO (lto1, lto-dump), gcov and
                                              the manuals left out, since Enaga's build uses none of them.
  enaga-toolchain-<tag>-source.tar            the corresponding source: GCC, binutils, GMP, MPFR, MPC and ISL as
                                              released upstream (checked against the hashes below), Enaga's patch
                                              and the two build scripts. GCC and binutils are GPLv3, and
                                              publishing their binaries means publishing this with them.

Source tarballs come from the build directories when they are there, otherwise from the GNU and GCC servers.
"""
import hashlib
import io
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import paths  # noqa: E402

TOOLS = paths.ROOT / "tools"
GCC_DIR = TOOLS / "gcc-build" / "gcc-13.3.0"
INFRA = "https://gcc.gnu.org/pub/gcc/infrastructure/"
SOURCES = [   # (file, where a build leaves it, where to fetch it, SHA-256)
    ("binutils-2.42.tar.xz", TOOLS / "binutils-build", "https://ftp.gnu.org/gnu/binutils/",
     "f6e4d41fd5fc778b06b7891457b3620da5ecea1006c6a4a41ae998109f85a800"),
    ("gcc-13.3.0.tar.xz", TOOLS / "gcc-build", "https://ftp.gnu.org/gnu/gcc/gcc-13.3.0/",
     "0845e9621c9543a13f484e94584a49ffc0129970e9914624235fc1d061a0c083"),
    ("gmp-6.2.1.tar.bz2", GCC_DIR, INFRA, "eae9326beb4158c386e39a356818031bd28f3124cf915f8c5b1dc4c7a36b4d7c"),
    ("mpfr-4.1.0.tar.bz2", GCC_DIR, INFRA, "feced2d430dd5a97805fa289fed3fc8ff2b094c02d05287fd6133e7f1f0ec926"),
    ("mpc-1.2.1.tar.gz", GCC_DIR, INFRA, "17503d2c395dfcf106b622dc142683c1199431d095367c6aacba6eec30340459"),
    ("isl-0.24.tar.bz2", GCC_DIR, INFRA, "fcf78dd9656c10eb8cf9fbd5f59a0b6b01386205fe1934b3b287a0a1898145c0"),
]
LEFT_OUT = ["share", "bin/tic6x-elf-lto-dump", "bin/tic6x-elf-gcov", "bin/tic6x-elf-gcov-dump",
            "bin/tic6x-elf-gcov-tool", "libexec/gcc/tic6x-elf/13.3.0/lto1",
            "libexec/gcc/tic6x-elf/13.3.0/install-tools"]
SOURCE_README = """Sources of the Enaga tic6x toolchain (enaga-toolchain-*-linux-x86_64.tar.xz).

GCC 13.3.0 and binutils 2.42 are released under the GNU GPL version 3; GMP, MPFR and MPC under the GNU LGPL
version 3; ISL under the MIT licence. Each tarball here is the upstream release, unchanged; their licences are
inside them.

The toolchain was built by build_tic6x_binutils.sh, then build_tic6x_gcc.sh (both here), on Ubuntu 24.04 x86-64,
with one change to binutils: binutils-2.42-tic6x-compact-rs.patch. To rebuild it, put the scripts in a folder
named tools/ with the patch in tools/patches/ and run them; they unpack these tarballs when they find them in
tools/binutils-build/ and tools/gcc-build/ (GMP, MPFR, MPC and ISL in tools/gcc-build/gcc-13.3.0/).
"""


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source(name, local_dir, url, digest, work):
    path = local_dir / name
    if not path.exists():
        path = work / name
        print(f"fetching {url}{name}")
        with urllib.request.urlopen(url + name) as response, open(path, "wb") as out:
            shutil.copyfileobj(response, out)
    if sha256(path) != digest:
        raise RuntimeError(f"{path}: SHA-256 does not match the release")
    return path


def is_elf(path):
    with open(path, "rb") as f:
        return f.read(4) == b"\x7fELF"


def stage_toolchain(dest):
    shutil.copytree(paths.TOOLCHAIN, dest, symlinks=True)
    for rel in LEFT_OUT:
        target = dest / rel
        if target.is_dir():
            shutil.rmtree(target)
        elif target.exists():
            target.unlink()
    for path in sorted(dest.rglob("*")):
        if path.is_symlink() or not path.is_file():
            continue
        if path.suffix in (".a", ".o"):                       # target code: tic6x objects
            subprocess.run([str(paths.tool("strip")), "--strip-debug", str(path)], check=True)
        elif is_elf(path):                                     # host programs
            subprocess.run(["strip", "--strip-unneeded", str(path)], check=True)


def add_bytes(tar, name, data, mode=0o644):
    info = tarfile.TarInfo(name)
    info.size, info.mode = len(data), mode
    tar.addfile(info, io.BytesIO(data))


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    tag = argv[0]
    if not paths.toolchain_present():
        print(f"no toolchain in {paths.TOOLCHAIN}: run tools/build_tic6x_binutils.sh and tools/build_tic6x_gcc.sh",
              file=sys.stderr)
        return 2
    dist = paths.BUILD / "dist"
    dist.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        stage_toolchain(work / "tic6x")
        binary = dist / f"enaga-toolchain-{tag}-linux-x86_64.tar.xz"
        with tarfile.open(binary, "w:xz") as tar:
            tar.add(work / "tic6x", "tic6x")
        sources = dist / f"enaga-toolchain-{tag}-source.tar"
        top = f"enaga-toolchain-{tag}-source"
        with tarfile.open(sources, "w") as tar:
            add_bytes(tar, f"{top}/README.txt", SOURCE_README.encode())
            for name, local_dir, url, digest in SOURCES:
                tar.add(source(name, local_dir, url, digest, work), f"{top}/{name}")
            for script in ("build_tic6x_binutils.sh", "build_tic6x_gcc.sh"):
                tar.add(TOOLS / script, f"{top}/{script}")
            for patch in sorted((TOOLS / "patches").glob("*.patch")):
                tar.add(patch, f"{top}/{patch.name}")
    for archive in (binary, sources):
        print(f"wrote {archive}: {archive.stat().st_size / 1e6:.1f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
