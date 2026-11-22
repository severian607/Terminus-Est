-- label: Slingshot
-- emoji: 🎯
-- category: Windows · Showy
-- slot: windows
-- tags: slingshot, fast-start, slide, v2
-- description: Windows are shot in from an edge: a burst at the start, then a long slow crawl to a stop.
-- source: https://github.com/LinuxBeginnings/Hyprland-Dots (animations/01-default%20-%20v2.lua)
-- license: GPL-3.0
-- demo: windows
hl.curve("sling", { type = "bezier", points = { { 1, 0 }, { 0.15, 1 } } })
hl.animation({ leaf = "windowsIn", enabled = true, speed = 6, bezier = "sling", style = "slide" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 4, bezier = "sling", style = "slide" })
