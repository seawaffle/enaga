#!/usr/bin/env python3
"""Capture Enaga's peripheral register accesses phase by phase on the emulator, for diffing against the stock's.

The emulator's capture (cpu.capture_start, cpu.codec_log, cpu.sd_log) records every access to USB0, SYSCFG0, the
PSCs, I2C0, McASP0, the EDMA3 controller and MMCSD0, plus the codec's register writes and the SD card's commands;
where the emulator has cpu.psc_violations, each phase also lists the accesses to a module its PSC has not enabled
(tests/test_usb.py expects none).
The ranges, the phase names and the raw format are those of the emulator's stock_register_capture.py, so the
stock tables (smpltrek-re docs/stock-register-writes) and Enaga's line up phase by phase:

  main run (tests/apps/input)
    boot           reset to the app running (300 M cycles)
    attach         cable in, to the start of the host's bus reset
    bus_reset      the reset and its recovery
    enumeration    usb_enumerate()
    bulk_out       a USB MIDI note on and off on endpoint 0x01
    interface_on   SET_INTERFACE 2 alt 1, then 3 alt 1 (no packets)
    streaming      20 ms of packets both ways (the stock run has no such phase)
    interface_off  SET_INTERFACE 2 alt 0, then 3 alt 0
    suspend        start-of-frame off until the device suspends, then on again
    detach         cable out
    input_N        the input app's STEP N: the input routes (1-8; 10 is USB)
    power_off      POWER held, as switching off
  drive run (tests/apps/battery)
    boot, attach, bus_reset, enumeration   as above
    drive_open     F1 + POWER, USB DRIVE
    drive_io       INQUIRY, a 128-sector READ(10), a 1-sector WRITE(10)
    drive_close    START STOP UNIT (eject), to the card remounted and the app back

    python3 tools/register_capture.py [--output build/register-capture]

Writes raw.json (a list of the two runs) and symbols.json (Enaga's firmware symbols, name to address). The
emulator's stock_register_tables.py --raw raw.json --all-runs --symbols symbols.json --output ... turns them into
tables, both runs' phases each under its own name and the PCs named by Enaga's functions.
"""
import argparse
import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import paths  # noqa: E402

ENAGA = HERE.parent
MS = 372_000
BOOT = 300_000_000
STEP1, F1, OK, DOWN = 32, 19, 23, 25
RANGES = [(0x01E00000, 0x01E10000),            # USB0: core, wrapper, CPPI
          (0x01C14000, 0x01C14200),            # SYSCFG0: KICK0/1, PINMUX0-19, CFGCHIP0-4
          (0x01C10000, 0x01C11000),            # PSC0
          (0x01E27000, 0x01E28000),            # PSC1
          (0x01C22000, 0x01C22060),            # I2C0
          (0x01D00000, 0x01D00300),            # McASP0 registers (not the DMA data port)
          (0x01C00000, 0x01C08000),            # EDMA3 CC0 registers and PaRAM
          (0x01C40000, 0x01C40078)]            # MMCSD0


