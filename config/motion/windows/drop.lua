-- label: Drop
-- emoji: ⬇️
-- category: Windows · Slide
-- slot: windows
-- tags: slide, top, down, directional
-- description: Windows drop in from the top edge and lift back up when closed.
-- source: original
-- license: original
-- demo: windows
hl.animation({ leaf = "windowsIn", enabled = true, speed = 3, bezier = "out", style = "slide top" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 2, bezier = "in", style = "slide top" })
