-- label: Gentle pop
-- emoji: 🍵
-- category: Windows · Calm
-- slot: windows
-- tags: ml4w, classic, standard, gentle, popin
-- description: Windows scale up gently from the centre over 700 ms and shrink to 80% while fading out.
-- source: https://github.com/mylinuxforwork/dotfiles (animations/classic.conf, via https://github.com/LinuxBeginnings/Hyprland-Dots/blob/main/config/hypr/animations/ML4W%20-%20classic.lua)
-- license: GPL-3.0
-- demo: windows
hl.curve("ml4w_bezier", { type = "bezier", points = { { 0.05, 0.9 }, { 0.1, 1 } } })
hl.animation({ leaf = "windows", enabled = true, speed = 7, bezier = "ml4w_bezier" })
hl.animation({ leaf = "windowsIn", enabled = true, speed = 7, bezier = "ml4w_bezier" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 7, bezier = "default", style = "popin 80%" })
hl.animation({ leaf = "fade", enabled = true, speed = 7, bezier = "default" })
hl.animation({ leaf = "workspaces", enabled = true, speed = 6, bezier = "default" })