def capture(scenario):
    from emulate import Emulation
    app = "input" if scenario == "main" else "battery"
    result = dict(scenario=scenario, project=f"tests/apps/{app}", phases=[], fault=None)
    with Emulation(ENAGA / "tests" / "apps" / app, app.upper()) as emu:
        cpu = emu.cpu
        since = dict(codec=0, sd=0, psc=0)
        psc_log = hasattr(cpu, "psc_violations")
        cpu.capture_start(RANGES)
        current = ["boot"]

        def advance(ms):
            report = emu.run(int(ms * MS))
            if report["error"]:
                raise RuntimeError(report["error"])

        def press(key, ms=30):
            emu.key(key, True); advance(ms)
            emu.key(key, False); advance(ms)

        def until_port(state, limit_ms=500):
            for _ in range(limit_ms):
                if cpu.usb_state()["port"] == state:
                    return
                advance(1)
            raise RuntimeError(f"the USB port never reached {state}")

        def mark(name, **extra):
            mmio = cpu.capture_take()
            codec, since["codec"] = cpu.codec_log(since["codec"])
            sd, since["sd"] = cpu.sd_log(since["sd"])
            if psc_log:
                extra["psc_violations"], since["psc"] = cpu.psc_violations(since["psc"])
            result["phases"].append(dict(name=name, end=cpu.cycles(), mmio=mmio, codec=codec, sd=sd, **extra))
            current[0] = None

        def begin(name):
            current[0] = name

        def scsi(tag, command, data_in=0, data_out=b""):
            length = data_in or len(data_out)
            cbw = struct.pack("<IIIBBB", 0x43425355, tag, length, 0x80 if data_in else 0, 0, len(command))
            cpu.usb_out(0x02, cbw + bytes(command).ljust(16, b"\0"))
            if data_out:
                cpu.usb_out(0x02, data_out)
            got = 0
            while got < data_in:
                r = cpu.usb_in(0x82, min(4096, data_in - got), timeout_ms=200)
                if r["status"] != "ok":
                    break
                got += len(r["data"])
            csw = cpu.usb_in(0x82, 13, timeout_ms=200)
            return csw["status"], bytes(csw["data"])[12:13].hex()

        try:
            advance(BOOT // MS)
            if scenario == "drive":
                cpu.gpio_input(3, 0, True)                       # POWER up (active low)
            mark("boot")
            begin("attach"); cpu.usb_attach(); until_port("reset"); mark("attach")
            begin("bus_reset"); until_port("enabled", 100); mark("bus_reset")
            begin("enumeration")
            found = cpu.usb_enumerate()
            advance(10)
            mark("enumeration", steps=[dict(step=s["step"], status=s["status"], submitted=s["submitted"],
                                            completed=s["completed"]) for s in found["steps"]])
            if scenario == "drive":
                begin("drive_open")
                emu.key(F1, True); advance(30)
                cpu.gpio_input(3, 0, False); advance(60)
                cpu.gpio_input(3, 0, True)
                emu.key(F1, False); advance(30)
                press(DOWN); press(DOWN); press(OK)
                advance(200)
                mark("drive_open")
                begin("drive_io")
                replies = [scsi(1, [0x12, 0, 0, 0, 36, 0], 36),
                           scsi(2, [0x28, 0, 0, 0, 0, 0, 0, 0, 128, 0], 128 * 512),
                           scsi(3, [0x2A, 0, 0, 0, 0x10, 0, 0, 0, 1, 0], data_out=bytes(512))]
                advance(50)
                mark("drive_io", replies=replies)
                begin("drive_close")
                replies = [scsi(4, [0x1B, 0, 0, 0, 0x02, 0])]
                advance(3000)
                mark("drive_close", replies=replies, log=emu.report()["log"][-6:])
                return result
            begin("bulk_out")
            sent = [cpu.usb_out(0x01, bytes([0x09, 0x90, 36, 110]))["status"]]
            advance(300)
            sent.append(cpu.usb_out(0x01, bytes([0x08, 0x80, 36, 0]))["status"])
            advance(50)
            mark("bulk_out", transfers=sent)
            begin("interface_on")
            replies = [cpu.usb_set_interface(2, 1)["status"], cpu.usb_set_interface(3, 1)["status"]]
            advance(20)
            mark("interface_on", replies=replies)
            begin("streaming")
            cpu.usb_iso_stream_out(0x03, [bytes(192)] * 20)
            cpu.usb_iso_stream_in(0x83, 20, wait=False)
            advance(25)
            cpu.usb_iso_collect(0x83)
            mark("streaming")
            begin("interface_off")
            replies = [cpu.usb_set_interface(2, 0)["status"], cpu.usb_set_interface(3, 0)["status"]]
            advance(20)
            mark("interface_off", replies=replies)
            begin("suspend")
            cpu.usb_sof(False); advance(10)
            suspended = cpu.usb_state()["suspended"]
            cpu.usb_sof(True); advance(10)
            mark("suspend", suspended=suspended, resumed=not cpu.usb_state()["suspended"])
            begin("detach"); cpu.usb_detach(); advance(50); mark("detach")
            for step in (1, 2, 3, 4, 5, 6, 7, 8, 10):
                begin(f"input_{step}")
                press(STEP1 + step - 1, 20)
                advance(30)
                mark(f"input_{step}", log=emu.report()["log"][-1])
            begin("power_off")
            emu.power_off()
            mark("power_off")
        except Exception as error:   # keep what was captured; the fault is reported with the phase it ended
            result["fault"] = f"{type(error).__name__}: {error}"
            try:
                mark(current[0] or "after_" + result["phases"][-1]["name"], fault=result["fault"])
            except Exception:
                pass
        result["cycles"] = cpu.cycles()
        result["symbols"] = {name: address for name, address in emu.names.items()}
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, default=paths.BUILD / "register-capture")
    args = parser.parse_args(argv)
    paths.use_emulator()
    args.output.mkdir(parents=True, exist_ok=True)
    runs = [capture(scenario) for scenario in ("main", "drive")]
    symbols = runs[0].pop("symbols", {})
    runs[1].pop("symbols", None)
    (args.output / "raw.json").write_text(json.dumps(runs) + "\n")
    (args.output / "symbols.json").write_text(json.dumps(symbols) + "\n")
    for result in runs:
        print(f'== {result["scenario"]}' + (f'  FAULT {result["fault"]}' if result["fault"] else ""))
        for phase in result["phases"]:
            extra = {k: v for k, v in phase.items() if k not in ("name", "end", "mmio", "codec", "sd", "psc_violations")}
            psc = f'  psc {len(phase["psc_violations"])}' if "psc_violations" in phase else ""
            print(f'  {phase["name"]:14} mmio {len(phase["mmio"]):6}  codec {len(phase["codec"]):4}  '
                  f'sd {len(phase["sd"]):5}{psc}  {extra if extra else ""}')
    return 1 if any(r["fault"] for r in runs) else 0


if __name__ == "__main__":
    sys.exit(main())
