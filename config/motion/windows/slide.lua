-- label: Slide
-- emoji: ➡️
-- category: Windows · Slide
-- slot: windows
-- tags: slide, edge, directional
-- description: Windows slide in from the nearest screen edge and back out to it.
-- source: original
-- license: original
-- demo: windows
hl.animation({ leaf = "windowsIn", enabled = true, speed = 3, bezier = "out", style = "slide" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 2, bezier = "in", style = "slide" })
