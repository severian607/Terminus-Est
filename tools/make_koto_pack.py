#!/usr/bin/env python3
"""Build the "Koto" Mechvibes keyboard pack (synthesised plucked strings in the hirajoshi scale) and install it.

usage: make_koto_pack.py [--out DIR]     default: ~/.local/share/mechvibes/soundpacks/keyboard/koto

Every key plays a note of the D hirajoshi scale (D E F A Bb), so any typing sounds musical. Each keyboard row
climbs the scale left to right and each row up starts three scale steps higher: Z row lowest, number row
highest. Space is a sweeping glissando, Enter and Backspace are low strings. Samples are additive-synthesis
plucks (pick position comb, fast-decaying upper partials, a small pitch settle, a pick click), generated here.
"""

import argparse
from pathlib import Path

import numpy as np
from make_piano_pack import NAMES, RATE, midi, write_pack

ROOT = "D3"
SCALE = [0, 2, 3, 7, 8]  # hirajoshi: D E F A Bb


def degree(i: int) -> str:
    """Note name of the i-th scale degree above the root (the scale repeats every octave)."""
    semis = midi(ROOT) + 12 * (i // 5) + SCALE[i % 5]
    return f"{NAMES[semis % 12]}{semis // 12 - 1}"


def pluck(note: str, seconds: float = 2.2, gain: float = 1.0) -> np.ndarray:
    f0 = 440 * 2 ** ((midi(note) - 69) / 12)
    t = np.arange(int(RATE * seconds)) / RATE
    rng = np.random.default_rng(midi(note))
    tau0 = np.clip(0.6 + 1.6 * (293.7 / f0) ** 0.5, 0.5, 2.4)
    settle = 1 + 0.0035 * np.exp(-t / 0.06)  # tension makes a fresh pluck a few cents sharp
    out = np.zeros_like(t)
    for n in range(1, 25):
        fn = n * f0 * np.sqrt(1 + 0.00012 * n * n)
        if fn > 10000:
            break
        amp = abs(np.sin(np.pi * n * 0.14)) / n**1.4  # plucked a seventh of the way along the string
        decay = np.exp(-t / (tau0 / (1 + 0.55 * (n - 1))))
        out += amp * decay * np.sin(2 * np.pi * fn * np.cumsum(settle) / RATE + rng.uniform(0, 6.28))
    out *= 1 - np.exp(-t / 0.0007)
    click = rng.standard_normal(len(t)) * np.exp(-t / 0.004)  # the ivory pick
    click = np.convolve(click, np.ones(6) / 6, mode="same") * 0.08
    out += click * np.max(np.abs(out))
    out *= np.clip((seconds - t) / 0.12, 0, 1)
    return out * (0.8 * gain / np.max(np.abs(out)))


def glissando(first: int, count: int, step: float = 0.045, gain: float = 1.0) -> np.ndarray:
    """A sweep up (count > 0) or down the scale, one string every `step` seconds."""
    notes = (
        [degree(first + i) for i in range(count)]
        if count > 0
        else [degree(first + count + 1 - i) for i in range(-count)]
    )
    length = step * len(notes) + 1.8
    mix = np.zeros(int(RATE * length))
    for i, n in enumerate(notes):
        tone = pluck(n, 1.8, 0.7)
        start = int(RATE * step * i)
        mix[start : start + len(tone)] += tone
    return mix * (0.8 * gain / np.max(np.abs(mix)))


# (first scale degree, key codes): each row up starts three scale steps higher.
ROWS = (
    (0, "KeyZ KeyX KeyC KeyV KeyB KeyN KeyM Comma Period Slash"),
    (3, "KeyA KeyS KeyD KeyF KeyG KeyH KeyJ KeyK KeyL Semicolon Quote"),
    (6, "KeyQ KeyW KeyE KeyR KeyT KeyY KeyU KeyI KeyO KeyP BracketLeft BracketRight"),
    (9, "Digit1 Digit2 Digit3 Digit4 Digit5 Digit6 Digit7 Digit8 Digit9 Digit0 Minus Equal"),
)
FUNCTION_KEYS_FIRST_DEGREE = 10
OTHER_KEY_DEGREES = {
    "Backslash": 19,
    "Backquote": -3,
    "Tab": -2,
    "Escape": -5,
    "Delete": -1,
    "Insert": 0,
    "Home": 1,
    "End": 2,
    "PageUp": 4,
    "PageDown": 5,
    "ArrowLeft": 7,
    "ArrowDown": 8,
    "ArrowUp": 11,
    "ArrowRight": 9,
}


def layout() -> dict[str, str]:
    """Key code -> note for every key."""
    keys = {}
    for first, codes in ROWS:
        for i, code in enumerate(codes.split()):
            keys[code] = degree(first + i)
    for i in range(12):
        keys[f"F{i + 1}"] = degree(FUNCTION_KEYS_FIRST_DEGREE + i)
    for code, scale_degree in OTHER_KEY_DEGREES.items():
        keys[code] = degree(scale_degree)
    return keys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=Path.home() / ".local/share/mechvibes/soundpacks/keyboard/koto")
    specials = {
        "sweep-up": (glissando(2, 9), ["Space"]),
        "sweep-down": (glissando(8, -6, 0.05), ["Backspace"]),
        "low-d": (pluck("D2", 2.2), ["Enter", "NumpadEnter"]),
        "soft-d": (
            pluck("D3", 0.9, 0.3),
            ["ShiftLeft", "ShiftRight", "ControlLeft", "ControlRight", "AltLeft", "AltRight", "CapsLock"],
        ),
    }
    meta = {
        "id": "koto",
        "name": "Koto",
        "tags": ["koto", "melodic", "custom"],
        "description": "Synthesised koto: every key plucks a string of the D hirajoshi scale.",
    }
    write_pack(parser.parse_args().out, meta, layout(), pluck, specials, 2200)


if __name__ == "__main__":
    main()
