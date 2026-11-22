"""Audio effect library on top of EasyEffects.

An effect is a TOML file in config/audiofx/. It is compiled into an EasyEffects output preset
(`ama-<name>`): optional convolver (impulse response from config/audiofx/irs), equalizer bands,
stereo_tools, crusher, and a closing limiter. EasyEffects (easyeffects.service, service mode) hosts
the active preset on all system audio; "off" is its global bypass. One preset is active at a time.
"""

import json
import math
import re
import shutil
import subprocess
import time
import tomllib
from pathlib import Path

CATEGORIES = ["Tone", "Dynamics", "Space", "Voice & Pitch", "Filters & Character", "Retro", "Stereo"]
BAND_TYPES = {
    "lowshelf": "Lo-shelf",
    "highshelf": "Hi-shelf",
    "peaking": "Bell",
    "lowpass": "Lo-pass",
    "highpass": "Hi-pass",
    "bandpass": "Bandpass",
    "notch": "Notch",
    "allpass": "Allpass",
}
GAIN_TYPES = {"lowshelf", "highshelf", "peaking"}
STEREO_MODES = {
    "stereo": "LR > LR (Stereo Default)",
    "mono": "LR > L+R (Mono Sum L+R)",
    "swap": "LR > RL (Stereo Flip Channels)",
    "left": "LR > LL (Mono Left Channel)",
    "right": "LR > RR (Mono Right Channel)",
}
# EasyEffects plugins passed through verbatim from [[plugin]] tables (keys as in its preset JSON).
PLUGINS = {
    "crusher",
    "reverb",
    "delay",
    "pitch",
    "exciter",
    "bass_enhancer",
    "crystalizer",
    "crossfeed",
    "compressor",
    "maximizer",
    "bass_loudness",
    "loudness",
    "expander",
    "gate",
    "deesser",
}
KEYS = {
    "label",
    "emoji",
    "category",
    "tags",
    "description",
    "source",
    "license",
    "order",
    "headroom_db",
    "ir",
    "stereo",
    "crusher",
    "band",
    "plugin",
}
STEREO_KEYS = {"mode", "base", "middle_level"}
CRUSHER_KEYS = {"bits", "downsample"}
PREFIX = "ama-"
DEFAULT_ORDER = 500
MAX_BANDS = 32
BAND_FREQ_MIN_HZ, BAND_FREQ_MAX_HZ = 10, 20000
DEFAULT_BAND_Q = 0.707  # Butterworth
# The EasyEffects equalizer mode and slope every band uses.
BAND_FILTER = {"mode": "RLC (BT)", "slope": "x2", "width": 4.0, "mute": False, "solo": False}
LIMITER_THRESHOLD_DB = -1.0
COMMAND_TIMEOUT_SECONDS = 15
SERVICE_START_TIMEOUT_SECONDS = 10
SERVICE_POLL_SECONDS = 0.2

# User equalizer: ten fixed Bell bands.
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

PROJECT = Path(__file__).resolve().parents[1]
EFFECT_DIR = PROJECT / "config/audiofx"
IR_DIR = EFFECT_DIR / "irs"
EE_DIR = Path.home() / ".local/share/easyeffects"
EQ_PATH = Path.home() / ".local/state/terminus-est/audiofx/eq.json"

NAME_RE = re.compile(r"^[a-z0-9-]+$")


class FxError(Exception):
    pass


# Preset building blocks


def eq_sides(bands):
    """The per-channel `bandN` entries of an EasyEffects equalizer (both channels share them)."""
    return {f"band{index}": {**band, **BAND_FILTER} for index, band in enumerate(bands)}


def equalizer_plugin(bands, input_gain):
    side = eq_sides(bands)
    return {
        "bypass": False,
        "mode": "IIR",
        "num-bands": len(bands),
        "split-channels": False,
        "input-gain": input_gain,
        "output-gain": 0.0,
        "balance": 0.0,
        "left": side,
        "right": side,
    }


def convolver_plugin(impulse_response):
    return {
        "bypass": False,
        "autogain": False,
        "kernel-name": impulse_response,
        "ir-width": 100,
        "dry": -100.0,
        "wet": 0.0,
        "input-gain": 0.0,
        "output-gain": 0.0,
        "sofa": {"azimuth": 0.0, "elevation": 0.0, "radius": 1.0},
    }


