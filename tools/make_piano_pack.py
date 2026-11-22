#!/usr/bin/env python3
"""Build the "Piano" Mechvibes keyboard pack (synthesised piano notes) and install it.

usage: make_piano_pack.py [--out DIR]     default: ~/.local/share/mechvibes/soundpacks/keyboard/piano

Layout: the home row is the white keys of C4..F5 and the QWERTY row above it holds the black keys (like a
piano roll); the bottom row is C3..E4, the number row C6..G7, the function keys a chromatic octave, and the
remaining keys play low or soft notes so every key makes a sound. Samples are additive-synthesis piano
tones (inharmonic partials, detuned string pairs, a short hammer thump), generated here, not recorded.
"""

import argparse
import json
import wave
from pathlib import Path

import numpy as np

RATE = 44100
LEVEL = 0.4  # master gain of the generated packs (piano and koto): -8 dB, about a third quieter to the ear
NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def midi(name):
    """'C#4' -> MIDI note number."""
    pitch, octave = name[:-1], int(name[-1])
    return 12 * (octave + 1) + NAMES.index(pitch)


def tone(note, seconds=2.4, gain=1.0):
    f0 = 440 * 2 ** ((midi(note) - 69) / 12)
    t = np.arange(int(RATE * seconds)) / RATE
    rng = np.random.default_rng(midi(note))
    out = np.zeros_like(t)
    tau0 = np.clip(0.35 + 2.2 * (261.6 / f0) ** 0.6, 0.35, 3.2)  # low notes ring longer
    inharmonicity = 0.00035
    for n in range(1, 16):
        fn = n * f0 * np.sqrt(1 + inharmonicity * n * n)
        if fn > 9000:
            break
        amp = (1 / n ** 1.15) * (1.0 if n < 9 else 0.5)
        decay = np.exp(-t / (tau0 / (1 + 0.45 * (n - 1))))
        for detune, phase in ((0.0, rng.uniform(0, 6.28)), (0.6 + 0.2 * n * 0.1, rng.uniform(0, 6.28))):
            out += 0.5 * amp * decay * np.sin(2 * np.pi * (fn + detune) * t + phase)
    attack = 1 - np.exp(-t / 0.002)
    out *= attack
    thump = rng.standard_normal(len(t)) * np.exp(-t / 0.012) * 0.04  # hammer
    lowpass = np.convolve(thump, np.ones(24) / 24, mode="same")
    out += lowpass
    fade = np.clip((seconds - t) / 0.15, 0, 1)
    out *= fade
    out *= 0.8 / np.max(np.abs(out)) * gain
    return out


def chord(notes, gain=1.0):
    mix = sum(tone(n, 2.4) for n in notes)
    return mix * (0.8 * gain / np.max(np.abs(mix)))


def write_wav(path, samples):
    pcm = (np.clip(samples * LEVEL, -1, 1) * 32767).astype("<i2")
    with wave.open(str(path), "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(RATE)
        f.writeframes(pcm.tobytes())


def layout():
    keys = {}
    white = "KeyA KeyS KeyD KeyF KeyG KeyH KeyJ KeyK KeyL Semicolon Quote".split()
    for code, note in zip(white, "C4 D4 E4 F4 G4 A4 B4 C5 D5 E5 F5".split()):
        keys[code] = note
    for code, note in zip("KeyW KeyE KeyT KeyY KeyU KeyO KeyP".split(), "C#4 D#4 F#4 G#4 A#4 C#5 D#5".split()):
        keys[code] = note
    for code, note in zip("KeyZ KeyX KeyC KeyV KeyB KeyN KeyM Comma Period Slash".split(),
                          "C3 D3 E3 F3 G3 A3 B3 C4 D4 E4".split()):
        keys[code] = note
    row = "Digit1 Digit2 Digit3 Digit4 Digit5 Digit6 Digit7 Digit8 Digit9 Digit0 Minus Equal".split()
    for code, note in zip(row, "C6 D6 E6 F6 G6 A6 B6 C7 D7 E7 F7 G7".split()):
        keys[code] = note
    for code, note in zip(["F%d" % i for i in range(1, 13)], [n + "5" for n in NAMES]):
        keys[code] = note
    keys.update({"KeyQ": "A#3", "KeyR": "F#3", "KeyI": "F#5", "BracketLeft": "G5", "BracketRight": "A5",
                 "Backslash": "B5", "Backquote": "C2", "Tab": "G2", "Escape": "C2", "Delete": "A2",
                 "Home": "D3", "End": "E3", "PageUp": "F3", "PageDown": "G3", "Insert": "B2",
                 "ArrowLeft": "A4", "ArrowDown": "E4", "ArrowUp": "C5", "ArrowRight": "E5"})
    return keys


def write_pack(out, meta, keys, render, specials, note_ms):
    """Write the pack folder: one wav per distinct note (made by `render(note)`), the special-key samples and config.json.

    `keys` maps key code -> note; `specials` maps sample name -> (samples, key codes).
    """
    out.mkdir(parents=True, exist_ok=True)
    definitions, files = {}, {}
    for code, note in keys.items():
        name = note.replace("#", "s")
        if name not in files:
            files[name] = render(note)
        definitions[code] = {"audio_file": f"{name}.wav", "timing": [[0, note_ms]]}
    for name, (samples, codes) in specials.items():
        files[name] = samples
        for code in codes:
            definitions[code] = {"audio_file": f"{name}.wav", "timing": [[0, round(len(samples) / RATE * 1000)]]}
    for name, samples in files.items():
        write_wav(out / f"{name}.wav", samples)
    config = {
        "id": meta["id"], "name": meta["name"], "author": "Terminus-Est", "config_version": "2", "version": "1",
        "description": meta["description"], "definition_method": "multi", "soundpack_type": "Keyboard",
        "tags": meta["tags"], "definitions": definitions, "options": {"random_pitch": False, "recommended_volume": 1.0},
    }
    (out / "config.json").write_text(json.dumps(config, indent=2))
    print(f"{len(definitions)} keys, {len(files)} samples -> {out}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=Path.home() / ".local/share/mechvibes/soundpacks/keyboard/piano")
    specials = {  # name -> (samples, keys)
        "chord-c": (chord(["C3", "G3", "E4"]), ["Space"]),
        "low-c": (tone("C2", 1.6), ["Enter", "NumpadEnter"]),
        "low-g": (tone("G1", 1.2), ["Backspace"]),
        "soft-c": (tone("C3", 0.9, 0.35), ["ShiftLeft", "ShiftRight", "ControlLeft", "ControlRight",
                                            "AltLeft", "AltRight", "CapsLock"]),
    }
    meta = {"id": "piano", "name": "Piano", "tags": ["piano", "melodic", "custom"],
            "description": "Synthesised piano: home row is the white keys, the row above the black keys."}
    write_pack(parser.parse_args().out, meta, layout(), tone, specials, 2400)


if __name__ == "__main__":
    main()
