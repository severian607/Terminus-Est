-- label: Slow bounce
-- emoji: 🏀
-- category: Windows · Bounce
-- slot: windows
-- tags: bounce, popin, slow, v2
-- description: Windows swell up slowly from 80% with a late bounce and shrink out with a slow squeeze.
-- source: https://github.com/LinuxBeginnings/Hyprland-Dots (animations/01-default%20-%20v2.lua)
-- license: GPL-3.0
-- demo: windows
hl.curve("v2_slow", { type = "bezier", points = { { 0, 0.85 }, { 0.3, 1 } } })
hl.curve("v2_winout", { type = "bezier", points = { { 0.3, 0 }, { 0, 1 } } })
hl.animation({ leaf = "windowsIn", enabled = true, speed = 5, bezier = "v2_slow", style = "popin" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 5, bezier = "v2_winout", style = "popin" })
