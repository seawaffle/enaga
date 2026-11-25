#!/usr/bin/env python3
"""Talk to a unit's debug port over USB: Enaga's log as it happens, its state, the screen, and words of memory,
instead of photographing the STATUS page. The debug port is the unit's second USB MIDI port ("Enaga Debug"; on
Windows' older MIDI stack "MIDIIN2 (Enaga SmplTrek)" and "MIDIOUT2 (...)"); the protocol is in
firmware/usb/usb_debug.c.

    python3 -B tools/usb_debug.py log                     # the log, until Ctrl-C (--out FILE keeps a copy)
    python3 -B tools/usb_debug.py status                  # NAME VALUE lines: version, app, audio, USB, SD ...
    python3 -B tools/usb_debug.py screen shot.png         # the screen as shown last
    python3 -B tools/usb_debug.py peek 0xC0001000 4       # words of RAM, or USB0 wrapper / SYSCFG0 registers
    python3 -B tools/usb_debug.py peek usb_tx_lost --elf build/firmware/enaga.elf   # a firmware variable by name
    python3 -B tools/usb_debug.py press F1 POWER          # keys, held together then let go (POWER only with F1)
    python3 -B tools/usb_debug.py turn VALUE -3           # a knob (A-D, VALUE), by detents
    python3 -B tools/usb_debug.py pad 5 --velocity 90     # a pad pressed and let go
    python3 -B tools/usb_debug.py release                 # everything held from the computer let go
    python3 -B tools/usb_debug.py ports                   # the MIDI ports seen, to pick one with --port

Each input answers once a frame has been drawn after it, so a `screen` straight after shows its effect. Scripts
use Client (press, key, turn, pad, pressure, release, screen, status, log_lines ...) the same way. While input
from the computer is in use the unit shows a small square at the top right; pressing or turning anything on the
unit lets go of it, as does 5 s without any request (Client.wait keeps a hold going).

Needs python-rtmidi (pip install python-rtmidi), on Windows, macOS and Linux. The port is found by asking each
Enaga MIDI port in turn (debug-looking names first); --port NAME picks one (a part of its name). A HELLO that goes
to the MIDI port instead reaches the running app as a SysEx it will not recognise.
"""
import argparse
import struct
import sys
import time
import zlib
from pathlib import Path

HELLO, LOG, LOST, STATUS, SCREEN, PEEK, ERROR = 0, 1, 2, 3, 4, 5, 0x7F
KEY, TURN, PAD, PRESSURE, RELEASE = 6, 7, 8, 9, 10

# enaga_key (include/enaga/abi.h), and POWER as the debug port numbers it
KEYS = {"PUSH_B": 0, "PUSH_A": 1, "PUSH_C": 2, "PUSH_D": 3, "PUSH_VALUE": 4, "SELECT_LEFT": 8, "SELECT_RIGHT": 9,
        "OCTAVE_LEFT": 16, "UP": 17, "RIGHT": 18, "F1": 19, "F3": 20, "F7": 21, "F5": 22, "OK": 23, "LEFT": 24,
        "DOWN": 25, "F2": 26, "F4": 27, "F8": 28, "F6": 29, "CLR": 30, "OCTAVE_RIGHT": 31, "POWER": 48,
        **{f"STEP{n}": 31 + n for n in range(1, 17)}}
ENCODERS = {"A": 0, "B": 1, "C": 2, "D": 3, "VALUE": 4}
HEADER = bytes([0xF0, 0x7D, 0x45])


# ---- The protocol ----------------------------------------------------------------------------------------------

def request(command, data=b""):
    """A request as SysEx bytes."""
    return HEADER + bytes([command]) + bytes(data) + b"\xF7"


def number(value, count):
    """value in count bytes of 7 bits, lowest first."""
    return bytes((value >> (7 * i)) & 0x7F for i in range(count))


def unnumber(data):
    return sum(b << (7 * i) for i, b in enumerate(data))


def decode(sysex):
    """(command, data) of one of the unit's messages, or None if it is not one."""
    sysex = bytes(sysex)
    if len(sysex) < 5 or not sysex.startswith(HEADER) or sysex[-1] != 0xF7:
        return None
    return sysex[3], sysex[4:-1]


