-- label: Flow
-- emoji: 🌀
-- category: Windows · Showy
-- slot: windows
-- tags: mahaveer, slide, layers, workspaces, hyde
-- description: Windows slide in and out; menus, bars and workspaces slide on matching Material curves.
-- source: https://github.com/LinuxBeginnings/Hyprland-Dots/blob/main/config/hypr/animations/Mahaveer%20-%20me-1.lua
-- license: GPL-3.0
-- demo: windows
hl.curve("mh_wind", { type = "bezier", points = { { 0.05, 0.9 }, { 0.1, 1 } } })
hl.curve("mh_in", { type = "bezier", points = { { 0.1, 1 }, { 0.1, 1 } } })
hl.curve("mh_out", { type = "bezier", points = { { 0.3, 0 }, { 0, 1 } } })
hl.curve("mh_decel", { type = "bezier", points = { { 0.05, 0.7 }, { 0.1, 1 } } })
hl.curve("mh_menu_decel", { type = "bezier", points = { { 0.1, 1 }, { 0, 1 } } })
hl.curve("mh_menu_accel", { type = "bezier", points = { { 0.38, 0.04 }, { 1, 0.07 } } })
hl.animation({ leaf = "windowsIn", enabled = true, speed = 6, bezier = "mh_in", style = "slide" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 5, bezier = "mh_out", style = "slide" })
hl.animation({ leaf = "windowsMove", enabled = true, speed = 5, bezier = "mh_wind", style = "slide" })
hl.animation({ leaf = "fade", enabled = true, speed = 3, bezier = "mh_decel" })
hl.animation({ leaf = "layersIn", enabled = true, speed = 3, bezier = "mh_menu_decel", style = "slide" })
hl.animation({ leaf = "layersOut", enabled = true, speed = 1.6, bezier = "mh_menu_accel" })
hl.animation({ leaf = "workspaces", enabled = true, speed = 7, bezier = "mh_menu_decel", style = "slide" })
hl.animation({ leaf = "specialWorkspace", enabled = true, speed = 3, bezier = "mh_decel", style = "slidevert" })
