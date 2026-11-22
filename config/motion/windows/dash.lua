-- label: Dash
-- emoji: 💨
-- category: Windows · Fast
-- slot: windows
-- tags: fast, slide, edge, 150ms
-- description: Windows dash in from the edge in 150 ms and leave in about 100 ms.
-- source: original
-- license: original
-- demo: windows
hl.animation({ leaf = "windowsIn", enabled = true, speed = 1.5, bezier = "out", style = "slide" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 1, bezier = "in", style = "slide" })
