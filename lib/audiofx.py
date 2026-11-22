"""Audio effect library on top of EasyEffects.

An effect is a TOML file in config/audiofx/. It is compiled into an EasyEffects output preset
(`ama-<name>`): optional convolver (impulse response from config/audiofx/irs), equalizer bands,
stereo_tools, crusher, and a closing limiter. EasyEffects (easyeffects.service, service mode) hosts
the active preset on all system audio; "off" is its global bypass. One preset is active at a time.
"""

import json
from pathlib import Path
import re
import shutil
import subprocess
import time
import tomllib

CATEGORIES = ["Tone", "Dynamics", "Space", "Voice & Pitch", "Filters & Character", "Retro", "Stereo"]
BAND_TYPES = {
    "lowshelf": "Lo-shelf", "highshelf": "Hi-shelf", "peaking": "Bell", "lowpass": "Lo-pass",
    "highpass": "Hi-pass", "bandpass": "Bandpass", "notch": "Notch", "allpass": "Allpass",
}
GAIN_TYPES = {"lowshelf", "highshelf", "peaking"}
STEREO_MODES = {
    "stereo": "LR > LR (Stereo Default)", "mono": "LR > L+R (Mono Sum L+R)", "swap": "LR > RL (Stereo Flip Channels)",
    "left": "LR > LL (Mono Left Channel)", "right": "LR > RR (Mono Right Channel)",
}
# EasyEffects plugins passed through verbatim from [[plugin]] tables (keys as in its preset JSON).
PLUGINS = {"crusher", "reverb", "delay", "pitch", "exciter", "bass_enhancer", "crystalizer", "crossfeed", "compressor",
           "maximizer", "bass_loudness", "loudness", "expander", "gate", "deesser"}
KEYS = {"label", "emoji", "category", "tags", "description", "source", "license", "order",
        "headroom_db", "ir", "stereo", "crusher", "band", "plugin"}
STEREO_KEYS = {"mode", "base", "middle_level"}
CRUSHER_KEYS = {"bits", "downsample"}
PREFIX = "ama-"

project = Path(__file__).resolve().parents[1]
effect_dir = project / "config/audiofx"
ir_dir = effect_dir / "irs"
ee_dir = Path.home() / ".local/share/easyeffects"

name_re = re.compile(r"^[a-z0-9-]+$")


class FxError(Exception):
    pass


def eq_sides(bands):
    """The per-channel `bandN` entries of an EasyEffects equalizer (both channels share them)."""
    return {f"band{i}": {**b, "mode": "RLC (BT)", "slope": "x2", "width": 4.0, "mute": False, "solo": False}
            for i, b in enumerate(bands)}


