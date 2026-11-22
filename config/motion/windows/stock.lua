-- label: Hyprland stock
-- emoji: 🏠
-- category: Windows · Calm
-- slot: windows
-- tags: stock, default, spring, oem
-- description: Windows spring in from 87% size and shrink away on a linear, quick fade.
-- source: Hyprland default animations (as shipped in https://github.com/LinuxBeginnings/Hyprland-Dots/blob/main/config/hypr/animations/Hyprland%20-%20OEM%20Default.lua)
-- license: BSD-3-Clause
-- demo: windows
hl.curve("stock_easy", { type = "spring", mass = 1, stiffness = 238.1191, dampening = 24.21279333 })
hl.animation({ leaf = "windowsIn", enabled = true, speed = 4.1, spring = "stock_easy", style = "popin 87%" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 1.49, bezier = "linear", style = "popin 87%" })
