-- label: Tilt
-- emoji: 🪁
-- category: Cursor · Pointer
-- slot: pointer
-- tags: pointer, tilt, lean, sway, cursor, dynamic-cursors
-- description: The pointer leans into the direction you move it, like a pennant in the wind.
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
pointer.mode = "tilt"
if o.file_exists(artifact) then
  hl.plugin.load(artifact)
  if hl.plugin.dynamic_cursors then
    hl.config({ plugin = { dynamic_cursors = {
      enabled = true,
      ignore_warps = false, -- on 0.57.2 the plugin sees mouse motion as warps; ignoring them leaves the pointer still
      mode = pointer.mode or "none",
      tilt = { limit = 4000, full = 60, activation = "negative_quadratic", window = 100 },
      shake = { enabled = pointer.shake == true },
    } } })
  end
else
  local warning = "[terminus-motion] dynamic-cursors plugin not built; build with: make -B -C " .. module
  print(warning)
  hl.notification.create({ text = warning, timeout = 10000, icon = "warning" })
end
