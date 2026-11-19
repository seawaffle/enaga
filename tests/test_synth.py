"""The synth example (examples/synth) on the emulator: notes from STEP keys and pads at the right pitch, its
worst case inside the audio budget, the pages and knobs, patches and the voice modes, and the same sound
whether the DSP library is built by TI's cl6x or by GCC.

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest discover -s tests -p test_synth.py -v
"""
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from test_e1 import BLOCK_CYCLES, BOOT, HERE, MS, STEP1, build_app, paths, samples
from emulate import WORK, Emulation

sys.path.insert(0, str(HERE.parent / "tools"))
from unit_dry_run import ENCODER_PINS, turn  # noqa: E402
import card  # noqa: E402

SYNTH = HERE.parent / "examples" / "synth"
F1, F2, F3, OK, PUSH_B, SELECT_LEFT, SELECT_RIGHT, CLR = 19, 26, 20, 23, 0, 8, 9, 30
FACTORY = 12                                     # patch_count: the PATCH list's first user slot, U1
DATA = "/ENAGA/DATA/SYNTH"
PARAMS = 86                                      # param_count in examples/synth/params.hpp
CUTOFF, MIX_OSC1, MIX_OSC2, MIX_SUB, MIX_NOISE, OSC1_WAVE = 13, 8, 9, 10, 11, 0
OSC2_WAVE, OSC2_SEMI, OSC2_FINE = 4, 6, 7
FILTER_TYPE, RESONANCE, DRIVE, FILTER_ENV, MOD_FIRST, VOICE_MODE = 12, 14, 15, 16, 37, 70
LFO1_SHAPE, LFO1_RATE, LFO1_SYNC = 31, 32, 33
OSC2_SYNC, MIX_RING, LFO1_FADE, CHORUS_MIX, UNISON_VOICES, UNISON_DETUNE = 76, 77, 78, 82, 83, 84


def hz(note):
    return 440 * 2 ** ((note - 69) / 12)


