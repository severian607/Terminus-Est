-- label: Rise
-- emoji: 🎈
-- category: Windows · Slide
-- slot: windows
-- tags: slide, bottom, up, directional
-- description: Windows rise from the bottom edge and sink back down when closed.
-- source: original
-- license: original
-- demo: windows
hl.animation({ leaf = "windowsIn", enabled = true, speed = 3, bezier = "out", style = "slide bottom" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 2, bezier = "in", style = "slide bottom" })
