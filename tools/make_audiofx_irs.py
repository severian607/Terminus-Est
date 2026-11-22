#!/usr/bin/env python3
"""Generate the synthetic impulse responses in config/audiofx/irs/ (needs numpy).

Each IR is a linear-phase FIR built from a target magnitude curve of an old playback device, plus an
optional short damped resonance for the cabinet. Re-run after editing CURVES; output is committed.
"""

import wave
from pathlib import Path

import numpy as np

RATE = 48000
TAPS = 4096
OUT = Path(__file__).resolve().parents[1] / "config/audiofx/irs"
SPEECH_BAND_HZ = (300, 3000)  # IRs are normalised to unity average gain across it
RING_DECAYS = 6  # the cabinet resonance is rendered for this many decay times

# name: ((freq Hz, gain dB) ..., (ring Hz, ring dB, decay ms) or None)
# fmt: off
CURVES = {
    "old-radio": (((20, -80), (80, -60), (200, -30), (300, -12), (500, -3), (1000, 3), (2000, 0),
                   (3000, -4), (3500, -12), (5000, -35), (8000, -60), (24000, -80)), (1100, -14, 4)),
    "gramophone": (((20, -80), (100, -60), (200, -24), (400, -6), (700, 2), (1200, 6), (1800, 0),
                    (2500, -8), (3500, -24), (5000, -50), (24000, -80)), (900, -10, 6)),
    "vintage-speaker": (((20, -60), (40, -40), (70, -18), (120, -3), (250, 2), (400, 4), (1000, 0),
                         (3000, -2), (6000, -9), (10000, -24), (16000, -45), (24000, -60)), (320, -16, 8)),
    "crt-tv": (((20, -70), (80, -40), (150, -12), (300, -3), (1000, 0), (3000, 4), (5000, 1),
                (7000, -10), (9000, -30), (12000, -55), (24000, -70)), (4200, -18, 3)),
}
# fmt: on


def build(points: tuple, ring: tuple | None) -> np.ndarray:
    """Linear-phase FIR from a magnitude curve (freq Hz, gain dB), plus the optional damped resonance."""
    freqs = np.fft.rfftfreq(TAPS, 1 / RATE)
    px, py = zip(*points)
    gain = 10 ** (np.interp(np.log10(np.maximum(freqs, 1.0)), np.log10(px), py) / 20)
    ir = np.roll(np.fft.irfft(gain, TAPS), TAPS // 2) * np.hanning(TAPS)
    if ring:
        hz, db, decay_ms = ring
        t = np.arange(int(RATE * decay_ms * RING_DECAYS / 1000)) / RATE
        tail = np.sin(2 * np.pi * hz * t) * np.exp(-t / (decay_ms / 1000)) * 10 ** (db / 20) * ir.max()
        tail = tail[: TAPS // 2]
        ir[TAPS // 2 : TAPS // 2 + len(tail)] += tail
    mag = np.abs(np.fft.rfft(ir))
    low_hz, high_hz = SPEECH_BAND_HZ
    band = (freqs >= low_hz) & (freqs <= high_hz)
    # Peaks may exceed unity; the limiter catches them.
    return ir / mag[band].mean()


def write(name: str, ir: np.ndarray) -> None:
    pcm = (np.clip(ir, -1, 1) * 32767).astype("<i2")
    with wave.open(str(OUT / f"{name}.wav"), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(np.repeat(pcm[:, None], 2, axis=1).tobytes())


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    for name, (points, ring) in CURVES.items():
        write(name, build(points, ring))
        print(name)
