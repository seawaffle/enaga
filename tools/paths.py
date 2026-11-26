"""Where Enaga's tools find things. Every script imports this instead of computing paths itself.

    ROOT       the repository
    TOOLCHAIN  GCC/binutils for tic6x-elf: $ENAGA_TOOLCHAIN, or tools/tic6x (tools/build_tic6x_*.sh)
    BUILD      build output: $ENAGA_BUILD, or build/ (ignored by git)
    EMULATOR   the SmplTrek emulator directory, for tests and tools/emulate.py: $ENAGA_EMULATOR (optional)
    TI_CGT     TI's C6000 code generation tools, for apps' *.ti.c files: $ENAGA_TI_CGT, or the newest
               ~/ti/ti-cgt-c6000_* (optional; ENAGA_TI_CGT=none builds those files with GCC instead, and the
               DSP library too, never taking the SDK's prebuilt one: TI_DISABLED)
"""
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLCHAIN = Path(os.environ.get("ENAGA_TOOLCHAIN", ROOT / "tools" / "tic6x"))
BIN = TOOLCHAIN / "bin"
LIBGCC_DIR = TOOLCHAIN / "lib" / "gcc" / "tic6x-elf"
BUILD = Path(os.environ.get("ENAGA_BUILD", ROOT / "build"))
EMULATOR = Path(os.environ["ENAGA_EMULATOR"]) if os.environ.get("ENAGA_EMULATOR") else None


def _version(path):
    return [int(n) for n in re.findall(r"\d+", path.name)]


def _ti_cgt():
    chosen = os.environ.get("ENAGA_TI_CGT")
    if chosen:
        return None if chosen == "none" else Path(chosen)
    found = sorted(Path.home().glob("ti/ti-cgt-c6000_*"), key=_version)
    return found[-1] if found else None


TI_CGT = _ti_cgt()
TI_DISABLED = os.environ.get("ENAGA_TI_CGT") == "none"


def tool(name):
    return BIN / f"tic6x-elf-{name}"


def libgcc():
    found = sorted(LIBGCC_DIR.glob("*/c674x/libgcc.a"))
    if not found:
        raise RuntimeError(f"libgcc for c674x not found under {LIBGCC_DIR}; run tools/build_tic6x_gcc.sh")
    return found[-1]


def toolchain_present():
    return tool("g++").exists() and tool("ld").exists()


def ti_present():
    return TI_CGT is not None and (TI_CGT / "bin" / "cl6x").exists()


def emulator_present():
    return EMULATOR is not None and (EMULATOR / "cpu.py").exists()


def use_emulator():
    """Make the emulator's modules (cpu, media, display, audio_capture) importable, or explain what is missing."""
    if not emulator_present():
        raise RuntimeError("set ENAGA_EMULATOR to the SmplTrek emulator directory (the one containing cpu.py)")
    if str(EMULATOR) not in sys.path:
        sys.path.append(str(EMULATOR))
