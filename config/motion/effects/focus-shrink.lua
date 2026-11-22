-- label: Focus shrink
-- emoji: 🫳
-- category: Window effects
-- slot: focus
-- tags: focus, shrink, hyprfocus, pulse, squash
-- description: The window that gains focus squeezes to 95% and springs back.
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
        keyboard_focus_animation = "shrink",
        mouse_focus_animation = "shrink",
        group_focus_animation = "shrink",
        shrink_percentage = 0.93,
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
