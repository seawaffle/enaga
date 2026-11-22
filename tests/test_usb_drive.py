"""USB DRIVE (E5 step 3) on the emulator's USB host: the SD card as a USB mass storage drive (bulk-only transport,
SCSI) beside USB MIDI. No medium until the menu's USB DRIVE page shares it; opening the page stops the app (its stop()
saves a file); the computer reads and writes sectors; ejecting ends it, the card is mounted again and the menu lists
an app copied over. OK and unplugging end it too.

The host side here writes a file the way a computer does: the file is added to a copy of the card image offline
(tools/card.py), and the sectors that changed are written over USB.

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest tests.test_usb_drive
"""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ENAGA = HERE.parent
sys.path.insert(0, str(ENAGA / "tools"))

import build_app  # noqa: E402
import card  # noqa: E402
import paths  # noqa: E402

BOOT = 300_000_000
MS = 372_000
F1, OK, DOWN = 19, 23, 25
EP_OUT, EP_IN = 0x02, 0x82
SECTOR = 512


@unittest.skipUnless(paths.emulator_present() and build_app.tools_present(), "needs ENAGA_EMULATOR and the toolchain")
class UsbDrive(unittest.TestCase):
    def setUp(self):
        from emulate import Emulation
        self.emu = Emulation(HERE / "apps" / "battery", "BATTERY")
        self.addCleanup(self.emu.close)
        self.assertIsNone(self.emu.run(BOOT)["error"])
        self.emu.cpu.gpio_input(3, 0, True)            # POWER up (active low)
        self.emu.cpu.usb_attach()
        self.emu.cpu.usb_enumerate()
        self.tag = 0

    def run_ms(self, ms):
        self.assertIsNone(self.emu.run(int(ms * MS))["error"])

    def press(self, key):
        self.emu.key(key, True)
        self.run_ms(30)
        self.emu.key(key, False)
        self.run_ms(30)

    def open_drive(self):                              # F1 + POWER, then the USB DRIVE line (app, SETTINGS, USB DRIVE)
        self.emu.key(F1, True)
        self.run_ms(30)
        self.emu.cpu.gpio_input(3, 0, False)
        self.run_ms(60)
        self.emu.cpu.gpio_input(3, 0, True)
        self.emu.key(F1, False)
        self.run_ms(30)
        self.press(DOWN)
        self.press(DOWN)
        self.press(OK)

    # ---- bulk-only transport: CBW, data, CSW ----

    def scsi(self, command, data_in=0, data_out=b""):
        """(status, data): status 0 passed, 1 failed (from the CSW)."""
        self.tag += 1
        length = data_in or len(data_out)
        cbw = struct.pack("<IIIBBB", 0x43425355, self.tag, length, 0x80 if data_in else 0, 0, len(command))
        self.assertEqual(self.emu.cpu.usb_out(EP_OUT, cbw + bytes(command).ljust(16, b"\0"))["status"], "ok")
        for i in range(0, len(data_out), 4096):
            self.assertEqual(self.emu.cpu.usb_out(EP_OUT, data_out[i:i + 4096])["status"], "ok")
        data = b""
        while len(data) < data_in:
            r = self.emu.cpu.usb_in(EP_IN, min(4096, data_in - len(data)), timeout_ms=200)
            if r["status"] != "ok":
                break                                  # a stall: the CSW says why
            data += bytes(r["data"])
        r = self.emu.cpu.usb_in(EP_IN, 13, timeout_ms=200)
        if r["status"] != "ok":                        # the IN endpoint was stalled: clear it, then the CSW
            self.emu.cpu.usb_control((0x02, 1, 0, EP_IN), 0)
            r = self.emu.cpu.usb_in(EP_IN, 13, timeout_ms=200)
        csw = bytes(r["data"])
        signature, tag, _, status = struct.unpack("<IIIB", csw)
        self.assertEqual((signature, tag), (0x53425355, self.tag))
        return status, data

    def sense(self):                                   # (key, ASC)
        status, data = self.scsi([0x03, 0, 0, 0, 18, 0], 18)
        self.assertEqual(status, 0)
        return data[2] & 15, data[12]

    def ready(self):
        status, _ = self.scsi([0x00] * 6)
        return status == 0 or self.sense()

    def read(self, lba, count):
        status, data = self.scsi([0x28, 0, *lba.to_bytes(4, "big"), 0, *count.to_bytes(2, "big"), 0], count * SECTOR)
        self.assertEqual(status, 0)
        return data

    def write(self, lba, data):
        count = len(data) // SECTOR
        status, _ = self.scsi([0x2A, 0, *lba.to_bytes(4, "big"), 0, *count.to_bytes(2, "big"), 0], data_out=data)
        self.assertEqual(status, 0)

    def card_now(self, folder):
        image = Path(folder) / "now.img"
        image.write_bytes(self.emu.cpu.sd_read())
        return image

    # ---- the tests ----

    def test_no_medium_until_shared(self):
        status, data = self.scsi([0x12, 0, 0, 0, 36, 0], 36)                   # INQUIRY
        self.assertEqual(status, 0)
        self.assertEqual((data[0], data[1] & 0x80), (0, 0x80))                  # a removable disk
        self.assertEqual(data[8:16], b"Enaga   ")
        self.assertEqual(self.ready(), (2, 0x3A))                               # NOT READY, MEDIUM NOT PRESENT
        self.assertEqual(self.scsi([0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0], SECTOR)[0], 1)   # reads fail
        self.assertEqual(self.emu.report()["app_running"], True)               # the app runs on

    def test_share_write_a_file_and_eject(self):
        self.open_drive()
        self.assertEqual(self.emu.word("menu_drive"), 1)
        self.assertFalse(self.emu.report()["app_running"])
        self.assertIn("USB DRIVE ON", self.emu.report()["log"])
        self.assertEqual(self.ready(), (6, 0x28))                               # UNIT ATTENTION: medium changed
        self.assertIs(self.ready(), True)
        status, data = self.scsi([0x25] + [0] * 9, 8)                           # READ CAPACITY (10)
        self.assertEqual(status, 0)
        size = len(self.emu.cpu.sd_read())
        self.assertEqual(struct.unpack(">II", data), (size // SECTOR - 1, SECTOR))
        with tempfile.TemporaryDirectory() as d:
            before = self.card_now(d)
            self.assertEqual(card.get(before, "/ENAGA/DATA/BATTERY/STOP.TXT"), b"STOPPED")   # stop() ran
            original = before.read_bytes()
            self.assertEqual(self.read(0, 16), original[:16 * SECTOR])          # what the computer reads is the card
            # The computer copies an app over: the sectors a FAT driver would change, written over USB.
            package = build_app.build(HERE / "apps" / "clock", paths.BUILD / "apps" / "clock", "CLOCK")["package"]
            after = Path(d) / "after.img"
            after.write_bytes(original)
            card.put(after, "/ENAGA/APPS/CLOCK/APP.ENA", package)
            new = after.read_bytes()
            changed = [i for i in range(size // SECTOR) if new[i * SECTOR:(i + 1) * SECTOR] != original[i * SECTOR:(i + 1) * SECTOR]]
            self.assertTrue(changed)
            runs, start = [], changed[0]
            for a, b in zip(changed, changed[1:] + [None]):
                if b != a + 1 or a + 1 - start == 16:
                    runs.append((start, a + 1))
                    start = b
            for first, end in runs:
                self.write(first, new[first * SECTOR:end * SECTOR])
            self.assertEqual(self.read(changed[0], 1), new[changed[0] * SECTOR:(changed[0] + 1) * SECTOR])
            self.assertEqual(self.emu.word("usb_drive_written_sectors"), len(changed))
            self.assertEqual(self.emu.word("usb_drive_errors"), 0)
            self.assertEqual(self.scsi([0x1B, 0, 0, 0, 0x02, 0])[0], 0)        # START STOP UNIT: eject
            self.run_ms(100)
            self.assertEqual(self.emu.word("menu_drive"), 0)
            self.assertEqual(self.emu.text("menu_message", 40), "USB DRIVE EJECTED")
            self.assertEqual(self.ready(), (2, 0x3A))                           # gone from the computer
            raw = self.emu.cpu.read(self.emu.names["menu_titles"], 2 * 24)
            titles = [raw[i * 24:(i + 1) * 24].split(b"\0")[0].decode() for i in range(2)]
            self.assertIn("CLOCK", titles)                                      # mounted again and rescanned
            self.assertEqual(card.get(self.card_now(d), "/ENAGA/APPS/CLOCK/APP.ENA"), package)

    def test_ok_ends_it(self):
        self.open_drive()
        self.assertEqual(self.emu.word("menu_drive"), 1)
        self.press(OK)
        self.assertEqual(self.emu.word("menu_drive"), 0)
        self.assertEqual(self.emu.text("menu_message", 40), "USB DRIVE ENDED")
        self.assertEqual(self.ready(), (2, 0x3A))

    def test_unplugging_ends_it(self):
        self.open_drive()
        self.assertIs(self.ready() == (6, 0x28) and self.ready(), True)
        self.emu.cpu.usb_detach()
        self.run_ms(100)
        self.assertEqual(self.emu.word("menu_drive"), 0)
        self.assertEqual(self.emu.text("menu_message", 40), "USB UNPLUGGED")


if __name__ == "__main__":
    unittest.main()
