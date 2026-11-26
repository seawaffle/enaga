# The USB debug port

A unit running Enaga has a second USB MIDI port, the debug port, for seeing into the unit and driving it from a
computer. With `tools/usb_debug.py` you can:
- follow the log as it happens, including your app's `enaga::log` lines;
- read Enaga's state: the app, crashes, audio load, USB, MIDI and SD counters;
- take screenshots of the unit's display as PNG files;
- press keys, turn knobs and press pads, from the command line or from a Python script.

Together with USB MIDI and USB audio, this means an app can be tried on a real unit by a script: press things, check
the screen and the log, listen to the sound. You don't need to photograph the display, and no one has to sit at the
unit.

The debug port doesn't change anything for apps. It sends nothing until a computer asks. An app's own USB MIDI keeps
working beside it: the debug traffic never holds MIDI up by more than four USB packets (about 0.3 ms).

## Setting up

You need Python 3.9 or newer and python-rtmidi, on Windows, macOS or Linux:

```
pip install python-rtmidi
python3 -B tools/usb_debug.py status
```

Connect the unit by USB first. The tool finds the debug port by asking each Enaga MIDI port in turn, and prints the
one it chose on its first line:

```
usb_debug: ENAGA 0.3.0 2 on MIDIOUT2 (Enaga SmplTrek) 5 / MIDIIN2 (Enaga SmplTrek) 4
```

The port's name differs by system: "Enaga Debug" where the system uses the USB descriptor's names, and
"MIDIIN2 / MIDIOUT2 (Enaga SmplTrek)" on Windows. `python3 -B tools/usb_debug.py ports` lists the MIDI ports, and
`--port NAME` (any part of the name) picks one.

Things to know:
- **One program at a time.** On Windows, only one program can open a MIDI port at once. Close anything that has the
  debug port open before running the tool. A DAW or MIDI monitor on the ordinary "Enaga SmplTrek" port is fine.
- **WSL.** WSL doesn't see the PC's MIDI ports, so run the tool with Windows' own Python.
- **Matching versions.** The tool needs the firmware it came with. If `status` says no Enaga port answered, check
  which Enaga is installed.

## Commands

| Command | Does |
|---|---|
| `log` | the log: the last 32 lines at once, then each new line as it is logged, until Ctrl-C. `--out FILE` also writes the lines to FILE |
| `status` | Enaga's state, one `NAME VALUE` line per item (below) |
| `screen shot.png` | the display as a 512 x 512 PNG, enlarged four times |
| `press KEY [KEY ...]` | keys pressed in order, held together, then let go in reverse. `--hold MS` holds them |
| `turn ENCODER DETENTS` | a knob, A-D or VALUE, by detents. Negative values turn it anticlockwise |
| `pad PAD` | pad 0-14 pressed and let go. `--velocity 1-127` (default 100), `--hold MS` |
| `release` | lets go of everything held from the computer |
| `peek ADDRESS [WORDS]` | words of memory (below) |
| `ports` | the MIDI ports the system shows |

Key names are those of `enaga_key` without the prefix: `STEP1`-`STEP16`, `F1`-`F8`, `OK`, `CLR`, `UP`, `DOWN`, `LEFT`,
`RIGHT`, `SELECT_LEFT`, `SELECT_RIGHT`, `OCTAVE_LEFT`, `OCTAVE_RIGHT`, `PUSH_A`-`PUSH_D` and `PUSH_VALUE` (pushing a
knob), plus `POWER`. Numbers work too.

Some examples:

```
python3 -B tools/usb_debug.py log --out session.log        # follow the log, keeping a copy
python3 -B tools/usb_debug.py press F1 POWER               # open the Enaga menu
python3 -B tools/usb_debug.py press DOWN                   # move down a line
python3 -B tools/usb_debug.py screen menu.png              # see where you are
python3 -B tools/usb_debug.py press F4 --hold 1200         # a long press
python3 -B tools/usb_debug.py pad 5 --velocity 30          # a soft hit on pad 5
```

## Logging from your app

`enaga::log(text)` adds a line to Enaga's log. The unit keeps the last 32 lines, which the STATUS page shows, and
`usb_debug.py log` shows them on the computer as they happen, within a few milliseconds. Lines are cut at 39
characters. The log is the simplest way to see what your app is doing: log events, results and errors, and follow
them on the computer.