def limiter_plugin(input_gain):
    return {"bypass": False, "threshold": LIMITER_THRESHOLD_DB, "output-gain": 0.0, "input-gain": input_gain}


def preset_document(plugins):
    return {"output": {"blocklist": [], "plugins_order": list(plugins), **plugins}}


# Effects


def read_effect_file(path):
    """Parse one effect TOML and check the keys every effect needs."""
    if not NAME_RE.match(path.stem):
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
    return data


def check_band(path, band):
    """A validated EasyEffects band entry for one [[band]] table."""
    kind = band.get("type")
    if kind not in BAND_TYPES:
        raise FxError(f"{path.name}: band type must be one of {sorted(BAND_TYPES)}")
    freq, q, gain = float(band.get("freq", 0)), float(band.get("q", DEFAULT_BAND_Q)), float(band.get("gain", 0))
    if not BAND_FREQ_MIN_HZ <= freq <= BAND_FREQ_MAX_HZ:
        raise FxError(f"{path.name}: band freq {freq} outside {BAND_FREQ_MIN_HZ}..{BAND_FREQ_MAX_HZ} Hz")
    if q <= 0:
        raise FxError(f"{path.name}: band q must be positive")
    if gain and kind not in GAIN_TYPES:
        raise FxError(f"{path.name}: {kind} bands have no gain")
    return {"type": BAND_TYPES[kind], "frequency": freq, "q": q, "gain": gain}


class Effect:
    def __init__(self, path):
        data = read_effect_file(path)
        self.name = path.stem
        self.label, self.emoji = data["label"], data["emoji"]
        self.category, self.description = data["category"], data["description"]
        self.tags = [str(tag) for tag in data.get("tags", [])]
        self.source = data.get("source", "original")
        self.license = data.get("license", "original")
        self.order = int(data.get("order", DEFAULT_ORDER))
        self.headroom_db = float(data.get("headroom_db", 0))
        self.ir = data.get("ir")
        if self.ir is not None and not (IR_DIR / f"{self.ir}.wav").is_file():
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
        self.bands = [check_band(path, band) for band in data.get("band", [])]
        self.plugins = [dict(plugin) for plugin in data.get("plugin", [])]
        for plugin in self.plugins:
            if plugin.get("type") not in PLUGINS:
                raise FxError(f"{path.name}: plugin type must be one of {sorted(PLUGINS)}")
        if len(self.bands) > MAX_BANDS:
            raise FxError(f"{path.name}: at most {MAX_BANDS} bands")
        if not (self.ir or self.stereo or self.crusher or self.bands or self.plugins):
            raise FxError(f"{path.name}: effect does nothing")

    def sort_key(self):
        return (CATEGORIES.index(self.category), self.order, self.label.lower())

    def preset(self):
        """The EasyEffects output preset JSON for this effect.

        The first plugin that is present takes the headroom as input gain, so boosts never clip;
        a closing limiter carries it when none of them is.
        """
        headroom = min(self.headroom_db, 0.0)
        plugins = {}
        if self.ir:
            plugins["convolver#0"] = convolver_plugin(self.ir)
        if self.bands:
            plugins["equalizer#0"] = equalizer_plugin(self.bands, headroom)
        if self.stereo:
            plugins["stereo_tools#0"] = {
                "bypass": False,
                "mode": STEREO_MODES[self.stereo.get("mode", "stereo")],
                "stereo-base": float(self.stereo.get("base", 0.0)),
                "middle-level": float(self.stereo.get("middle_level", 0.0)),
                "input-gain": 0.0 if self.bands else headroom,
            }
        if self.crusher:
            plugins["crusher#0"] = {
                "bypass": False,
                "bit-reduction": float(self.crusher.get("bits", 16)),
                "sample-reduction": float(self.crusher.get("downsample", 1)),
                "mode": "Linear",
                "input-gain": 0.0 if (self.bands or self.stereo) else headroom,
            }
        counts = {}
        for plugin in self.plugins:
            kind = plugin["type"]
            settings = {key: value for key, value in plugin.items() if key != "type"}
            plugins[f"{kind}#{counts.get(kind, 0)}"] = {"bypass": False, **settings}
            counts[kind] = counts.get(kind, 0) + 1
        carried = bool(self.bands or self.stereo or self.crusher)
        plugins["limiter#0"] = limiter_plugin(0.0 if carried else headroom)
        return preset_document(plugins)


