# Hyprland plugins

Locally built Hyprland plugins for Terminus-Est. Build with the Makefile in each directory.

## Window effects (hyprwindowshade, hyprfocus, hyprtrails)

Used by the `Window effects` entries in `config/motion/effects/*.lua`. All three are built locally against the
installed Hyprland headers; hyprpm is not used. Each directory has an `UPSTREAM` file with URL, commit, licence and
patches.

| Plugin | Provides | Entries |
| --- | --- | --- |
| `hyprwindowshade` | one-shot open/close fragment shaders on windows (fire, glitch, matrix, cell break, pixelate) | `fire`, `close-glitch`, `close-matrix`, `close-voronoi-shatter`, `open-glitch`, `open-matrix`, `open-pixelate` |
| `hyprfocus` | flash / shrink / slide when a window gains focus | `focus-flash`, `focus-shrink` |
| `hyprtrails` | amber ribbon behind a moving window (ported to 0.57.2, removed upstream) | `trails` |

### Rebuild after a Hyprland update

Hyprland refuses plugins built against another commit, and its C++ ABI changes between releases. After every
Hyprland/compiler update, restart into the new compositor first, then:

```bash
for p in hyprwindowshade hyprfocus hyprtrails; do make -B -C ~/Projects/Terminus-Est/modules/hyprland-plugins/$p; done
```

Output is `<plugin>/build/<plugin>.so`. Build in a temp copy or the motion sandbox first
(`tools/terminus-motion-sandbox start fx; ... use fx <entry>`); a plugin that does not compile against the new
headers needs a source fix (hyprfocus/hyprwindowshade track upstream; hyprtrails is our own port, see its `UPSTREAM`).
If a `.so` is missing, an entry prints a warning, shows a notification and does nothing else.

### Option syntax (Lua config)

Entries load the plugin with `hl.plugin.load(path)` (declarative: omitted on the next reload means unloaded) and set
options through `hl.config`, the plugin name is a nested table:

```lua
hl.config({ plugin = { hyprfocus = { keyboard_focus_animation = "flash", fade_opacity = 0.5 } } })
hl.config({ plugin = { hyprtrails = { color = "rgba(ffaa00cc)", history_points = 24 } } })
```

- hyprfocus: `enable`, `keyboard_focus_animation` / `mouse_focus_animation` / `group_focus_animation`
  (`flash`, `shrink`, `slide`, `none`), `fade_opacity`, `shrink_percentage`, `slide_height`, `animate_floating`,
  `only_on_monitor_change`; animation leaves `hyprfocusIn` / `hyprfocusOut` via `hl.animation`.
- hyprtrails: `color`, `bezier_step`, `points_per_step`, `history_points`, `history_step`.
- hyprwindowshade has no options table; it is driven by window-rule tags:
  `hl.window_rule({ name = "...", match = { class = ".*" }, tag = "+shader_close:/abs/path/fire_close.glsl" })`
  (`+shader_open:` for open). The shader declares its length with `// @duration <seconds>` (max 5 s). Open and close
  rules use different tags, so one open and one close entry work together. Rule names must be unique per entry slot
  (`motion-fx-open`, `motion-fx-close`).
- Entries removed from the active list are not run on the next reload; the plugin is unloaded and the rule and options
  are gone (checked in the sandbox; the compositor kept running).

### Known limits

- hyprwindowshade open effects do not play on a window that is fullscreen or maximized. kitty asks for a maximized
  state in the sandbox, so use ghostty (`--gtk-single-instance=false`) or another client for open demos. Close effects
  also need a client that keeps its buffer until unmap (foot, for one, burns as an empty frame).
- hyprwindowshade open shaders from the Hyprland-Shader collection declare no duration; we added `// @duration 0.7`.
- The rule matches every window (`class = ".*"`). Add a narrower `match` in the entry to limit it.
- hyprtrails draws behind the window and its tail stays about a second after the
  window stops. Its tick timer wakes the compositor on every frame while the plugin is loaded.
- hyprtrails is not drawn for windows with `decorate = false`.

### Performance

- `fire`: four-octave simplex noise, three fbm lookups per pixel of the window area for 1.2 s; the heaviest entry.
  On an RX 580 at 2560x1440 expect it to be fine for one window but not for a fullscreen-sized one at 144 Hz
  [INFERENCE, not measured: the sandbox is 960x540 and headless].
- `close-matrix`, `open-matrix`: long shaders (~300 lines), also on the heavy side.
- `close-glitch`, `open-glitch`, `open-pixelate`, `close-voronoi-shatter`: cheap.
- hyprwindowshade renders every shaded element through an offscreen pass; with no shader tag the cost is one lookup.
- hyprfocus: negligible. hyprtrails: one extra pass element and a per-frame timer while loaded.
