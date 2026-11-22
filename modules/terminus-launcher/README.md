# Terminus-Launcher

The desktop's launcher: an old instrument's CRT in a riveted casing, centred low on the focused monitor. A small
resident Quickshell config next to Terminus-Bar; it only toggles visibility. The app index started as
[SekaiPop](https://github.com/Blapples/SekaiPop)'s (GPL-3.0, see `LICENSE`; copied from version 1.0.0).

## Use

- Run: `terminus-launcher.service` (`config/systemd/user/terminus-launcher.service`), started with the session.
- `SUPER + SPACE` (and the bar's left button) toggle it. `terminus-launcher toggle | open | close | search <text>`.
- Typing always types. Up/Down, PgUp/PgDn, Ctrl+Home/End move. Enter runs the row's first action (launch, or focus the
  running window); Shift+Enter opens a new window (the entry's own new-window action when it declares one, otherwise
  the app again). Esc closes; the query is cleared once it is off. Ctrl+U or Ctrl+Backspace clears the query.
- Tab: the row's actions, as the same screen one level deeper (`[ firefox › actions ]`). Apps and windows: focus (f),
  new window (n), move to workspace (m, then 1-0), pin to slot (p, then 1-9, 0 unpins), kill (k, SIGTERM; asks for a
  second Enter). Inside a menu the keys run rows and nothing types; Esc goes back one level before it closes.
- Slots: Ctrl+1-9 pins the selected row, Ctrl+0 unpins it, Alt+1-9 runs a slot. `terminus-launcher pin <digit> <name>`,
  `terminus-launcher slots`.
- With nothing typed: pinned slots, then recents (frecency), then running apps, then everything A to Z.
- omp has a desktop entry (`config/applications/omp.desktop`, linked into `~/.local/share/applications`) whose
  `StartupWMClass` is its Ghostty class, so its windows merge into one `omp` row.

## Screen

Panel grammar as the rest of the system: `[ launch ]` header, a rule, and a `[ mode · total ]` slot (`matches/total`
while typing, the breadcrumb in a menu); two numbered sections (`¹results`, `²<selected item>`); one `key action`
hint row that drops low-priority hints when they do not fit.

- Font: Terminus, a deliberate exception to the panels' JetBrains Mono. The Consoleet Terminus build
  (`~/.local/share/fonts/ConsoleetTerminus`, one font per native size, outlines drawn from the bitmap's pixels)
  at the native size closest to 17 design px (28 px at scale 1.5), unhinted, native rendering, so it lands
  pixel-exact. Rows are the font size plus even leading (36 px), and the scanline period divides the row (3 px).
  Glyphs used are all in Terminus: `● ○ ■ · ¹ ² ↓ › ☼`.
- Mode strip under the prompt: `index  = computus  / archivum  : imperium  @ memoria`, the active mode bright, an
  unavailable one (tool or bar command missing) struck through and dimmer.
- Rows: `●` running / `○` not, the title, `ws N` for running apps, a 5-cell frecency meter. The list box keeps one
  spare row: padding, or `↓ N more`. The detail pane shows only rows with something to say: windows and last use
  while running (plus the window titles), launch count and the entry's desktop actions when closed, the slot only
  when pinned.
- Phosphor: three brightnesses of the theme accent on a warm near-black derived from the theme background. Bright:
  the header, section labels, hint keys, the prompt and selected titles' details. Normal (one step under bright):
  body text. Dim: metadata only. Dangerous rows use `warning` from `theme.json`, or `alert` if it has none.
- Selection: inverse video, the only filled element. Moving it leaves a brief afterglow.
- CRT: one shader, `shell/shaders/crt.frag`, over the text layer: warm glass faintly lit toward the middle, bloom and
  a wider halation from the bright tier only, deep scanlines (34%, plus a lighter line before each), a 2 px aperture
  grille (9%, the same columns in every 14 px glyph cell), a vignette confined to a band along the edges inside the
  text margins (no text is darkened by it), uneven ageing, static grain on the glass
  (not the text), rounded tube corners with a shadow inside the edge, and a faint reflection at the upper left.
  The scanline period divides the row height exactly (3 px), so every row is cut the same way. Strengths are the
  ShaderEffect's properties in `Launcher.qml`. There is no chromatic aberration and no barrel warp (it would bend
  the bitmap font off its pixels).
- Motion (`launcher.json` `motion`, on by default, in the shader): very fine phosphor grain changing 30 times a
  second (full on the glass, a third on the text), a faint brighter refresh band drifting down every 7 s, and a 1-2%
  waver of the brightness on two slow periods (4.3 s, 7.1 s), never fast enough to flicker. Nothing moves pixels,
  so the text stays sharp; the clock runs only while the launcher is shown.
- In the screen: the prompt's block cursor blinks (530 ms, shown at once while typing), and when the rows change
  they type on top to bottom over 60 ms (a refresh that brings back the same rows does not).
- Open: about 300 ms. A line opens to full height, the tube brightens unevenly, then the text settles.
  Close: collapse to a line, then off.
- Casing: riveted plates ("☼ terminus · est" / "apparatus i" above, "this is the line of division" below) and
  a double-ruled frame.

`~/.config/terminus-launcher/launcher.json` (a link to `config/terminus-launcher/launcher.json`), re-read when saved:
`{"frame": true, "motion": true, "animation": {"warmup": true, "typeOn": true, "cursorBlink": true, "afterglow": true},
"labels": "latin"}`. `frame: false` drops the double rule, `motion: false` stills the tube. Each `animation` key
turns one animation off: the warm-up/collapse (the tube is then simply on or off), the rows typing on, the cursor
blink (a steady block), the selection's afterglow. `labels` is `"latin"` (index, computus, archivum, imperium,
memoria), `"plain"` (apps, calculator, files, system, clipboard) or an object overriding single modes.

