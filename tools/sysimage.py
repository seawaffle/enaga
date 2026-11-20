"""Wrap the Enaga image as a SYSTEM update file for one of the family's devices, which its bootloader installs.

    data = sysimage.wrap((0, 1, 0), entry=0xC0000000, sections=[(0xC0000000, code_bytes)], device="smpltrek")

The update is a `cmtd` container holding one `mmtd` (SYSTEM) image (docs/hardware.md, "Installing"):

    cmtd: magic, total size, CRC, 0, family word (DEVICES), descriptor count 1     (24 bytes)
          descriptor: kind 3 (SYSTEM), version triplet, image offset 48, image size (24 bytes)
    mmtd: magic, size, CRC, 0xFFFFFFFF (install marker), version triplet, entry, section count,
          then per section: "sect", load address, length, bytes

Each CRC is IEEE CRC-32 over its whole block with its own CRC field zeroed, started from a per-product seed
instead of 0xFFFFFFFF. The image's CRC is sealed first, because the container's CRC covers it. The image is
padded to at least 4 KiB (the updater checks the first 4 KiB of flash as one block) and may not exceed the
4 MiB SYSTEM flash area.
"""
import struct
import zlib

# The SmplTrek, Lofi-12 XT and CyDrums are one board (smpltrek-re README). Their update files differ only in the
# family word, the two CRC seeds and the file name the bootloader looks for at the card's root. Family words and
# seeds from the smpltrek-re project (its tools/palette_swap.py): validated against 8 SmplTrek, 3 Lofi-12 XT and 2
# CyDrums official releases. Only the SmplTrek has installed a file made this way.
DEVICES = {
    "smpltrek": {"label": "SmplTrek", "family": 5, "cmtd": 0x6D2DCD30, "mmtd": 0x1FC67D2F, "card_name": "SmplTrek.bin"},
    "lofi12xt": {"label": "Lofi-12 XT", "family": 12, "cmtd": 0x8061B03E, "mmtd": 0x2929E2B7,
                 "card_name": "Lofi-12 XT.bin"},
    "cydrums": {"label": "CyDrums", "family": 8, "cmtd": 0x9763BE13, "mmtd": 0x2C577CBB, "card_name": "CyDrums.bin"},
}
MIN_IMAGE = 4096
MAX_IMAGE = 0x400000


def _crc(block, seed):
    """CRC-32 of `block` with bytes 8..11 (its CRC field) taken as zero, from `seed`."""
    crc = zlib.crc32(block[:8], seed)
    crc = zlib.crc32(b"\0" * 4, crc)
    return zlib.crc32(block[12:], crc)


def wrap(version, entry, sections, device="smpltrek", pad_to=MIN_IMAGE):
    """Return the update file bytes for `sections` [(load address, bytes), ...] entered at `entry`."""
    d = DEVICES[device]
    body = b""
    for address, data in sections:
        body += b"sect" + struct.pack("<II", address, len(data)) + bytes(data)
    size = 36 + len(body)
    if size > MAX_IMAGE:
        raise ValueError(f"system image {size:#x} bytes exceeds the 4 MiB SYSTEM flash area")
    if size < pad_to:
        # Pad the last section with zero words (C6000 NOPs) so the image reaches the minimum.
        address, data = sections[-1]
        sections = list(sections[:-1]) + [(address, bytes(data) + bytes(pad_to - size))]
        return wrap(version, entry, sections, device, pad_to=0)
    mmtd = bytearray(b"mmtd" + struct.pack("<II", size, 0) + b"\xff\xff\xff\xff" + struct.pack("<3I", *version)
                     + struct.pack("<II", entry, len(sections)) + body)
    struct.pack_into("<I", mmtd, 8, _crc(bytes(mmtd), d["mmtd"]))
    total = 48 + size
    cmtd = bytearray(b"cmtd" + struct.pack("<IIIII", total, 0, 0, d["family"], 1)
                     + struct.pack("<6I", 3, *version, 48, size) + mmtd)
    struct.pack_into("<I", cmtd, 8, _crc(bytes(cmtd), d["cmtd"]))
    return bytes(cmtd)


def verify(data, device="smpltrek"):
    """True when an update made by wrap() is for `device` and both its CRCs are correct."""
    d = DEVICES[device]
    size = struct.unpack_from("<I", data, 52)[0]
    mmtd = data[48:48 + size]
    return (len(data) == 48 + size and data[:4] == b"cmtd" and mmtd[:4] == b"mmtd"
            and struct.unpack_from("<I", data, 16)[0] == d["family"]
            and struct.unpack_from("<I", data, 8)[0] == _crc(data, d["cmtd"])
            and struct.unpack_from("<I", mmtd, 8)[0] == _crc(mmtd, d["mmtd"]))
