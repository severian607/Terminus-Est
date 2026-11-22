-- label: GNOME
-- emoji: 🦶
-- category: Windows · Pop
-- slot: windows
-- tags: gnome, gnomed, scale
-- description: Windows grow out of their centre like in GNOME Shell and shrink back when closed.
-- source: original
-- license: original
-- demo: windows
hl.animation({ leaf = "windowsIn", enabled = true, speed = 3, bezier = "out", style = "gnomed" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 2, bezier = "in", style = "gnomed" })