class Effect:
    def __init__(self, path):
        self.name = path.stem
        if not name_re.match(self.name):
            raise FxError(f"{path.name}: effect names are lowercase letters, digits and hyphens")
        try:
            data = tomllib.loads(path.read_text())
        except tomllib.TOMLDecodeError as err:
            raise FxError(f"{path.name}: {err}") from None
        unknown = set(data) - KEYS
        if unknown:
            raise FxError(f"{path.name}: unknown keys {sorted(unknown)}")
        for key in ("label", "emoji", "category", "description"):
            if not isinstance(data.get(key), str) or not data[key]:
                raise FxError(f"{path.name}: '{key}' is required")
        if data["category"] not in CATEGORIES:
            raise FxError(f"{path.name}: category must be one of {CATEGORIES}")
        self.label, self.emoji = data["label"], data["emoji"]
        self.category, self.description = data["category"], data["description"]
        self.tags = [str(t) for t in data.get("tags", [])]
        self.source = data.get("source", "original")
        self.license = data.get("license", "original")
        self.order = int(data.get("order", 500))
        self.headroom_db = float(data.get("headroom_db", 0))
        self.ir = data.get("ir")
        if self.ir is not None and not (ir_dir / f"{self.ir}.wav").is_file():
            raise FxError(f"{path.name}: missing impulse response irs/{self.ir}.wav")
        self.stereo = data.get("stereo")
        if self.stereo is not None:
            if set(self.stereo) - STEREO_KEYS:
                raise FxError(f"{path.name}: stereo keys are {sorted(STEREO_KEYS)}")
            if self.stereo.get("mode", "stereo") not in STEREO_MODES:
                raise FxError(f"{path.name}: stereo mode must be one of {sorted(STEREO_MODES)}")
        self.crusher = data.get("crusher")
        if self.crusher is not None and set(self.crusher) - CRUSHER_KEYS:
            raise FxError(f"{path.name}: crusher keys are {sorted(CRUSHER_KEYS)}")
        self.bands = [self.band(path, b) for b in data.get("band", [])]
        self.plugins = [dict(p) for p in data.get("plugin", [])]
        for p in self.plugins:
            if p.get("type") not in PLUGINS:
                raise FxError(f"{path.name}: plugin type must be one of {sorted(PLUGINS)}")
        if len(self.bands) > 32:
            raise FxError(f"{path.name}: at most 32 bands")
        if not (self.ir or self.stereo or self.crusher or self.bands or self.plugins):
            raise FxError(f"{path.name}: effect does nothing")

    @staticmethod
    def band(path, b):
        kind = b.get("type")
        if kind not in BAND_TYPES:
            raise FxError(f"{path.name}: band type must be one of {sorted(BAND_TYPES)}")
        freq, q, gain = float(b.get("freq", 0)), float(b.get("q", 0.707)), float(b.get("gain", 0))
        if not 10 <= freq <= 20000:
            raise FxError(f"{path.name}: band freq {freq} outside 10..20000 Hz")
        if q <= 0:
            raise FxError(f"{path.name}: band q must be positive")
        if gain and kind not in GAIN_TYPES:
            raise FxError(f"{path.name}: {kind} bands have no gain")
        return {"type": BAND_TYPES[kind], "frequency": freq, "q": q, "gain": gain}

    def sort_key(self):
        return (CATEGORIES.index(self.category), self.order, self.label.lower())

    # # EasyEffects preset
    def preset(self):
        """The EasyEffects output preset JSON for this effect."""
        plugins = {}
        if self.ir:
            plugins["convolver#0"] = {
                "bypass": False, "autogain": False, "kernel-name": self.ir, "ir-width": 100,
                "dry": -100.0, "wet": 0.0, "input-gain": 0.0, "output-gain": 0.0,
                "sofa": {"azimuth": 0.0, "elevation": 0.0, "radius": 1.0},
            }
        if self.bands:
            side = eq_sides(self.bands)
            plugins["equalizer#0"] = {
                "bypass": False, "mode": "IIR", "num-bands": len(self.bands), "split-channels": False,
                "input-gain": min(self.headroom_db, 0.0), "output-gain": 0.0, "balance": 0.0,
                "left": side, "right": side,
            }
        if self.stereo:
            s = self.stereo
            plugins["stereo_tools#0"] = {
                "bypass": False, "mode": STEREO_MODES[s.get("mode", "stereo")],
                "stereo-base": float(s.get("base", 0.0)), "middle-level": float(s.get("middle_level", 0.0)),
                "input-gain": min(self.headroom_db, 0.0) if not self.bands else 0.0,
            }
        if self.crusher:
            c = self.crusher
            plugins["crusher#0"] = {
                "bypass": False, "bit-reduction": float(c.get("bits", 16)),
                "sample-reduction": float(c.get("downsample", 1)), "mode": "Linear",
                "input-gain": min(self.headroom_db, 0.0) if not (self.bands or self.stereo) else 0.0,
            }
        counts = {}
        for p in self.plugins:
            kind = p["type"]
            plugins[f"{kind}#{counts.get(kind, 0)}"] = {"bypass": False, **{k: v for k, v in p.items() if k != "type"}}
            counts[kind] = counts.get(kind, 0) + 1
        # A final limiter keeps any stack of boosts from clipping; it also carries the headroom when no
        # earlier plugin did.
        carried = bool(self.bands or self.stereo or self.crusher)
        plugins["limiter#0"] = {"bypass": False, "threshold": -1.0, "output-gain": 0.0,
                                "input-gain": 0.0 if carried else min(self.headroom_db, 0.0)}
        return {"output": {"blocklist": [], "plugins_order": list(plugins), **plugins}}


def load_effects():
    """All effects keyed by name, in category/order/label order."""
    effects = sorted((Effect(p) for p in effect_dir.glob("*.toml")), key=Effect.sort_key)
    return {e.name: e for e in effects}


# # EasyEffects
def run(*cmd, check=True):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.SubprocessError) as err:
        raise FxError(f"{cmd[0]}: {err}") from None
    if check and r.returncode:
        raise FxError(f"{' '.join(cmd[:3])}: {r.stderr.strip() or f'exit {r.returncode}'}")
    return r.stdout.strip()


def ee(*args):
    if not shutil.which("easyeffects"):
        raise FxError("easyeffects is not installed (sudo pacman -S easyeffects lsp-plugins-lv2 calf)")
    return run("easyeffects", *args)


def service_active():
    return run("systemctl", "--user", "is-active", "easyeffects.service", check=False) == "active"


def ensure_service():
    if not service_active():
        run("systemctl", "--user", "start", "easyeffects.service")
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if run("pw-link", "-o", check=False).find("ee_soe_output_level:output") >= 0:
                break
            time.sleep(0.2)


def stop_service():
    run("systemctl", "--user", "stop", "easyeffects.service")


