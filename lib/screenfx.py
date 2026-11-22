"""Screen shader engine: effect library, composition into one Hyprland screen shader, state files.

Hyprland has a single `decoration:screen_shader` slot, so the active effects (the "stack") are
composed into one generated shader. Effect file format and composition rules: config/shaders/*.frag.
"""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

CATEGORIES = [
    "Retro · CRT",
    "Retro · Tape & Film",
    "Retro · Pixel & Palette",
    "Glitch",
    "Color · Grade",
    "Color · Mono & Tint",
    "Color · Vision",
    "Comfort",
    "Distortion · Lens",
    "Distortion · Water",
    "Psychedelic",
    "Art · Paint & Ink",
    "Art · Tiles & Shapes",
    "Art · Line",
    "Light & Finish",
    "Overlay",
]
# Category prefixes the panel shows as one parent row with the "Prefix · Name" categories nested under it.
GROUPS = ["Retro", "Color", "Distortion", "Art"]
HEADER_KEYS = ["label", "emoji", "category", "tags", "description", "source", "license", "order", "animated", "samples"]

project = Path(__file__).resolve().parents[1]
shader_dir = project / "config/shaders"
state_dir = Path.home() / ".local/state/terminus-est/toggles/hypr"
stack_file = state_dir / "screen-fx.stack"
frag_file = state_dir / "screen-fx.frag"
damage_file = state_dir / "screen-fx.damage"
obsolete_flag = state_dir / "screen-fx"

PROLOGUE = (
    "#version 300 es\n"
    "precision highp float;\n"
    "in vec2 v_texcoord;\n"
    "uniform sampler2D tex;\n"
    "uniform float time;\n"
    "out vec4 fragColor;\n"
    "#define SCREEN_SIZE vec2(textureSize(tex, 0))\n"
)

name_re = re.compile(r"^[a-z0-9-]+$")
header_re = re.compile(r"^//\s*([a-z]+)\s*:\s*(.*?)\s*$")
ident_re = re.compile(r"(?<![\w.])[A-Za-z_]\w*")
comment_re = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)


class FxError(Exception):
    pass


class Effect:
    def __init__(self, path):
        self.path = path
        self.name = path.stem
        self.source_text = path.read_text()
        self.meta = {}
        for line in self.source_text.splitlines():
            if not line.startswith("//"):
                break
            m = header_re.match(line)
            if m and m.group(1) not in self.meta:
                self.meta[m.group(1)] = m.group(2)
        meta = self.meta
        self.label = meta.get("label") or self.name
        self.emoji = meta.get("emoji", "")
        self.category = meta.get("category", "")
        self.tags = [t.strip().lower() for t in meta.get("tags", "").split(",") if t.strip()]
        self.description = meta.get("description", "")
        self.source = meta.get("source", "original")
        self.license = meta.get("license", "original")
        try:
            self.order = int(meta.get("order", 500))
        except ValueError:
            self.order = 500
        self.animated = meta.get("animated", "no").lower() == "yes"
        self.samples = meta.get("samples", "no").lower() == "yes"
        self.raw = meta.get("raw", "no").lower() == "yes"

    def warnings(self):
        out = []
        for key in HEADER_KEYS:
            if key not in self.meta:
                out.append(f"missing header key '{key}'")
        if self.category and self.category not in CATEGORIES:
            out.append(f"unknown category '{self.category}'")
        return out

    def sort_key(self):
        cat = CATEGORIES.index(self.category) if self.category in CATEGORIES else len(CATEGORIES)
        return (cat, self.order, self.label.lower())


def load_effects():
    """All effects keyed by name, in category/order/label order."""
    effects = [Effect(p) for p in sorted(shader_dir.glob("*.frag")) if name_re.match(p.stem)]
    effects.sort(key=Effect.sort_key)
    return {e.name: e for e in effects}


# # Identifier renaming
def blank_preprocessor(text):
    """Text with preprocessor lines blanked out, for scanning declarations."""
    return re.sub(r"^[ \t]*#[^\n]*", "", text, flags=re.M)


