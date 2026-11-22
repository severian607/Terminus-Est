-- label: Shake to find
-- emoji: 🔍
-- category: Cursor · Pointer
-- slot: pointer-shake
-- tags: pointer, shake, find, enlarge, magnify, cursor, dynamic-cursors
-- description: Shake the mouse and the pointer grows large, then shrinks back after a moment.
-- source: https://github.com/VirtCode/hypr-dynamic-cursors (plugin built locally)
-- license: MIT
-- demo: pointer
-- # Self-contained: declares the dynamic-cursors plugin and sets its options. Entries share TERMINUS_POINTER
-- # (reset on every config load) so that pointer-mode and shake entries combine in any load order.
local home = assert(os.getenv("HOME"), "HOME is required")
local module = home .. "/Projects/Terminus-Est/modules/hyprland-plugins/dynamic-cursors"
local artifact = module .. "/build/dynamic-cursors.so"
local pointer = _G.TERMINUS_POINTER or {}
_G.TERMINUS_POINTER = pointer
pointer.shake = true
if o.file_exists(artifact) then
  hl.plugin.load(artifact)
  if hl.plugin.dynamic_cursors then
    hl.config({ plugin = { dynamic_cursors = {
      enabled = true,
      ignore_warps = false, -- on 0.57.2 the plugin sees mouse motion as warps; ignoring them leaves the pointer still
      mode = pointer.mode or "none",
      shake = { enabled = true, threshold = 6.0, base = 4.0, speed = 4.0, limit = 8.0, timeout = 1500, effects = false },
    } } })
  end
else
  local warning = "[terminus-motion] dynamic-cursors plugin not built; build with: make -B -C " .. module
  print(warning)
  hl.notification.create({ text = warning, timeout = 10000, icon = "warning" })
end
