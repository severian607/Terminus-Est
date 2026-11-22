-- label: Matrix out
-- emoji: 🟩
-- category: Window effects
-- slot: close-effect
-- tags: matrix, code, green, close, rain
-- description: The closing window turns into falling green code and rains away.
-- source: https://github.com/jbuck95/Hyprland-Shader (run through HyprWindowShade)
-- license: MIT
-- demo: windows
local home = os.getenv("HOME")
local module = home .. "/Projects/Terminus-Est/modules/hyprland-plugins/hyprwindowshade"
local plugin = module .. "/build/hyprwindowshade.so"
if o.file_exists(plugin) then
  hl.plugin.load(plugin)
  hl.window_rule({
    name = "motion-fx-close",
    match = { class = ".*" },
    tag = "+shader_close:" .. module .. "/shaders/close/matrix_close.glsl",
  })
else
  local warning = "[terminus-motion] Window effects unavailable, plugin not built. Run: make -B -C " .. module
  print(warning)
  hl.notification.create({ text = warning, timeout = 10000, icon = "warning" })
end
