-- label: Pixelate in
-- emoji: 🧩
-- category: Window effects
-- slot: open-effect
-- tags: pixelate, pixels, open, blocks
-- description: The new window resolves from big grey blocks into sharp pixels.
-- source: https://github.com/jbuck95/Hyprland-Shader (run through HyprWindowShade)
-- license: MIT
-- demo: windows
local home = os.getenv("HOME")
local module = home .. "/Projects/Terminus-Est/modules/hyprland-plugins/hyprwindowshade"
local plugin = module .. "/build/hyprwindowshade.so"
if o.file_exists(plugin) then
  hl.plugin.load(plugin)
  hl.window_rule({
    name = "motion-fx-open",
    match = { class = ".*" },
    tag = "+shader_open:" .. module .. "/shaders/open/pixelate_open.glsl",
  })
else
  local warning = "[terminus-motion] Window effects unavailable, plugin not built. Run: make -B -C " .. module
  print(warning)
  hl.notification.create({ text = warning, timeout = 10000, icon = "warning" })
end