def top_level_names(text):
    """Names declared at file scope (functions, variables, structs) plus #define names."""
    names = set(re.findall(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)", text, flags=re.M))
    depth, start = 0, 0
    scan = blank_preprocessor(text)
    for i, ch in enumerate(scan):
        if ch in "({[":
            depth += 1
        elif ch in ")]":
            depth -= 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                names |= declared(scan[start : i + 1])
                start = i + 1
        elif ch == ";" and depth == 0:
            names |= declared(scan[start:i])
            start = i + 1
    return names


def declared(stmt):
    stmt = stmt.strip().lstrip(";").strip()
    if not stmt or stmt.startswith("precision"):
        return set()
    toks = re.findall(r"[A-Za-z_]\w*", stmt)
    if stmt.startswith("struct"):
        return {toks[1]} if len(toks) > 1 else set()
    head = stmt.split("=")[0]
    if "(" in head:  # function definition or prototype
        m = re.search(r"([A-Za-z_]\w*)\s*\(", head)
        return {m.group(1)} if m else set()
    names, depth, piece = set(), 0, ""
    for ch in stmt + ",":
        depth += ch in "([{"
        depth -= ch in ")]}"
        if ch == "," and depth == 0:
            decl = re.sub(r"\[[^\]]*\]", "", piece.split("=")[0])
            ids = re.findall(r"[A-Za-z_]\w*", decl)
            if ids:
                names.add(ids[-1])
            piece = ""
        else:
            piece += ch
    return names


def rename(text, names, prefix):
    return ident_re.sub(lambda m: prefix + m.group(0) if m.group(0) in names else m.group(0), text)


def prepare(effect, index):
    """Effect body with every file-scope identifier prefixed `fx<index>_`; returns (code, effect_fn)."""
    body = comment_re.sub(lambda m: "\n" * m.group(0).count("\n"), effect.source_text)
    names = top_level_names(body) | {"effect"}
    prefix = f"fx{index}_"
    return rename(body, names, prefix), prefix + "effect"


def strip_header(text):
    """Raw shader without its leading `//` header lines (`#version` must come first)."""
    lines = text.splitlines(keepends=True)
    i = 0
    while i < len(lines) and (lines[i].startswith("//") or not lines[i].strip()):
        i += 1
    return "".join(lines[i:])


# # Stack logic
def resolve(stack, effects):
    """Active effects (applied/shadowed split) for a stack of names."""
    active = [effects[n] for n in stack]
    if any(e.raw for e in active):
        raw = [e for e in active if e.raw][-1]
        return [raw], []
    geo = [e for e in active if e.samples]
    return active, [e.name for e in geo[:-1]]


def compose(names, effects=None):
    """Generated shader text for the stack `names` (verbatim file for a raw effect)."""
    effects = effects or load_effects()
    active, shadowed = resolve(list(names), effects)
    if not active:
        raise FxError("empty stack")
    if active[0].raw:
        return strip_header(active[0].source_text)
    parts = [PROLOGUE]
    geo, others = None, []
    for i, e in enumerate(active):
        if e.name in shadowed:
            continue
        code, fn = prepare(e, i)
        parts.append(f"// ---- {e.name}\n{code}\n")
        if e.samples:
            geo = fn
        else:
            others.append(fn)
    parts.append("void main() {\n    vec3 c = texture(tex, v_texcoord).rgb;\n")
    if geo:
        parts.append(f"    c = {geo}(c, v_texcoord);\n")
    parts.extend(f"    c = {fn}(c, v_texcoord);\n" for fn in others)
    parts.append("    fragColor = vec4(clamp(c, 0.0, 1.0), 1.0);\n}\n")
    return "".join(parts)


def needs_damage_off(names, effects):
    active, shadowed = resolve(list(names), effects)
    return any((e.animated or e.samples) and e.name not in shadowed for e in active)


def read_stack(effects=None):
    effects = effects if effects is not None else load_effects()
    try:
        lines = stack_file.read_text().split()
    except FileNotFoundError:
        return []
    out = []
    for n in lines:
        if n in effects and n not in out:
            out.append(n)
    return out


def known(names, effects):
    for n in names:
        if n not in effects:
            raise FxError(f"unknown effect '{n}'")


def build_stack(current, names, effects, mode):
    """New stack after `mode` (set|toggle|remove) applied to `names`."""
    known(names, effects)
    stack = list(current)
    if mode == "set":
        stack = []
    for n in names:
        e = effects[n]
        if mode == "remove" or (mode == "toggle" and n in stack):
            stack = [s for s in stack if s != n]
        elif n not in stack or e.raw:
            if e.raw or any(effects[s].raw for s in stack):
                stack = [n]
            else:
                stack.append(n)
    return stack


# # State
def write_atomic(path, text):
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=f".{path.name}.", suffix=".tmp")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, 0o644)
        os.replace(tmp, path)
    except BaseException:
        Path(tmp).unlink(missing_ok=True)
        raise