def sync(effects, extra=None):
    """Install every effect (plus `extra` name->JSON presets) as EasyEffects presets, and its IRs; rewrites only what changed."""
    out, irs = ee_dir / "output", ee_dir / "irs"
    out.mkdir(parents=True, exist_ok=True)
    irs.mkdir(parents=True, exist_ok=True)
    wanted = {}
    for e in effects.values():
        wanted[out / f"{PREFIX}{e.name}.json"] = json.dumps(e.preset(), indent=2) + "\n"
    for name, text in (extra or {}).items():
        wanted[out / f"{name}.json"] = text
    for path, text in wanted.items():
        if not path.exists() or path.read_text() != text:
            path.write_text(text)
    for stale in out.glob(f"{PREFIX}*.json"):
        if stale not in wanted:
            stale.unlink()
    for ir in ir_dir.glob("*.wav"):
        dest = irs / f"{ir.stem}.irs"  # EasyEffects only scans .irs; the content is a plain WAV
        if not dest.exists() or dest.read_bytes() != ir.read_bytes():
            shutil.copyfile(ir, dest)


def bypassed():
    """EasyEffects is off by default: a stopped service counts as bypassed."""
    return not service_active() or ee("--bypass", "3").strip() == "1"


def current(effects=None):
    """Name of the active effect, or None when nothing is loaded, only the EQ runs, or EasyEffects is bypassed."""
    effects = effects if effects is not None else load_effects()
    if bypassed():
        return None
    loaded = ee("--last-loaded-preset", "output").strip()
    name = loaded[len(PREFIX):] if loaded.startswith(PREFIX) else None
    if name and name.startswith("eq+"):
        name = name[3:]
    return name if name in effects else None


def read_stack(effects=None):
    name = current(effects)
    return [name] if name else []


def build_stack(current_stack, names, effects, mode):
    """New stack after `mode` (set|toggle|remove); EasyEffects hosts one preset at a time."""
    known(names, effects)
    if mode == "remove":
        return [n for n in current_stack if n not in names]
    if mode == "toggle" and names and current_stack == [names[-1]]:
        return []
    return [names[-1]] if names else []


def known(names, effects):
    for n in names:
        if n not in effects:
            raise FxError(f"unknown effect '{n}'")


def eq_name(name=None):
    """EasyEffects preset name for the user EQ alone (name None) or composed with effect `name`."""
    return f"{PREFIX}eq+{name or ''}"


def apply(stack, effects=None):
    """Load the effect in `stack` (at most one) together with the user EQ when it is enabled.

    With neither an effect nor the EQ, EasyEffects stops so audio goes straight to the device."""
    effects = effects if effects is not None else load_effects()
    known(stack, effects)
    if len(stack) > 1:
        raise FxError("EasyEffects runs one preset at a time")
    state = eq_load()
    use_eq = state["enabled"]
    if not stack and not use_eq:
        stop_service()
        return
    ensure_service()
    extra = {}
    if use_eq:
        effect = effects[stack[0]] if stack else None
        extra[eq_name(stack[0] if stack else None)] = json.dumps(eq_preset(state["gains"], effect), indent=2) + "\n"
    sync(effects, extra)
    ee("--bypass", "2")
    ee("--load-preset", next(iter(extra)) if use_eq else PREFIX + stack[0])


def status(effects=None):
    state = eq_load()
    return {"active": read_stack(effects), "bypassed": bypassed(),
            "eq": {"enabled": state["enabled"], "preset": state["preset"]}}


# # User equalizer
EQ_FREQS = [32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 16000]
EQ_MIN, EQ_MAX, EQ_Q = -12.0, 12.0, 1.41
EQ_PRESETS = {
    "flat": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    "bass": [6, 5, 4, 2, 0, 0, 0, 0, 0, 0],
    "treble": [0, 0, 0, 0, 0, 1, 2, 4, 5, 6],
    "vocal": [-2, -2, -1, 1, 3, 4, 3, 1, 0, -1],
    "v-shape": [5, 4, 2, 0, -2, -2, 0, 2, 4, 5],
    "loudness": [6, 4, 1, 0, -1, -1, 0, 1, 4, 5],
    "warm": [3, 3, 2, 2, 1, 0, -1, -2, -3, -4],
    "bright": [-3, -2, -1, 0, 0, 1, 2, 4, 5, 6],
}
eq_path = Path.home() / ".local/state/terminus-est/audiofx/eq.json"


def eq_clamp(value):
    try:
        gain = float(value)
    except (TypeError, ValueError):
        raise FxError(f"invalid gain '{value}'") from None
    if gain != gain:
        raise FxError("invalid gain 'nan'")
    return round(max(EQ_MIN, min(EQ_MAX, gain)), 2)