def load_effects():
    """All effects keyed by name, in category/order/label order."""
    effects = sorted((Effect(path) for path in EFFECT_DIR.glob("*.toml")), key=Effect.sort_key)
    return {effect.name: effect for effect in effects}


# EasyEffects


def run(*cmd, check=True):
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=COMMAND_TIMEOUT_SECONDS)
    except (OSError, subprocess.SubprocessError) as err:
        raise FxError(f"{cmd[0]}: {err}") from None
    if check and result.returncode:
        raise FxError(f"{' '.join(cmd[:3])}: {result.stderr.strip() or f'exit {result.returncode}'}")
    return result.stdout.strip()


def ee(*args):
    if not shutil.which("easyeffects"):
        raise FxError("easyeffects is not installed (sudo pacman -S easyeffects lsp-plugins-lv2 calf)")
    return run("easyeffects", *args)


def service_active():
    return run("systemctl", "--user", "is-active", "easyeffects.service", check=False) == "active"


def ensure_service():
    if service_active():
        return
    run("systemctl", "--user", "start", "easyeffects.service")
    deadline = time.monotonic() + SERVICE_START_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        if "ee_soe_output_level:output" in run("pw-link", "-o", check=False):
            break
        time.sleep(SERVICE_POLL_SECONDS)


def stop_service():
    run("systemctl", "--user", "stop", "easyeffects.service")


def sync(effects, extra=None):
    """Install every effect (plus `extra` name->JSON presets) as EasyEffects presets, and its IRs.

    Rewrites only what changed."""
    out, irs = EE_DIR / "output", EE_DIR / "irs"
    out.mkdir(parents=True, exist_ok=True)
    irs.mkdir(parents=True, exist_ok=True)
    wanted = {
        out / f"{PREFIX}{effect.name}.json": json.dumps(effect.preset(), indent=2) + "\n" for effect in effects.values()
    }
    for name, text in (extra or {}).items():
        wanted[out / f"{name}.json"] = text
    for path, text in wanted.items():
        if not path.exists() or path.read_text() != text:
            path.write_text(text)
    for stale in out.glob(f"{PREFIX}*.json"):
        if stale not in wanted:
            stale.unlink()
    for ir in IR_DIR.glob("*.wav"):
        dest = irs / f"{ir.stem}.irs"  # EasyEffects only scans .irs; the content is a plain WAV
        if not dest.exists() or dest.read_bytes() != ir.read_bytes():
            shutil.copyfile(ir, dest)


def bypassed():
    """EasyEffects is off by default: a stopped service counts as bypassed."""
    return not service_active() or ee("--bypass", "3").strip() == "1"


def loaded_effect(effects):
    """Name of the effect in the last loaded preset (with or without the user EQ), or None."""
    loaded = ee("--last-loaded-preset", "output").strip()
    name = loaded.removeprefix(PREFIX) if loaded.startswith(PREFIX) else None
    if name and name.startswith("eq+"):
        name = name[3:]
    return name if name in effects else None


def current(effects=None):
    """Name of the active effect, or None when nothing is loaded, only the EQ runs, or EasyEffects is bypassed."""
    effects = effects if effects is not None else load_effects()
    return None if bypassed() else loaded_effect(effects)


def read_stack(effects=None):
    name = current(effects)
    return [name] if name else []


def build_stack(current_stack, names, effects, mode):
    """New stack after `mode` (set|toggle|remove); EasyEffects hosts one preset at a time."""
    known(names, effects)
    if mode == "remove":
        return [name for name in current_stack if name not in names]
    if mode == "toggle" and names and current_stack == [names[-1]]:
        return []
    return [names[-1]] if names else []


def known(names, effects):
    for name in names:
        if name not in effects:
            raise FxError(f"unknown effect '{name}'")


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
    effects = effects if effects is not None else load_effects()
    state = eq_load()
    is_bypassed = bypassed()  # one query: current() would repeat it
    name = None if is_bypassed else loaded_effect(effects)
    return {
        "active": [name] if name else [],
        "bypassed": is_bypassed,
        "eq": {"enabled": state["enabled"], "preset": state["preset"]},
    }


# User equalizer


def eq_clamp(value):
    try:
        gain = float(value)
    except (TypeError, ValueError):
        raise FxError(f"invalid gain '{value}'") from None
    if math.isnan(gain):
        raise FxError("invalid gain 'nan'")
    return round(max(EQ_MIN, min(EQ_MAX, gain)), 2)


