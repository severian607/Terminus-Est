-- label: Glide
-- emoji: 🛷
-- category: Windows · Slide
-- slot: windows
-- tags: slide, right, slow, directional
-- description: Windows glide in slowly from the right edge and leave the same way.
-- source: original
-- license: original
-- demo: windows
hl.animation({ leaf = "windowsIn", enabled = true, speed = 5, bezier = "out", style = "slide right" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 4, bezier = "in", style = "slide right" })
