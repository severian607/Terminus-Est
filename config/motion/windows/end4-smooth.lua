-- label: Smooth settle
-- emoji: 🕊️
-- category: Windows · Calm
-- slot: windows
-- tags: end-4, smooth, decel, popin
-- description: Windows settle in from 80% size with a long smooth ease and shrink out faster.
-- source: https://github.com/end-4/dots-hyprland (as shipped in https://github.com/LinuxBeginnings/Hyprland-Dots/blob/main/config/hypr/animations/END4-smooth.lua)
-- license: GPL-3.0
-- demo: windows
hl.curve("end4s_decel", { type = "bezier", points = { { 0.05, 0.7 }, { 0.1, 1 } } })
hl.animation({ leaf = "windowsIn", enabled = true, speed = 4.9, bezier = "end4s_decel", style = "popin 80%" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 2.9, bezier = "end4s_decel", style = "popin 90%" })