class Client:
    """The debug port's requests and answers over a transport with send(sysex bytes) and receive(timeout_s) ->
    a list of whole SysEx messages (each bytes) from the port. `clock` gives the time in seconds that timeouts
    count in (the emulator's, in its tests)."""

    def __init__(self, transport, clock=time.monotonic):
        self.transport = transport
        self.clock = clock
        self.waiting = []                      # messages received but not yet asked for

    def messages(self, timeout):
        """The next messages, waiting up to timeout seconds for the first."""
        if self.waiting:
            out, self.waiting = self.waiting, []
            return out
        return [m for m in (decode(s) for s in self.transport.receive(timeout)) if m]

    def ask(self, command, data=b""):
        self.transport.send(request(command, data))

    def answer(self, command, until, timeout=3.0):
        """Messages of `command` until until(messages) says the answer is whole. Raises on a 7F error answer,
        TimeoutError if it does not come."""
        got, others, end = [], [], self.clock() + timeout
        try:
            while self.clock() < end:
                for c, data in self.messages(0.05):     # others are kept aside, for later calls
                    if c == ERROR:
                        raise RuntimeError(f"the unit refused command {data[0]:02X}: "
                                           f"{data[1:].decode('ascii', 'replace')}")
                    if c == command:
                        got.append(data)
                    else:
                        others.append((c, data))
                if until(got):
                    return got
            raise TimeoutError(f"no complete answer to command {command:02X} ({len(got)} messages)")
        finally:
            self.waiting = others + self.waiting

    def hello(self, timeout=1.0):
        self.ask(HELLO)
        return self.answer(HELLO, lambda got: got, timeout)[0].decode("ascii", "replace")

    def status(self):
        self.ask(STATUS)
        lines = self.answer(STATUS, lambda got: got and got[-1] == b"")
        return [line.decode("ascii", "replace") for line in lines[:-1]]

    def screen(self):
        """128 rows of 128 greys (0-15)."""
        self.ask(SCREEN)
        rows = self.answer(SCREEN, lambda got: len(got) >= 128, timeout=10.0)
        frame = [None] * 128
        for row in rows:
            frame[row[0]] = list(row[1:129])
        return frame

    def peek(self, address, words=1):
        self.ask(PEEK, number(address, 5) + bytes([words]))
        data = self.answer(PEEK, lambda got: got)[0]
        return [unnumber(data[5 + 5 * i:10 + 5 * i]) for i in range(words)]

    # ---- Remote input: each call returns once the unit has drawn a frame after it ----

    def _input(self, command, data=b""):
        self.ask(command, data)
        self.answer(command, lambda got: got)

    def key(self, key, down):
        """A key (a KEYS name or its number) down or up."""
        self._input(KEY, bytes([key_number(key), 1 if down else 0]))

    def wait(self, seconds):
        """Wait, keeping what is held from here held: the unit lets go of remote input after 5 s without a
        request, so this asks HELLO each second. Messages that arrive meanwhile are kept for later calls."""
        end = self.clock() + seconds
        asked = self.clock()
        while self.clock() < end:
            got = self.transport.receive(min(end - self.clock(), 0.25))
            self.waiting += [m for m in (decode(s) for s in got) if m]
            if self.clock() - asked >= 1.0 and self.clock() < end:
                self.hello()
                asked = self.clock()

    def press(self, *keys, hold=0.0):
        """Keys pressed in order, held `hold` seconds (on the computer's clock), let go in reverse."""
        for k in keys:
            self.key(k, True)
        if hold:
            self.wait(hold)
        for k in reversed(keys):
            self.key(k, False)

    def turn(self, encoder, detents):
        """A knob (A-D, VALUE, or 0-4) turned by detents (-8192 to 8191)."""
        encoder = ENCODERS[encoder.upper()] if isinstance(encoder, str) else encoder
        self._input(TURN, bytes([encoder]) + number(detents & 0x3FFF, 2))

    def pad(self, pad, velocity=100):
        """Pad 0-14 pressed with velocity 1-127, or let go with 0."""
        self._input(PAD, bytes([pad, velocity]))

    def pressure(self, pad, pressure):
        """Pressure 0-127 on a pad held with pad()."""
        self._input(PRESSURE, bytes([pad, pressure]))

    def release(self):
        """Every key and pad held from here let go; the unit's mark goes."""
        self._input(RELEASE)

    def log(self, on=True):
        self.ask(1 if on else 2)

    def log_lines(self, timeout):
        """(sequence, text) for each log line that arrived; (None, "... n lines lost") for gaps."""
        out, others = [], []
        for c, data in self.messages(timeout):
            if c == LOG:
                out.append((unnumber(data[:3]), data[3:].decode("ascii", "replace")))
            elif c == LOST:
                out.append((None, f"... {unnumber(data[:3])} lines lost"))
            else:
                others.append((c, data))
        self.waiting = others + self.waiting
        return out


def key_number(key):
    if isinstance(key, int):
        return key
    name = key.upper()
    if name.isdigit():
        return int(name)
    if name not in KEYS:
        raise KeyError(f"{key}: not a key ({', '.join(KEYS)})")
    return KEYS[name]


# ---- USB MIDI event packets (for transports that see the raw stream: the emulator's) ---------------------------

