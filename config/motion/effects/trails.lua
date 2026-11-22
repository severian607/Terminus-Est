-- label: Trails
-- emoji: ☄️
-- category: Window effects
-- slot: trails
-- tags: trail, trails, hyprtrails, motion, move, comet
-- description: A fading amber ribbon follows a window while it is moved or dragged.
-- source: https://github.com/hyprwm/hyprland-plugins (hyprtrails, removed upstream; ported to 0.57.2)
-- license: BSD-3-Clause
-- demo: move
local home = os.getenv("HOME")
local module = home .. "/Projects/Terminus-Est/modules/hyprland-plugins/hyprtrails"
local plugin = module .. "/build/hyprtrails.so"
if o.file_exists(plugin) then
  hl.plugin.load(plugin)
  hl.config({
    plugin = {
      hyprtrails = {
        color = "rgba(ffaa00cc)",
        history_points = 24,
        history_step = 2,
      },
    },
  })
else
  local warning = "[terminus-motion] Window effects unavailable, plugin not built. Run: make -B -C " .. module
  print(warning)
  hl.notification.create({ text = warning, timeout = 10000, icon = "warning" })
end