def eq_load():
    """Saved EQ state; a missing or unreadable file means the default (off, flat)."""
    state = {"enabled": False, "preset": "flat", "gains": [0.0] * len(EQ_FREQS)}
    try:
        data = json.loads(eq_path.read_text())
        gains = [eq_clamp(g) for g in data["gains"]]
        if len(gains) == len(EQ_FREQS):
            state = {"enabled": bool(data["enabled"]), "preset": str(data["preset"]), "gains": gains}
    except (OSError, ValueError, KeyError, TypeError, FxError):
        pass
    return state


def eq_save(state):
    eq_path.parent.mkdir(parents=True, exist_ok=True)
    tmp = eq_path.with_name(eq_path.name + ".tmp")
    tmp.write_text(json.dumps(state, indent=2) + "\n")
    tmp.replace(eq_path)


def eq_status():
    state = eq_load()
    return {"enabled": state["enabled"], "min": EQ_MIN, "max": EQ_MAX, "preset": state["preset"],
            "presets": list(EQ_PRESETS),
            "bands": [{"hz": hz, "gain": g} for hz, g in zip(EQ_FREQS, state["gains"])]}


def eq_stage(gains):
    """The user's equalizer plugin: ten Bell bands, input gain lowered by the largest boost."""
    side = eq_sides([{"type": BAND_TYPES["peaking"], "frequency": float(hz), "q": EQ_Q, "gain": float(g)}
                     for hz, g in zip(EQ_FREQS, gains)])
    return {"bypass": False, "mode": "IIR", "num-bands": len(EQ_FREQS), "split-channels": False,
            "input-gain": -max(0.0, *gains), "output-gain": 0.0, "balance": 0.0, "left": side, "right": side}


def eq_preset(gains, effect=None):
    """Preset JSON: the user EQ first, then the effect's own plugins (or just the limiter)."""
    plugins = dict(effect.preset()["output"]) if effect else {}
    plugins.pop("blocklist", None)
    plugins.pop("plugins_order", None)
    if not effect:
        plugins["limiter#0"] = {"bypass": False, "threshold": -1.0, "output-gain": 0.0, "input-gain": 0.0}
    plugins = {("equalizer#1" if "equalizer#0" in plugins else "equalizer#0"): eq_stage(gains), **plugins}
    return {"output": {"blocklist": [], "plugins_order": list(plugins), **plugins}}


def eq_update(enabled=None, preset=None, gains=None):
    """Persist the new EQ state."""
    state = eq_load()
    if gains is not None:
        state["gains"] = [eq_clamp(g) for g in gains]
    state["preset"] = preset or state["preset"]
    if enabled is not None:
        state["enabled"] = enabled
    eq_save(state)


def eq_set(gains):
    if len(gains) != len(EQ_FREQS):
        raise FxError(f"eq set needs {len(EQ_FREQS)} gains")
    eq_update(True, "custom", gains)


def eq_band(index, gain):
    try:
        i = int(index)
    except ValueError:
        raise FxError(f"invalid band '{index}'") from None
    if not 1 <= i <= len(EQ_FREQS):
        raise FxError(f"band index must be 1..{len(EQ_FREQS)}")
    gains = eq_load()["gains"]
    gains[i - 1] = eq_clamp(gain)
    eq_update(True, "custom", gains)


def eq_preset_set(name):
    if name not in EQ_PRESETS:
        raise FxError(f"unknown EQ preset '{name}' (known: {', '.join(EQ_PRESETS)})")
    eq_update(True, name, EQ_PRESETS[name])


def eq_reset():
    eq_update(None, "flat", EQ_PRESETS["flat"])


def list_effects(effects=None):
    effects = effects if effects is not None else load_effects()
    stack = read_stack(effects)
    rows = []
    for e in effects.values():
        flags = [f for f, on in (("ir", e.ir), ("eq", e.bands), ("stereo", e.stereo), ("crusher", e.crusher)) if on]
        flags += sorted({p["type"].replace("_", "-") for p in e.plugins})
        rows.append({
            "name": e.name, "label": e.label, "emoji": e.emoji, "category": e.category,
            "tags": e.tags, "description": e.description, "source": e.source, "license": e.license,
            "order": e.order, "flags": flags,
            "active": e.name in stack,
            "position": stack.index(e.name) + 1 if e.name in stack else None,
            "shadowed": False,
        })
    return {"categories": CATEGORIES, "effects": rows}


# # Validation
def check(names, effects=None):
    """Compile each effect to a preset and check the JSON round-trips; returns (ok, messages)."""
    effects = effects if effects is not None else load_effects()
    names = list(names) or list(effects)
    known(names, effects)
    for n in names:
        json.loads(json.dumps(effects[n].preset()))
    return True, [f"{n}: ok" for n in names]
