-- label: Focus flash
-- emoji: ⚡
-- category: Window effects
-- slot: focus
-- tags: focus, flash, hyprfocus, fade, blink
-- description: The window that gains focus dips to half opacity and flashes back.
-- source: https://github.com/hyprwm/hyprland-plugins (hyprfocus)
-- license: BSD-3-Clause
-- demo: focus
local home = os.getenv("HOME")
local module = home .. "/Projects/Terminus-Est/modules/hyprland-plugins/hyprfocus"
local plugin = module .. "/build/hyprfocus.so"
if o.file_exists(plugin) then
  hl.plugin.load(plugin)
  hl.config({
    plugin = {
      hyprfocus = {
        enable = true,
        keyboard_focus_animation = "flash",
        mouse_focus_animation = "flash",
        group_focus_animation = "flash",
        fade_opacity = 0.5,
      },
    },
  })
  hl.curve("fxfocus", { type = "bezier", points = { { 0.2, 0.9 }, { 0.3, 1.0 } } })
  hl.animation({ leaf = "hyprfocusIn", enabled = true, speed = 2.5, bezier = "fxfocus" })
  hl.animation({ leaf = "hyprfocusOut", enabled = true, speed = 4, bezier = "fxfocus" })
else
  local warning = "[terminus-motion] Window effects unavailable, plugin not built. Run: make -B -C " .. module
  print(warning)
  hl.notification.create({ text = warning, timeout = 10000, icon = "warning" })
end
