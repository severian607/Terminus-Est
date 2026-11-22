-- label: Fire
-- emoji: 🔥
-- category: Window effects
-- slot: close-effect
-- tags: fire, burn, flame, close, shader, burn-my-windows
-- description: The closing window burns away from the top down, flames licking up from the edge.
-- source: https://github.com/theguriev/hyprburn (Burn-My-Windows fire.frag, run through HyprWindowShade)
-- license: GPL-3.0
-- demo: windows
local home = os.getenv("HOME")
local module = home .. "/Projects/Terminus-Est/modules/hyprland-plugins/hyprwindowshade"
local plugin = module .. "/build/hyprwindowshade.so"
if o.file_exists(plugin) then
  hl.plugin.load(plugin)
  hl.window_rule({
    name = "motion-fx-close",
    match = { class = ".*" },
    tag = "+shader_close:" .. module .. "/shaders/close/fire_close.glsl",
  })
else
  local warning = "[terminus-motion] Window effects unavailable, plugin not built. Run: make -B -C " .. module
  print(warning)
  hl.notification.create({ text = warning, timeout = 10000, icon = "warning" })
end
