# dynamic-cursors (local build)

Pointer tilt, rotate, stretch and shake-to-find for Hyprland, from
[hypr-dynamic-cursors](https://github.com/VirtCode/hypr-dynamic-cursors) (MIT, see `UPSTREAM`). It is built here
without hyprpm and loaded by the motion entries in `config/motion/pointer/`. The upstream README is kept as
`README.upstream.md`.

## Rebuild after a Hyprland update

Hyprland's C++ ABI is not stable: rebuild against the headers of the Hyprland that will run it, then restart into
that Hyprland before loading.

```sh
cd ~/Projects/Terminus-Est/modules/hyprland-plugins/dynamic-cursors
pkg-config --modversion hyprland      # must match `hyprctl version`
make clean && make -j"$(nproc)"       # writes build/dynamic-cursors.so
```

If the build breaks, fetch the newest upstream commit (`git clone https://github.com/VirtCode/hypr-dynamic-cursors`),
copy `src/` over, reapply the Makefile change listed in `UPSTREAM`, and update `UPSTREAM`. Test in the sandbox first:
`tools/terminus-motion-sandbox start ptr; tools/terminus-motion-sandbox use ptr config/motion/pointer/tilt.lua;
tools/terminus-motion-sandbox ctl ptr plugin list`. Never test a new build in the live session.

## Option syntax (Lua config, Hyprland 0.57.2)

The plugin registers its options under `plugin:dynamic-cursors:*`; in Lua they are a nested table, and the section
name uses an underscore:

```lua
hl.plugin.load(home .. "/Projects/Terminus-Est/modules/hyprland-plugins/dynamic-cursors/build/dynamic-cursors.so")
if hl.plugin.dynamic_cursors then
  hl.config({ plugin = { dynamic_cursors = {
    mode = "tilt",                     -- tilt | rotate | stretch | none
    tilt = { limit = 4000, full = 60 },
    shake = { enabled = true, base = 4.0, limit = 8.0 },
  } } })
end
```

Verified in the sandbox:

- `hl.plugin.load` is declarative. The plugin is loaded when a config load lists it and unloaded on the next reload
  that omits it (`ctl plugin list` then reports `no plugins loaded` and the options are gone).
- `hl.plugin.dynamic_cursors` already exists right after `hl.plugin.load(...)` in the same config pass, on a fresh
  start and on every reload. Guard `hl.config` with it, otherwise a missing plugin is a config error.
- The options are set again by every config load; there is nothing to re-apply by hand. Read them back with
  `hyprctl getoption plugin:dynamic-cursors:mode` (colon form, hyphenated section).
- The Lua state, including globals, is new on every reload. The motion entries rely on that: they share the global
  `TERMINUS_POINTER` (pointer mode, shake on/off), so a `slot: pointer` entry and the `slot: pointer-shake` entry
  produce the same options in any load order, and each alone produces the right options (`mode = "none"` with shake
  only, `shake.enabled = false` with a pointer mode only).
- `hl.dispatch(hl.dsp.cursor.move({ x = .., y = .. }))` moves the pointer in a nested instance.

## Known limits

- Built against 0.57.2 only. Loading a build from another Hyprland version can crash the compositor.
- Software cursors only matter here (`cursor:no_hardware_cursors = 1`); with hardware cursors the plugin has to
  update the cursor buffer on every shape change.
- `ignore_warps` (default `true`) makes the plugin skip pointer warps. Dispatcher moves are warps, so synthetic
  movement in the sandbox shows nothing until `plugin:dynamic-cursors:ignore_warps` is `false`.
  `tools/terminus-motion-demo-pointer` sets it for the preview run; the entries leave it at the default, so
  workspace and focus warps do not move the shape on the real desktop. A real mouse is unaffected.
- Shake magnification of a hyprcursor shape is drawn from the Mita theme's largest bitmap; beyond that size it is
  scaled up pixelated (`hyprcursor:nearest = 1`).
- Entries do not touch the cursor theme; `HYPRCURSOR_THEME=Mita` and size 32 come from `config/hyprland/cursor.lua`.
- If the `.so` is missing the entries only print a warning and show a notification.
