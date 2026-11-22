-- label: Glitch out
-- emoji: 📺
-- category: Window effects
-- slot: close-effect
-- tags: glitch, close, digital, rgb, chromatic
-- description: The closing window tears into shifted red, green and blue slices and breaks up.
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
    tag = "+shader_close:" .. module .. "/shaders/close/glitch_close.glsl",
  })
else
  local warning = "[terminus-motion] Window effects unavailable, plugin not built. Run: make -B -C " .. module
  print(warning)
  hl.notification.create({ text = warning, timeout = 10000, icon = "warning" })
end
