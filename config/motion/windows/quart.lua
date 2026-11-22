-- label: Quart
-- emoji: 🌊
-- category: Windows · Calm
-- slot: windows
-- tags: hyde, minimal, quart, slide
-- description: Windows slide in from the nearest edge with a smooth quartic ease over 600 ms.
-- source: https://github.com/HyDE-Project/HyDE (animations/HYDE - minimal-2.conf, via https://github.com/LinuxBeginnings/Hyprland-Dots/blob/main/config/hypr/animations/HYDE%20-%20minimal-2.lua)
-- license: GPL-3.0
-- demo: windows
hl.curve("hyde_quart", { type = "bezier", points = { { 0.25, 1 }, { 0.5, 1 } } })
hl.animation({ leaf = "windowsIn", enabled = true, speed = 6, bezier = "hyde_quart", style = "slide" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 6, bezier = "hyde_quart", style = "slide" })
hl.animation({ leaf = "fade", enabled = true, speed = 6, bezier = "hyde_quart" })
hl.animation({ leaf = "workspaces", enabled = true, speed = 6, bezier = "hyde_quart" })
