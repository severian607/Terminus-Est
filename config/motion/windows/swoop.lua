-- label: Swoop
-- emoji: 🦅
-- category: Windows · Showy
-- slot: windows
-- tags: overshoot, slide, left, workspaces
-- description: Windows swoop in from the left, overshoot, settle, and exit to the right.
-- source: original
-- license: original
-- demo: windows
hl.curve("swoop_out", { type = "bezier", points = { { 0.2, 1.5 }, { 0.3, 1 } } })
hl.animation({ leaf = "windowsIn", enabled = true, speed = 5, bezier = "swoop_out", style = "slide left" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 2, bezier = "in", style = "slide right" })
hl.animation({ leaf = "workspaces", enabled = true, speed = 4, bezier = "swoop_out", style = "slidefadevert 20%" })