def packets(cable, sysex):
    """SysEx bytes as USB MIDI event packets on a cable."""
    out = b""
    for i in range(0, len(sysex), 3):
        chunk = sysex[i:i + 3]
        cin = 4 + len(chunk) if chunk[-1] == 0xF7 else 4
        out += bytes([cable << 4 | cin]) + chunk + bytes(3 - len(chunk))
    return out


class Unpacker:
    """USB MIDI event packets in, whole SysEx messages per cable out."""
    SIZES = {4: 3, 5: 1, 6: 2, 7: 3}

    def __init__(self):
        self.partial = {}

    def feed(self, data):
        """{cable: [sysex, ...]} completed by these packets."""
        done = {}
        for i in range(0, len(data) - 3, 4):
            cable, cin = data[i] >> 4, data[i] & 15
            if cin not in self.SIZES:
                continue
            buffer = self.partial.setdefault(cable, bytearray())
            if data[i + 1] == 0xF0:            # a new SysEx: anything unfinished before it is dropped
                buffer.clear()
            buffer += data[i + 1:i + 1 + self.SIZES[cin]]
            if cin != 4:
                done.setdefault(cable, []).append(bytes(buffer))
                buffer.clear()
        return done


# ---- PNG and ELF, without other modules -------------------------------------------------------------------------

