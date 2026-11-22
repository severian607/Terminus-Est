-- label: Hop
-- emoji: 🐇
-- category: Windows · Bounce
-- slot: windows
-- tags: overshoot, slide, bottom, hop
-- description: Windows jump up from the bottom edge, overshoot a little and drop into place.
-- source: original
-- license: original
-- demo: windows
hl.curve("hop_out", { type = "bezier", points = { { 0.2, 1.6 }, { 0.3, 1 } } })
hl.animation({ leaf = "windowsIn", enabled = true, speed = 4, bezier = "hop_out", style = "slide bottom" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 2, bezier = "in", style = "slide bottom" })
