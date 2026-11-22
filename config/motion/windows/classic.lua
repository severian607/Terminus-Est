-- label: Classic
-- emoji: 🕹️
-- category: Windows · Bounce
-- slot: windows
-- tags: classic, overshoot, slide, previous, hyde
-- description: Windows slide in from an edge with a springy overshoot and leave with a small backswing.
-- source: https://github.com/HyDE-Project/HyDE (animations/HYDE - default.conf), the previous Terminus-Est setup
-- license: GPL-3.0
-- demo: windows
hl.curve("winIn", { type = "bezier", points = { { 0.1, 1.1 }, { 0.1, 1.1 } } })
hl.curve("winOut", { type = "bezier", points = { { 0.3, -0.3 }, { 0, 1 } } })
hl.animation({ leaf = "windowsIn", enabled = true, speed = 6, bezier = "winIn", style = "slide" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 5, bezier = "winOut", style = "slide" })