`log` numbers the lines it receives. If lines were logged faster than the computer read them, it says how many were
lost.

## Input from the computer

Keys, knobs and pads from the computer reach your app exactly as if they were pressed on the unit. Your app needs no
code for it. The same goes for the menu and key combinations: `press F1 POWER` opens the Enaga menu.

How it behaves:
- **Each input waits for the screen.** A command returns once the unit has delivered the input and drawn a new frame
  after it, so a screenshot taken next shows the effect. A press (down and up) takes about 100 ms and a screenshot
  about 50 ms. A whole command line takes about 250 ms, because Python starts and opens the port each time; a script
  doesn't pay that more than once.
- **Pads give press, pressure and release, but no raw readings.** `on_pad_press`, `on_pad_pressure` and
  `on_pad_release` are called. `on_pad`, the raw pressure reading, is not, because those readings come from the pad
  hardware itself.
- **POWER only with F1.** POWER is refused unless F1 is held, because on its own it would switch the unit off. A
  computer can't switch the unit off.
- **A mark on the display.** While the computer holds anything, a small bright square shows at the top right of the
  unit's display. It is not part of your app's frame and doesn't appear in screenshots.
- **The unit's own controls win.** Pressing or turning anything on the unit lets go of everything the computer held.
- **Holds end on their own.** Everything held from the computer is let go after 5 s without any request from it, so a
  script that crashes while holding a key doesn't leave it held. To hold longer, use `Client.wait` (below), which
  keeps the hold going. Unplugging USB lets go too.

## Scripts

`usb_debug.py` is also a Python module. Put the folder holding it on the path (for example
`PYTHONPATH=path/to/sdk/tools`), then:

```python
import usb_debug

c = usb_debug.open_port()                # finds the debug port; Client
try:
    c.log(True)
    c.log_lines(0.5)                     # the lines kept from before
    c.press("STEP1")
    lines = [text for _, text in c.log_lines(1.0)]
    assert "STEP 1 ON" in lines, lines   # whatever your app logs
    frame = c.screen()                   # 128 rows of 128 greys, 0-15
    assert frame[10][20] == 15           # a pixel your app lights
    with open("after-step1.png", "wb") as f:
        f.write(usb_debug.png(frame))
    c.pad(3, 100)                        # a pad held, with pressure, for 2 s
    c.pressure(3, 60)
    c.wait(2)
    c.pad(3, 0)
finally:
    c.log(False)
    c.release()                          # nothing left held
```

`Client` methods:

| Method | Does |
|---|---|
| `press(*keys, hold=0.0)` | keys pressed in order, held `hold` seconds, let go in reverse |
| `key(key, down)` | one key down (`True`) or up |
| `turn(encoder, detents)` | a knob, `"A"`-`"D"`, `"VALUE"` or 0-4, by detents (-8192 to 8191) |
| `pad(pad, velocity=100)` | pad 0-14 pressed with velocity 1-127, or let go with 0 |
| `pressure(pad, value)` | pressure 0-127 on a pad held with `pad()` |
| `release()` | lets go of everything held from the computer |
| `wait(seconds)` | waits, keeping held input held. Use it instead of `time.sleep` while anything is held |
| `screen()` | the display: a list of 128 rows, each 128 greys from 0 (black) to 15 |
| `log(on)` | starts or stops the log |
| `log_lines(timeout)` | `(number, text)` for the log lines that arrived, waiting up to `timeout` seconds for the first |
| `status()` | the `NAME VALUE` lines, as a list of strings |
| `hello()` | the firmware's greeting, `ENAGA <version> <protocol>` |
| `peek(address, words=1)` | words of memory |

`usb_debug.png(frame)` turns a frame into PNG bytes. `usb_debug.KEYS` maps key names to numbers. The methods raise
`RuntimeError` when the unit refuses something (`POWER` without F1, pressure on a pad not held), `TimeoutError` when
no answer comes, and `ConnectionError` when the port goes away.

With USB MIDI (any MIDI library, on the "Enaga SmplTrek" port) and USB audio (the unit is a sound card: record its
output, or play into it as an input), a script can check an app's MIDI and sound as well.

## Status

`status` prints one line per item:

