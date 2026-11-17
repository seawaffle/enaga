"""The synth example (examples/synth) on the emulator: notes from STEP keys and pads at the right pitch, its
worst case inside the audio budget, the pages and knobs, patches and the voice modes, and the same sound
whether voice.ti.c is built by TI's cl6x or by GCC.

    ENAGA_EMULATOR=/path/to/emulator python3 -B -m unittest discover -s tests -p test_synth.py -v
"""
import struct
import sys
import unittest
from unittest import mock

from test_e1 import BLOCK_CYCLES, BOOT, HERE, MS, STEP1, build_app, paths, samples
from emulate import WORK, Emulation

sys.path.insert(0, str(HERE.parent / "tools"))
from unit_dry_run import ENCODER_PINS, turn  # noqa: E402

SYNTH = HERE.parent / "examples" / "synth"
F1, F2, F3, OK, PUSH_B, SELECT_LEFT, SELECT_RIGHT, CLR = 19, 26, 20, 23, 0, 8, 9, 30
PARAMS = 76                                      # param_count in examples/synth/params.hpp
CUTOFF, MIX_OSC2, MIX_SUB, MIX_NOISE, OSC1_WAVE = 13, 9, 10, 11, 0
FILTER_TYPE, RESONANCE, DRIVE, FILTER_ENV, MOD_FIRST, VOICE_MODE = 12, 14, 15, 16, 37, 70


def hz(note):
    return 440 * 2 ** ((note - 69) / 12)


@unittest.skipUnless(build_app.tools_present(), "tic6x GCC not built (tools/build_tic6x_*.sh)")
@unittest.skipUnless(paths.emulator_present(), "ENAGA_EMULATOR not set to the SmplTrek emulator directory")
class Synth(unittest.TestCase):
    def start(self):
        emu = Emulation(SYNTH, "SYNTH")
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

    def press(self, emu, key):
        emu.key(key, True)
        emu.run(15 * MS)
        emu.key(key, False)
        emu.run(15 * MS)

    def pad(self, emu, pad, reading):
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
            # Everything on: pulse and saw, sub and noise, 24 dB filter with drive, all eight slots routed.
            changes = {OSC1_WAVE: 1, MIX_OSC2: 80, MIX_SUB: 60, MIX_NOISE: 30, FILTER_TYPE: 1, CUTOFF: 70,
                       RESONANCE: 50, DRIVE: 40}
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

    @unittest.skipUnless(paths.ti_present(), "TI's cl6x not installed (tools/paths.py, TI_CGT)")
    def test_cl6x_and_gcc_builds_sound_the_same(self):
        # voice.ti.c is built by cl6x when it is installed, by GCC otherwise. Both do the same IEEE operations in
        # the same order (the C674x has no fused multiply-add), so the samples match exactly.
        def play(ti):
            with mock.patch.object(paths, "TI_CGT", paths.TI_CGT if ti else None), self.start() as emu:
                self.assertEqual("voice_ti.o" in (WORK / "apps" / "synth" / "app.map").read_text(), ti)
                self.set_values(emu, {OSC1_WAVE: 1, MIX_OSC2: 80, MIX_SUB: 60, FILTER_TYPE: 1, CUTOFF: 80,
                                      RESONANCE: 50, DRIVE: 40})
                for n in (0, 4, 7):
                    emu.key(STEP1 + n, True)
                emu.run(250 * MS)
                return samples(emu)[-2 * 48 * 200:]
        ti, gcc = play(True), play(False)
        self.assertGreater(max(map(abs, ti)), 2 ** 28)
        self.assertEqual(ti, gcc)


if __name__ == "__main__":
    unittest.main()
