-- label: Quick pop
-- emoji: 🏃
-- category: Windows · Fast
-- slot: windows
-- tags: hyde, vertical, fast, popin, workspaces
-- description: Windows pop in from 60% size within 150 ms; workspaces slide and fade vertically.
-- source: https://github.com/HyDE-Project/HyDE (animations/HYDE - Vertical.conf, via https://github.com/LinuxBeginnings/Hyprland-Dots/blob/main/config/hypr/animations/HYDE%20-%20Vertical.lua)
-- license: GPL-3.0
-- demo: windows
hl.curve("hv_decel", { type = "bezier", points = { { 0, 0.2 }, { 0.4, 1 } } })
hl.curve("hv_sine", { type = "bezier", points = { { 0.37, 0 }, { 0.63, 1 } } })
hl.curve("hv_cubic", { type = "bezier", points = { { 0.33, 1 }, { 0.68, 1 } } })
hl.animation({ leaf = "windowsIn", enabled = true, speed = 1.5, bezier = "hv_sine", style = "popin 60%" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 1.5, bezier = "hv_cubic", style = "popin 60%" })
hl.animation({ leaf = "fade", enabled = true, speed = 2.5, bezier = "hv_decel" })
hl.animation({ leaf = "workspaces", enabled = true, speed = 3, bezier = "hv_decel", style = "slidefadevert 30%" })
hl.animation({ leaf = "specialWorkspace", enabled = true, speed = 2, bezier = "hv_decel", style = "slidefade 10%" })