@unittest.skipUnless(build_app.tools_present(), "tic6x GCC not built (tools/build_tic6x_*.sh)")
@unittest.skipUnless(paths.emulator_present(), "ENAGA_EMULATOR not set to the SmplTrek emulator directory")
class Synth(unittest.TestCase):
    def start(self, card_files=None):
        """The synth booted; with card_files ({path: bytes}), on a card made by tools/card.py (long names) holding
        them and the synth."""
        emu = Emulation(SYNTH, "SYNTH")
        if card_files is not None:
            with tempfile.TemporaryDirectory() as d:
                image = Path(d) / "card.img"
                package = (WORK / "emulate" / "SYNTH.ENA").read_bytes()
                card.make(image, 16, "fat", {"/ENAGA/APPS/SYNTH/APP.ENA": package, **card_files})
                emu.cpu.sd_load(image.read_bytes())
        emu.cpu.gpio_input(3, 0, True)           # POWER up (active low)
        for pins in ENCODER_PINS:                # at rest both encoder pins are high
            for bank, pin in pins:
                emu.cpu.gpio_input(bank, pin, True)
        self.assertIsNone(emu.run(BOOT)["error"])
        report = emu.report()
        self.assertEqual((report["app_status"], report["log"][-2:]), ("APP RUNNING", ["synth ready", "SYNTH"]))
        # The engine's parameter values: the first member of the Engine, after the app object's vtable pointer.
        symbols = build_app.symbols(WORK / "apps" / "synth" / "app.elf")
        instance = next(a for name, a in symbols.items() if name.endswith("8instance") and not name.startswith("_ZGV"))
        emu.values = instance + 4
        return emu

    def values(self, emu):
        return list(struct.unpack(f"<{PARAMS}h", emu.cpu.read(emu.values, 2 * PARAMS)))

    def set_values(self, emu, changes):
        v = self.values(emu)
        for param, value in changes.items():
            v[param] = value
        emu.cpu.write(emu.values, struct.pack(f"<{PARAMS}h", *v))

    def active_voices(self, emu):
        # Engine::active_voices follows values[] (int16_t each), at the next word.
        return struct.unpack("<I", emu.cpu.read(emu.values + (2 * PARAMS + 3) // 4 * 4, 4))[0]

    def press(self, emu, key):
        emu.key(key, True)
        emu.run(15 * MS)
        emu.key(key, False)
        emu.run(15 * MS)

    def pad(self, emu, pad, reading):
        for _ in range(2):                        # the MCU reports each pad every 3.8 ms; a press needs two reports
            emu.cpu.uart_receive(1, bytes([0xB0 | pad, reading & 0x7F, reading >> 7]))
            emu.run(4 * MS)

    def pitch(self, emu, ms=200):
        """The left channel's frequency from rising zero crossings over the last `ms`."""
        left = samples(emu)[0::2][-48 * ms:]
        crossings = [i for i in range(1, len(left)) if left[i - 1] < 0 <= left[i]]
        return (len(crossings) - 1) * 48000 / (crossings[-1] - crossings[0])

    def test_keys_and_pads_play_in_tune(self):
        with self.start() as emu:
            emu.key(STEP1, True)                  # STEP 1: C3, a saw through the open filter
            emu.run(250 * MS)
            self.assertAlmostEqual(self.pitch(emu), hz(48), delta=0.5)
            emu.key(STEP1, False)
            emu.run(300 * MS)
            self.assertEqual(emu.word("audio_overruns"), 0)
            # Pads, piano layout: channel 7 is the upper row's third pad, F#; 5 lies between E and F, no note.
            self.pad(emu, 5, 5000)
            emu.run(50 * MS)
            self.assertLess(max(map(abs, samples(emu)[-960:])), 2 ** 20)
            self.pad(emu, 7, 5000)
            emu.run(250 * MS)
            self.assertAlmostEqual(self.pitch(emu), hz(54), delta=0.5)
            pad_green = emu.cpu.read(emu.names["led_level"], 144)[48 * 0 + 8 * 2 + 6]   # pad 7 green: (0, 2, 6)
            self.assertGreater(pad_green, 0)
            self.press(emu, 31)                   # octave right: the next pad is an octave up
            self.pad(emu, 7, 0)
            self.pad(emu, 0, 9000)                # channel 0: lower row's first pad, C
            emu.run(250 * MS)
            self.assertAlmostEqual(self.pitch(emu), hz(60), delta=0.5)

    def test_worst_case_stays_inside_the_audio_budget(self):
        with self.start() as emu:
            # Everything on: pulse and a synced saw, ring, sub and noise, 24 dB filter with drive, all eight slots
            # routed, LFO fade, chorus.
            changes = {OSC1_WAVE: 1, MIX_OSC2: 80, MIX_SUB: 60, MIX_NOISE: 30, FILTER_TYPE: 1, CUTOFF: 70,
                       RESONANCE: 50, DRIVE: 40, OSC2_SYNC: 1, MIX_RING: 50, LFO1_FADE: 60, CHORUS_MIX: 50}
            for slot in range(8):
                changes.update({MOD_FIRST + 3 * slot: 1 + slot, MOD_FIRST + 3 * slot + 1: slot + 3,
                                MOD_FIRST + 3 * slot + 2: 10})
            self.set_values(emu, changes)
            for n in range(10):                   # more keys than voices: stealing as well
                emu.key(STEP1 + n, True)
                emu.run(10 * MS)
            emu.run(200 * MS)
            peak = emu.word("audio_cycles_peak") / BLOCK_CYCLES
            print(f"\nsynth worst case: {peak:.0%} of the audio budget")
            self.assertLess(peak, 0.8)
            self.assertEqual(emu.word("audio_overruns"), 0)

    def test_pages_knobs_and_patches(self):
        with self.start() as emu:
            self.press(emu, F3)                   # FILTER page, first group: TYPE CUTOFF RESO DRIVE on A-D
            turn(emu, 1, -3)
            self.assertEqual(self.values(emu)[CUTOFF], 124)
            turn(emu, 4, 5)                       # VALUE does nothing on a parameter page
            self.assertEqual(self.values(emu)[CUTOFF], 124)
            self.press(emu, SELECT_RIGHT)         # the next group, FILTER MOD: A is ENV2's amount
            turn(emu, 0, 5)
            self.assertEqual(self.values(emu)[FILTER_ENV], 5)
            self.press(emu, SELECT_LEFT)          # back to FILTER; pushing B restores the patch's cutoff
            self.press(emu, PUSH_B)
            self.assertEqual(self.values(emu)[CUTOFF], 127)
            emu.key(F1, True)                     # F1 + F2: PATCH; VALUE chooses, OK loads
            self.press(emu, F2)
            emu.key(F1, False)
            turn(emu, 4, 1)
            self.assertEqual(self.values(emu)[CUTOFF], 127)   # chosen, not loaded yet
            self.press(emu, OK)
            self.assertEqual(self.values(emu)[CUTOFF], 50)    # BRASS
            emu.run(60 * MS)
            frame, _ = emu.cpu.oled_frame()
            self.assertTrue(any(frame))
            self.assertEqual(emu.report()["app_status"], "APP RUNNING")

    def patch_page(self, emu):
        emu.key(F1, True)
        self.press(emu, F2)
        emu.key(F1, False)
        emu.run(15 * MS)

    def card_files(self, emu):
        """Every file in the synth's data folder: {name: bytes}."""
        with tempfile.TemporaryDirectory() as d:
            image = Path(d) / "card.img"
            image.write_bytes(emu.cpu.sd_read())
            return {name: card.get(image, f"{DATA}/{name}") for name, _ in card.listdir(image, DATA) or []}

    @staticmethod
    def patch_file(name, values, entry=-1, base=None):
        sets = [values] + ([base] if base else [])
        return (b"ESYN" + struct.pack("<BBHh", 1, len(sets), len(values), entry) + name.encode().ljust(16, b"\0") +
                b"".join(struct.pack(f"<{len(v)}h", *v) for v in sets))

    def test_user_patches_and_the_last_sound(self):
        # U2 holds a patch from an older synth: ten parameters (OSC 1, OSC 2, two of the mixer); the rest INIT.
        old = [3, 64, 0, 0, 1, 64, -12, 5, 90, 20]
        with self.start({f"{DATA}/USER 02.SYN": self.patch_file("OLD ONE", old)}) as emu:
            init = self.values(emu)
            self.press(emu, F3)                   # FILTER: cutoff 127 -> 124
            turn(emu, 1, -3)
            self.patch_page(emu)
            emu.key(F1, True)                     # F1 + OK on a factory patch saves nothing
            self.press(emu, OK)
            emu.key(F1, False)
            turn(emu, 4, FACTORY)                 # U1, empty: OK leaves the sound as it is
            self.press(emu, OK)
            self.assertEqual(self.values(emu)[CUTOFF], 124)
            emu.key(F1, True)                     # F1 + OK: saved in U1
            self.press(emu, OK)
            emu.key(F1, False)
            turn(emu, 4, 1)                       # U2, the older file
            self.press(emu, OK)
            self.assertEqual(self.values(emu), old + init[len(old):])
            turn(emu, 4, -FACTORY)                # BRASS, then U1 again
            self.press(emu, OK)
            self.assertEqual(self.values(emu)[CUTOFF], 50)
            turn(emu, 4, FACTORY - 1)
            self.press(emu, OK)
            self.assertEqual(self.values(emu), init[:CUTOFF] + [124] + init[CUTOFF + 1:])
            self.press(emu, F3)                   # an edit after loading U1: 124 -> 120
            turn(emu, 1, -4)
            emu.cpu.gpio_input(3, 0, False)       # switched off: POWER held a second saves the sound
            emu.run(1200 * MS)
            files = self.card_files(emu)
        self.assertEqual(sorted(files), ["LAST.SYN", "USER 01.SYN", "USER 02.SYN"])
        edited = init[:CUTOFF] + [124] + init[CUTOFF + 1:]
        self.assertEqual(files["USER 01.SYN"], self.patch_file("INIT", edited))
        now = edited[:CUTOFF] + [120] + edited[CUTOFF + 1:]
        self.assertEqual(files["LAST.SYN"], self.patch_file("INIT", now, FACTORY, edited))
        # The next start: the sound as it was switched off, U1 shown loaded, and pushing B goes back to U1's cutoff.
        with self.start({f"{DATA}/{name}": data for name, data in files.items()}) as emu:
            self.assertEqual(self.values(emu), now)
            self.press(emu, F3)
            self.press(emu, PUSH_B)
            self.assertEqual(self.values(emu)[CUTOFF], 124)

    def test_mono_returns_to_the_held_note(self):
        with self.start() as emu:
            self.set_values(emu, {VOICE_MODE: 1})
            emu.run(5 * MS)
            emu.key(STEP1, True)
            emu.run(30 * MS)
            emu.key(STEP1 + 7, True)              # G3 over C3: one voice, last note wins
            emu.run(250 * MS)
            self.assertAlmostEqual(self.pitch(emu), hz(55), delta=0.5)
            emu.key(STEP1 + 7, False)             # back to C3 while it is still held
            emu.run(250 * MS)
            self.assertAlmostEqual(self.pitch(emu), hz(48), delta=0.5)
            self.press(emu, CLR)                  # CLR silences everything
            emu.run(50 * MS)
            self.assertLess(max(map(abs, samples(emu)[-960:])), 2 ** 20)

    def test_hard_sync_and_ring(self):
        with self.start() as emu:
            # OSC 2 alone, a fourth up (five semitones): its own pitch; synced, it repeats at OSC 1's.
            self.set_values(emu, {MIX_OSC1: 0, MIX_OSC2: 80, OSC2_SEMI: 5, OSC2_FINE: 0})
            emu.run(5 * MS)
            emu.key(STEP1, True)
            emu.run(250 * MS)
            self.assertAlmostEqual(self.pitch(emu), hz(53), delta=0.5)
            self.set_values(emu, {OSC2_SYNC: 1})
            emu.run(250 * MS)
            self.assertAlmostEqual(self.pitch(emu), hz(48), delta=0.5)
            # Both oscillators' levels at 0: only the ring modulator sounds.
            self.set_values(emu, {OSC2_SYNC: 0, MIX_OSC2: 0})
            emu.run(100 * MS)
            self.assertLess(max(map(abs, samples(emu)[-960:])), 2 ** 20)
            self.set_values(emu, {MIX_RING: 100})
            emu.run(100 * MS)
            self.assertGreater(max(map(abs, samples(emu)[-960:])), 2 ** 26)
            self.assertEqual(emu.word("audio_overruns"), 0)

    def test_unison_chorus_and_lfo_fade(self):
        with self.start() as emu:
            # A centred voice: left = right, until the chorus sweeps the two sides apart.
            emu.key(STEP1, True)
            emu.run(200 * MS)
            out = samples(emu)[-9600:]
            self.assertGreater(max(map(abs, out)), 2 ** 26)
            self.assertEqual(out[0::2], out[1::2])
            self.set_values(emu, {CHORUS_MIX: 80})
            emu.run(200 * MS)
            out = samples(emu)[-9600:]
            self.assertNotEqual(out[0::2], out[1::2])
            emu.key(STEP1, False)
            self.set_values(emu, {CHORUS_MIX: 0, UNISON_VOICES: 3, UNISON_DETUNE: 0})
            emu.run(1500 * MS)
            self.assertEqual(self.active_voices(emu), 0)
            # Three voices a note, in tune with no detune (sines: from their random phases they sum to one sine, where
            # three saws would cross zero three times a cycle).
            self.set_values(emu, {OSC1_WAVE: 3})
            emu.run(5 * MS)
            emu.key(STEP1, True)
            emu.run(250 * MS)
            self.assertEqual(self.active_voices(emu), 3)
            self.assertAlmostEqual(self.pitch(emu), hz(48), delta=0.5)
            emu.key(STEP1, False)
            # LFO 1, a square held high (0.05 Hz, from the key), an octave up on pitch: at once without a fade,
            # hardly at all in the first quarter second of a 19 s fade.
            self.set_values(emu, {UNISON_VOICES: 1, OSC1_WAVE: 0, LFO1_SHAPE: 3, LFO1_RATE: 0, LFO1_SYNC: 1,
                                  MOD_FIRST: 3, MOD_FIRST + 1: 0, MOD_FIRST + 2: 100})
            emu.run(1500 * MS)
            emu.key(STEP1, True)
            emu.run(250 * MS)
            self.assertAlmostEqual(self.pitch(emu), hz(60), delta=1)
            emu.key(STEP1, False)
            self.set_values(emu, {LFO1_FADE: 127})
            emu.run(1500 * MS)
            emu.key(STEP1, True)
            emu.run(250 * MS)
            self.assertAlmostEqual(self.pitch(emu), hz(48), delta=1.5)
            self.assertEqual(emu.word("audio_overruns"), 0)

    def test_a_voice_taken_while_sounding_does_not_click(self):
        # Eight sines held, then a ninth note takes the oldest voice. Low sines change little from one sample to
        # the next; a voice whose phase or filter is reset at its level steps by a large part of its amplitude.
        with self.start() as emu:
            self.set_values(emu, {OSC1_WAVE: 3, MIX_OSC2: 0})
            for k in range(8):
                emu.key(STEP1 + k, True)
                emu.run(5 * MS)
            emu.run(300 * MS)
            self.assertEqual(self.active_voices(emu), 8)
            held = samples(emu)[-4800 * 2:][0::2]
            emu.key(STEP1 + 8, True)
            emu.run(100 * MS)
            taken = samples(emu)[-4800 * 2:][0::2]
            steady = max(abs(b - a) for a, b in zip(held, held[1:]))
            step = max(abs(b - a) for a, b in zip(taken, taken[1:]))
            self.assertLess(step, 2 * steady, (step, steady))

    @unittest.skipUnless(paths.ti_present(), "TI's cl6x not installed (tools/paths.py, TI_CGT)")
    def test_cl6x_and_gcc_builds_sound_the_same(self):
        # The DSP library (runtime/dsp) is built by cl6x when it is installed, by GCC otherwise. Both do the same IEEE
        # operations in the same order (the C674x has no fused multiply-add), so the samples match exactly. When the
        # keys reach the engine depends on the main loop, which runs slower under a heavier audio load: compare
        # from each capture's first sound.
        def play(ti):
            with mock.patch.object(paths, "TI_CGT", paths.TI_CGT if ti else None), self.start() as emu:
                self.assertEqual("dsplib/cl6x/" in (WORK / "apps" / "synth" / "app.map").read_text(), ti)
                self.set_values(emu, {OSC1_WAVE: 1, MIX_OSC2: 80, MIX_SUB: 60, FILTER_TYPE: 1, CUTOFF: 80,
                                      RESONANCE: 50, DRIVE: 40, OSC2_SYNC: 1, MIX_RING: 40, CHORUS_MIX: 50})
                for n in (0, 4, 7):
                    emu.key(STEP1 + n, True)
                emu.run(250 * MS)
                captured = samples(emu)
                start = next(i for i, v in enumerate(captured) if v)
                self.assertLess(start, len(captured) - 2 * 48 * 200)
                return captured[start:start + 2 * 48 * 200]
        ti, gcc = play(True), play(False)
        self.assertGreater(max(map(abs, ti)), 2 ** 28)
        self.assertEqual(ti, gcc)


if __name__ == "__main__":
    unittest.main()