| Line | Shows |
|---|---|
| `VERSION` | Enaga's version |
| `BUILD` | the commit the firmware was built from (`d0eb6c1`), with `+` if it had changes not yet committed, or `unknown` |
| `UPTIME_MS` | milliseconds since power-on |
| `APP`, `APP_STATUS` | the app running, and whether it runs, failed to load, or was stopped |
| `CRASH`, `CRASH_PC` | why the last app stopped, and where, if it crashed |
| `AUDIO` | the audio engine's state, blocks run, and overruns (blocks the app's `audio()` took too long for) |
| `AUDIO_LOAD_PEAK` | the highest audio load since start-up, per mille of the block's time |
| `USB_AUDIO_OUT`, `USB_AUDIO_IN` | USB audio packets each way, with overruns and underruns |
| `USB_DRIVE` | USB DRIVE's sectors read and written, and errors |
| `USB_MIDI_LOST` | USB MIDI bytes dropped because the computer wasn't reading |
| `MIDI` | MIDI messages in (from the inputs the MIDI IN setting allows) and out, and messages dropped |
| `SD` | SD card sectors read and written, and the last error |
| `BATTERY` | 0-4, or -1 on USB or adapter power |
| `LOG_COUNT` | lines logged since start-up |
| `USB_DRIVE_TIME` | since USB DRIVE opened, per direction: 8 KiB reads (`R`) and writes (`W`), ms in the SD driver, ms between them; then chunks read ahead and written behind (the card working while USB moves the next) |
| `USB_DRIVE_PACKETS` | since USB DRIVE opened: the drive's packets to the computer and the ms it took to collect them, and packets from it and the ms between them |
| `USB_DRIVE_FIFO` | at each of those packets, whether the unit had the next one ready (`IN_QUEUED`, `OUT_ALONE`: the computer is the slower side) or not (`IN_EMPTY`, `OUT_BACKLOG`) |
| `USB_ISR` | since USB DRIVE opened: USB interrupts, their total and longest time in microseconds |

The four `USB_DRIVE_...` lines and `USB_ISR` are for work on Enaga's USB DRIVE speed. On a computer, a port on the
CPU's own USB controller (often the rear ones) can be several times faster for USB DRIVE than one on the chipset's,
and the unit's audio streams take some of the bandwidth: disable the unit's audio devices in the computer's sound
settings during a big copy if they aren't needed.

## Peek

`peek` reads words of RAM, the USB controller's wrapper registers and the system configuration registers. Other
addresses are refused. Give an address (`peek 0xC0001000 4`), or a firmware variable's name with the firmware's
symbols (`peek log_count --elf enaga.elf`). This is mostly for work on Enaga itself.

## The protocol

To talk to the port from another language: every message is a SysEx with the non-commercial ID, `F0 7D 45 <command>
<data> F7`, on the debug port. Data bytes are 7-bit. Numbers go as groups of 7 bits, lowest first: a 32-bit word in 5
bytes, a 14-bit value in 2.

| Command | From the computer | The unit's answer |
|---|---|---|
| `00` | HELLO | `00` "ENAGA <version> <protocol>" |
| `01` | LOG ON | `01` <number: 3 bytes> <text> for each line kept, then each new line; `02` <count: 3 bytes> for lines lost |
| `02` | LOG OFF | none |
| `03` | STATUS | `03` <text> per line, then `03` alone |
| `04` | SCREEN | `04` <row> <128 greys> for each row, 0-127 |
| `05` | PEEK <address: 5> <words: 1-32> | `05` <address: 5> <word: 5> ... |
| `06` | KEY <key: 0-47, 48 for POWER> <1 down, 0 up> | `06`, once a frame has been drawn after it |
| `07` | TURN <encoder: 0-4> <detents: 2, two's complement> | `07`, likewise |
| `08` | PAD <pad: 0-14> <velocity: 1-127, 0 releases> | `08`, likewise |
| `09` | PRESSURE <pad> <0-127> | `09`, likewise |
| `0A` | RELEASE | `0A`, likewise |

A refused request is answered `7F <command> "<reason>"`, and an unknown command `7F <command> "UNKNOWN"`. The unit
answers one request at a time: a new one waits for the message under way to finish, then replaces the rest of the
answer. If the computer stops reading for 2 s, the unit stops sending the log and the answer. The protocol number in
HELLO's answer goes up when commands are added. `firmware/usb/usb_debug.c` has the full description.
