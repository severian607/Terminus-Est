-- label: Sharp slide
-- emoji: 🏎️
-- category: Windows · Fast
-- slot: windows
-- tags: hyde, optimized, fast, slide, sharp
-- description: Windows shoot in from an edge with a sharp stop in about 320 ms and shrink away on close.
-- source: https://github.com/HyDE-Project/HyDE (animations/HYDE - optimized.conf, via https://github.com/LinuxBeginnings/Hyprland-Dots/blob/main/config/hypr/animations/HYDE%20-%20optimized.lua)
-- license: GPL-3.0
-- demo: windows
hl.curve("ho_in", { type = "bezier", points = { { 0.07, 0.88 }, { 0.04, 0.99 } } })
hl.curve("ho_out", { type = "bezier", points = { { 0, 0.48 }, { 0.38, 1 } } })
hl.animation({ leaf = "windowsIn", enabled = true, speed = 3.2, bezier = "ho_in", style = "slide" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 2.8, bezier = "ho_out" })
hl.animation({ leaf = "fade", enabled = true, speed = 1.8, bezier = "ho_out" })