## Architecture (`shell/`)

- `modules/launcher/Item.js`: the item schema every provider returns (id, title, value, glyph, meter, slot, summary,
  detail rows, lazy `probe`, actions, danger, remember, pinnable, inert). The screen reads only these fields.
- `modules/launcher/Provider.qml`: the provider shape. `query(text, token)` answers through `results(token, items)`.
  Also `cancel()`, `opened()`, `resolve(id)` (for pinned slots), and `stale()` to ask the router to query again.
  Properties: `prefix`, `global`, `slow`, `missing`.
- `modules/launcher/Router.qml`: sends a query to the provider its prefix selects, or to every `global` provider.
  Slow providers are debounced by 30 ms. Each query gets a new token, older answers are dropped and providers
  that are no longer asked are cancelled. A provider whose tool is missing becomes a notice row.
- `modules/launcher/Launcher.qml`: the window, keys, animation and drawing. It contains nothing source-specific.
- `providers/IndexProvider.qml`: apps (Quickshell `DesktopEntries`, through `services/AppSource.qml`) merged with
  Hyprland windows (`Quickshell.Hyprland`).
  - A running app shows `●` and its workspaces, and Enter focuses its most recent window.
  - A window gets its own row when no app claims it, or when the query matches its title.
  - Memory is measured only for the selected row (`scripts/probe/memory.sh`: proportional memory of the
    process tree).
  - Scattered-letter matches of apps never launched only show when nothing matches more closely. A match from the
    name's first letter ("ff" -> firefox) is close, and an app with launch history is never hidden.
- `providers/ComputusProvider.qml` (`=`, and bare arithmetic or "N unit to unit"): qalc, one process per query
  (killed when the query changes), no mixed-unit output. Enter copies the result (`wl-copy`) and keeps it among the
  recent calculations (`=` alone lists them; the detail pane shows them).
- `providers/ImperiumProvider.qml` (`:`, and close matches without it): lock, sleep, logout, reboot, shutdown, night
  light, do not disturb (toggle; its row shows the bar's current state from `notification-dnd-status`), keep awake, mic mute, through `terminus-bar msg`. Logout, reboot and shutdown ask
  for a second Enter and show how many apps are open. "rescan displays" runs the bar's `display-rescan` (DDC/CI).
- `providers/ArchivumProvider.qml` (`/` only): fd by name in `~/Projects`, `~/.config` (hidden files and folders
  included there only, `.git` internals not) and the rest of home (hidden and git-ignored paths skipped), 200 hits
  from each, one process per query. Ranked by name match, shallower paths, `~/Projects` first, then `~/.config`, and
  `~/Backups` down (not excluded). Symlinks are not followed. The selected row is probed (`scripts/probe/file.sh`): MIME type, the apps
  registered for it, and the first 12 lines of text files in the detail pane. Actions: open (xdg-open), open with
  (the registered apps, through `gio launch`), reveal in folder (FileManager1 ShowItems, else the folder), copy path.
- `providers/MemoriaProvider.qml` (`@`): the bar's clipboard history through `terminus-bar msg clipboard-list`
  (JSON), `clipboard-get`, `clipboard-copy-entry`, `clipboard-pin <id> on|off` and `clipboard-remove`. Rows: preview
  and age, `◆` pinned, `■` image. Entries the bar marks `masked` keep their masked preview in the list; the real text
  is fetched with `clipboard-get` only for the selected row's detail pane, as is every entry's full text or image
  (tinted to the phosphor). Enter copies the entry back; Tab: copy, pin/unpin, delete. The bar is re-asked for
  `clipboard-list` every time `@` is typed, so the mode shows as unavailable on a bar without it.
- `modules/common/functions/Fuzzy.js`: matching.
  - Order: exact > prefix > word start > initials > inside a word > subsequence (word starts and runs score
    higher, gaps cost).
  - Keywords, descriptions and window titles only count substring hits.
- `modules/common/functions/Frecency.js` + `services/LaunchState.qml`: what the launcher remembers.
  - A launch log per item with a 7-day half-life, plus which query led to which pick (a strong boost when the
    same query is typed again).
  - Stored in `~/.local/state/terminus-launcher/launch-state.json`.
  - Slots are in `launcher-slots.json` (item ids; an old bare desktop id reads as `app:<id>`).
- `services/ShellScale.qml`: size per monitor (`~/.config/terminus-launcher/scale.json`, `{"default": 1.5}`). 1.0 is the
  design size, applied to every length and font size (not an item scale), so text stays sharp through the shader.
- `services/LauncherConfig.qml`: `launcher.json`. `modules/common/Theme.qml`: `theme.json`.
- `shaders/build.sh`: compiles `*.frag` to `.qsb` with `qsb` (qt6-shadertools). The `.qsb` is kept in the repository.
  Run the script after editing the shader.

Changes need `systemctl --user restart terminus-launcher` (the shell does not reload itself).