def png(frame):
    """A frame of greys 0-15 as an 8-bit greyscale PNG, each pixel 4 x 4."""
    rows = b"".join(b"\0" + bytes(17 * g for g in row for _ in range(4)) for row in frame for _ in range(4))
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    header = struct.pack(">IIBBBBB", 4 * len(frame[0]), 4 * len(frame), 8, 0, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")


def elf_symbol(path, name):
    """The address of a symbol in a 32-bit little-endian ELF (the firmware's enaga.elf)."""
    data = Path(path).read_bytes()
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
    sections = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    for s in sections:
        if s[1] != 2:                          # SHT_SYMTAB
            continue
        strings = sections[s[6]]
        for at in range(s[4], s[4] + s[5], 16):
            name_at, value = struct.unpack_from("<II", data, at)
            end = data.index(b"\0", strings[4] + name_at)
            if data[strings[4] + name_at:end].decode() == name:
                return value
    raise KeyError(f"{name} is not in {path}")


# ---- The MIDI ports (python-rtmidi) ------------------------------------------------------------------------------

class RtMidiTransport:
    def __init__(self, midi_in, midi_out):
        self.midi_in, self.midi_out = midi_in, midi_out
        midi_in.ignore_types(sysex=False, timing=True, active_sense=True)
        self.buffer = bytearray()

    def send(self, sysex):
        try:
            self.midi_out.send_message(list(sysex))
        except Exception as e:                 # rtmidi's errors: the port went away (unplugged)
            raise ConnectionError(f"the debug port went away ({e})") from e

    def receive(self, timeout):
        out, end = [], time.monotonic() + timeout
        while True:
            message = self.midi_in.get_message()
            if message:
                data = bytes(message[0])
                if data[:1] == b"\xF0":
                    self.buffer = bytearray(data)
                else:
                    self.buffer += data         # some systems deliver a long SysEx in parts
                if self.buffer[:1] == b"\xF0" and self.buffer[-1:] == b"\xF7":
                    out.append(bytes(self.buffer))
                    self.buffer = bytearray()
                continue
            if out or time.monotonic() >= end:
                return out
            time.sleep(0.002)


def open_port(choice=None):
    try:
        import rtmidi
    except ImportError:
        sys.exit("usb_debug: needs python-rtmidi (pip install python-rtmidi)")
    probe_in, probe_out = rtmidi.MidiIn(), rtmidi.MidiOut()
    ins, outs = probe_in.get_ports(), probe_out.get_ports()
    def wanted(name):
        return choice.lower() in name.lower() if choice else "enaga" in name.lower()
    def debug_first(name):
        lowered = name.lower()
        return 0 if "debug" in lowered else 1 if "2" in lowered else 2
    candidates_in = sorted((i for i, n in enumerate(ins) if wanted(n)), key=lambda i: debug_first(ins[i]))
    candidates_out = sorted((i for i, n in enumerate(outs) if wanted(n)), key=lambda i: debug_first(outs[i]))
    if not candidates_in or not candidates_out:
        sys.exit("usb_debug: no Enaga MIDI port: is the unit connected? (ports: " + ", ".join(ins) + ")")
    for o in candidates_out:
        midi_out = rtmidi.MidiOut()
        midi_out.open_port(o)
        opened = []
        for i in candidates_in:
            midi_in = rtmidi.MidiIn()
            midi_in.open_port(i)
            opened.append((i, midi_in))
        clients = [(i, Client(RtMidiTransport(m, midi_out))) for i, m in opened]
        midi_out.send_message(list(request(HELLO)))
        end = time.monotonic() + 1.0
        while time.monotonic() < end:
            for i, client in clients:
                for c, data in client.messages(0.02):
                    if c == HELLO:
                        for j, m in opened:
                            if j != i:
                                m.close_port()
                        print(f"usb_debug: {data.decode('ascii', 'replace')} on {outs[o]} / {ins[i]}", file=sys.stderr)
                        return client
        for _, m in opened:
            m.close_port()
        midi_out.close_port()
    sys.exit("usb_debug: no Enaga port answered (is the firmware older than the debug port?)")


def list_ports():
    import rtmidi
    print("in: ", rtmidi.MidiIn().get_ports())
    print("out:", rtmidi.MidiOut().get_ports())


def follow_log(client, out=None, until=None):
    client.log(True)
    expected = None
    try:
        while until is None or time.monotonic() < until:
            for sequence, text in client.log_lines(0.2):
                if sequence is not None and expected is not None and sequence != expected:
                    text = f"... {sequence - expected} lines skipped\n{text}"
                expected = None if sequence is None else sequence + 1
                print(text, flush=True)
                if out:
                    out.write(text + "\n")
                    out.flush()
    except KeyboardInterrupt:
        pass
    finally:
        client.log(False)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", help="a part of the debug port's name")
    sub = ap.add_subparsers(dest="command", required=True)
    log = sub.add_parser("log", help="the log, until Ctrl-C")
    log.add_argument("--out", type=Path, help="also write the lines here")
    sub.add_parser("status", help="the unit's state, NAME VALUE lines")
    screen = sub.add_parser("screen", help="the screen as a PNG")
    screen.add_argument("png", type=Path)
    peek = sub.add_parser("peek", help="words of memory")
    peek.add_argument("address", help="an address (0x...) or, with --elf, a symbol")
    peek.add_argument("words", type=int, nargs="?", default=1)
    peek.add_argument("--elf", type=Path, help="the firmware's ELF, for symbols (build/firmware/enaga.elf)")
    press = sub.add_parser("press", help="keys held together, then let go")
    press.add_argument("keys", nargs="+", help=f"key names ({', '.join(KEYS)}) or numbers")
    press.add_argument("--hold", type=int, default=0, help="milliseconds to hold them")
    turn = sub.add_parser("turn", help="a knob, by detents")
    turn.add_argument("encoder", choices=list(ENCODERS), type=str.upper)
    turn.add_argument("detents", type=int)
    pad = sub.add_parser("pad", help="a pad pressed and let go")
    pad.add_argument("pad", type=int, choices=range(15), metavar="PAD", help="0-14")
    pad.add_argument("--velocity", type=int, default=100, choices=range(1, 128), metavar="1-127")
    pad.add_argument("--hold", type=int, default=0, help="milliseconds to hold it")
    sub.add_parser("release", help="everything held from the computer let go")
    sub.add_parser("ports", help="the MIDI ports seen")
    args = ap.parse_args(argv)
    if args.command == "ports":
        list_ports()
        return 0
    client = open_port(args.port)
    try:
        return run(client, args)
    except (RuntimeError, TimeoutError, KeyError, ConnectionError) as e:   # refused, no answer, unknown, unplugged
        print(f"usb_debug: {e.args[0] if isinstance(e, KeyError) and e.args else e}", file=sys.stderr)
        return 1


def run(client, args):
    if args.command == "log":
        out = args.out.open("a") if args.out else None
        try:
            follow_log(client, out)
        finally:
            if out:
                out.close()
    elif args.command == "status":
        print("\n".join(client.status()))
    elif args.command == "screen":
        args.png.write_bytes(png(client.screen()))
        print(f"wrote {args.png}")
    elif args.command == "peek":
        if not args.address[:1].isdigit() and not args.elf:
            raise KeyError(f"{args.address}: a symbol needs --elf (the firmware's enaga.elf)")
        address = int(args.address, 0) if args.address[:1].isdigit() else elf_symbol(args.elf, args.address)
        for i, word in enumerate(client.peek(address, args.words)):
            print(f"{address + 4 * i:08X}  {word:08X}  {word}")
    elif args.command in ("press", "turn", "pad", "release"):
        try:
            if args.command == "press":
                client.press(*args.keys, hold=args.hold / 1000)
            elif args.command == "turn":
                client.turn(args.encoder, args.detents)
            elif args.command == "pad":
                client.pad(args.pad, args.velocity)
                if args.hold:
                    client.wait(args.hold / 1000)
                client.pad(args.pad, 0)
        finally:
            try:
                client.release()               # nothing left held, and the unit's mark goes
            except ConnectionError:
                pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
