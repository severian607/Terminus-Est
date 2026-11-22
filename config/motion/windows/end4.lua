-- label: Material pop
-- emoji: 🎨
-- category: Windows · Pop
-- slot: windows
-- tags: end-4, material, md3, popin, workspaces
-- description: Windows pop up from 60% size with a soft Material-style ease; workspaces slide.
-- source: https://github.com/end-4/dots-hyprland (as shipped in https://github.com/LinuxBeginnings/Hyprland-Dots/blob/main/config/hypr/animations/END-4.lua)
-- license: GPL-3.0
-- demo: windows
hl.curve("end4_decel", { type = "bezier", points = { { 0.05, 0.7 }, { 0.1, 1 } } })
hl.curve("end4_accel", { type = "bezier", points = { { 0.3, 0 }, { 0.8, 0.15 } } })
hl.curve("end4_menu_decel", { type = "bezier", points = { { 0.1, 1 }, { 0, 1 } } })
hl.animation({ leaf = "windowsIn", enabled = true, speed = 3, bezier = "end4_decel", style = "popin 60%" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 3, bezier = "end4_accel", style = "popin 60%" })
hl.animation({ leaf = "fade", enabled = true, speed = 3, bezier = "end4_decel" })
hl.animation({ leaf = "workspaces", enabled = true, speed = 7, bezier = "end4_menu_decel", style = "slide" })
hl.animation({ leaf = "specialWorkspace", enabled = true, speed = 3, bezier = "end4_decel", style = "slidevert" })
