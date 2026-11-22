-- label: Genie
-- emoji: 🧞
-- category: Windows · Showy
-- slot: windows
-- tags: gnomed, overshoot, genie, slow
-- description: Windows balloon out from their centre past full size, settle, and collapse on close.
-- source: original
-- license: original
-- demo: windows
hl.curve("genie_out", { type = "bezier", points = { { 0.2, 1.5 }, { 0.3, 1 } } })
hl.animation({ leaf = "windowsIn", enabled = true, speed = 7, bezier = "genie_out", style = "gnomed" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 3, bezier = "in", style = "gnomed" })