def apply(stack, effects=None):
    """Write the state files for `stack` and reload Hyprland so toggles.lua picks them up."""
    effects = effects if effects is not None else load_effects()
    state_dir.mkdir(parents=True, exist_ok=True)
    obsolete_flag.unlink(missing_ok=True)
    if stack:
        write_atomic(stack_file, "\n".join(stack) + "\n")
        write_atomic(frag_file, compose(stack, effects))
        if needs_damage_off(stack, effects):
            damage_file.touch()
        else:
            damage_file.unlink(missing_ok=True)
    else:
        for f in (stack_file, frag_file, damage_file):
            f.unlink(missing_ok=True)
    try:
        result = subprocess.run(["hyprctl", "reload"], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True, check=False)
    except OSError as err:
        raise FxError(f"state written but hyprctl reload failed: {err}")
    if result.returncode != 0:
        detail = result.stderr.strip() or f"exit status {result.returncode}"
        raise FxError(f"state written but hyprctl reload failed: {detail}")


def status(effects=None):
    effects = effects if effects is not None else load_effects()
    stack = read_stack(effects)
    _, shadowed = resolve(stack, effects) if stack else ([], [])
    return {
        "active": stack,
        "shadowed": shadowed,
        "damage_tracking_off": bool(stack) and needs_damage_off(stack, effects),
    }


def list_effects(effects=None):
    effects = effects if effects is not None else load_effects()
    stack = read_stack(effects)
    _, shadowed = resolve(stack, effects) if stack else ([], [])
    rows = []
    for e in effects.values():
        rows.append({
            "name": e.name, "label": e.label, "emoji": e.emoji, "category": e.category,
            "tags": e.tags, "description": e.description, "source": e.source, "license": e.license,
            "order": e.order, "animated": e.animated, "samples": e.samples, "raw": e.raw,
            "flags": [f for f, on in (("animated", e.animated), ("geometric", e.samples), ("raw", e.raw)) if on],
            "active": e.name in stack,
            "position": stack.index(e.name) + 1 if e.name in stack else None,
            "shadowed": e.name in shadowed,
        })
    return {"categories": CATEGORIES, "groups": GROUPS, "effects": rows}


# # Validation
def check(names, effects=None):
    """Validate effects with glslangValidator; returns (ok, messages)."""
    effects = effects if effects is not None else load_effects()
    known(names, effects)
    if not shutil.which("glslangValidator"):
        raise FxError("glslangValidator not found")
    ok, msgs = True, []
    for n in names or effects:
        e = effects[n]
        msgs.extend(f"{n}: warning: {w}" for w in e.warnings())
        if not e.raw and "effect" not in top_level_names(comment_re.sub("", e.source_text)):
            ok = False
            msgs.append(f"{n}: error: no effect() function")
            continue
        r = subprocess.run(["glslangValidator", "--stdin", "-S", "frag"], input=compose([n], effects),
                           capture_output=True, text=True)
        if r.returncode:
            ok = False
            msgs.append(f"{n}: FAILED\n{(r.stdout + r.stderr).strip()}")
        else:
            msgs.append(f"{n}: ok")
    return ok, msgs
