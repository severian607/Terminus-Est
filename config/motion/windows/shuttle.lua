-- label: Shuttle
-- emoji: 🚃
-- category: Windows · Slide
-- slot: windows
-- tags: slide, left, right, directional
-- description: Windows enter from the left edge and leave through the right edge.
-- source: original
-- license: original
-- demo: windows
hl.animation({ leaf = "windowsIn", enabled = true, speed = 3, bezier = "out", style = "slide left" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 2, bezier = "in", style = "slide right" })
