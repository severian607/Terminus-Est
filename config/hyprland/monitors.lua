-- # Physical displays

-- The default for any machine: Hyprland places every output at its preferred mode by itself.
-- To name outputs, copy this file to config/hosts/<hostname>/hyprland/monitors.lua (it replaces this one) and set:
--   o.monitors = { primary = "DP-1", secondary = "HDMI-A-1" }
--   hl.monitor({ output = o.monitors.primary, mode = "2560x1440@144", position = "auto", scale = 1 })
-- Other Lua modules read o.monitors: the launcher and control center open on `primary`, workspaces 1-4 live there
-- and workspace 5 on `secondary`. Both are optional.
o.monitors = {}
hl.env("GDK_SCALE", "1")