def eq_load():
    """Saved EQ state; a missing or unreadable file means the default (off, flat)."""
    state = {"enabled": False, "preset": "flat", "gains": [0.0] * len(EQ_FREQS)}
    try:
        data = json.loads(EQ_PATH.read_text())
        gains = [eq_clamp(gain) for gain in data["gains"]]
        if len(gains) == len(EQ_FREQS):
            state = {"enabled": bool(data["enabled"]), "preset": str(data["preset"]), "gains": gains}
    except (OSError, ValueError, KeyError, TypeError, FxError):
        pass  # the default state is the documented answer for any unusable file
    return state


def eq_save(state):
    EQ_PATH.parent.mkdir(parents=True, exist_ok=True)
    temporary = EQ_PATH.with_name(EQ_PATH.name + ".tmp")
    temporary.write_text(json.dumps(state, indent=2) + "\n")
    temporary.replace(EQ_PATH)


def eq_status():
    state = eq_load()
    return {
        "enabled": state["enabled"],
        "min": EQ_MIN,
        "max": EQ_MAX,
        "preset": state["preset"],
        "presets": list(EQ_PRESETS),
        "bands": [{"hz": hz, "gain": gain} for hz, gain in zip(EQ_FREQS, state["gains"])],
    }


def eq_stage(gains):
    """The user's equalizer plugin: ten Bell bands, input gain lowered by the largest boost."""
    bands = [
        {"type": BAND_TYPES["peaking"], "frequency": float(hz), "q": EQ_Q, "gain": float(gain)}
        for hz, gain in zip(EQ_FREQS, gains)
    ]
    return equalizer_plugin(bands, -max(0.0, *gains))


def eq_preset(gains, effect=None):
    """Preset JSON: the user EQ first, then the effect's own plugins (or just the limiter)."""
    plugins = dict(effect.preset()["output"]) if effect else {}
    plugins.pop("blocklist", None)
    plugins.pop("plugins_order", None)
    if not effect:
        plugins["limiter#0"] = limiter_plugin(0.0)
    plugins = {("equalizer#1" if "equalizer#0" in plugins else "equalizer#0"): eq_stage(gains), **plugins}
    return preset_document(plugins)


def eq_update(enabled=None, preset=None, gains=None):
    """Persist the new EQ state."""
    state = eq_load()
    if gains is not None:
        state["gains"] = [eq_clamp(gain) for gain in gains]
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
        position = int(index)
    except ValueError:
        raise FxError(f"invalid band '{index}'") from None
    if not 1 <= position <= len(EQ_FREQS):
        raise FxError(f"band index must be 1..{len(EQ_FREQS)}")
    gains = eq_load()["gains"]
    gains[position - 1] = eq_clamp(gain)
    eq_update(True, "custom", gains)


def eq_preset_set(name):
    if name not in EQ_PRESETS:
        raise FxError(f"unknown EQ preset '{name}' (known: {', '.join(EQ_PRESETS)})")
    eq_update(True, name, EQ_PRESETS[name])


def eq_reset():
    eq_update(None, "flat", EQ_PRESETS["flat"])


# Listing and validation


def list_effects(effects=None):
    effects = effects if effects is not None else load_effects()
    stack = read_stack(effects)
    rows = []
    for effect in effects.values():
        flags = [
            flag
            for flag, on in (
                ("ir", effect.ir),
                ("eq", effect.bands),
                ("stereo", effect.stereo),
                ("crusher", effect.crusher),
            )
            if on
        ]
        flags += sorted({plugin["type"].replace("_", "-") for plugin in effect.plugins})
        rows.append(
            {
                "name": effect.name,
                "label": effect.label,
                "emoji": effect.emoji,
                "category": effect.category,
                "tags": effect.tags,
                "description": effect.description,
                "source": effect.source,
                "license": effect.license,
                "order": effect.order,
                "flags": flags,
                "active": effect.name in stack,
                "position": stack.index(effect.name) + 1 if effect.name in stack else None,
                "shadowed": False,
            }
        )
    return {"categories": CATEGORIES, "effects": rows}


def check(names, effects=None):
    """Compile each effect to a preset and check the JSON round-trips; returns (ok, messages)."""
    effects = effects if effects is not None else load_effects()
    names = list(names) or list(effects)
    known(names, effects)
    for name in names:
        json.loads(json.dumps(effects[name].preset()))
    return True, [f"{name}: ok" for name in names]
