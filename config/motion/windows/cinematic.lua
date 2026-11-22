-- label: Cinematic
-- emoji: 🎬
-- category: Windows · Showy
-- slot: windows
-- tags: slow, cinematic, popin, dramatic
-- description: Windows swell up slowly from 40% size over almost a second, then shrink away.
-- source: original
-- license: original
-- demo: windows
hl.curve("cin_in", { type = "bezier", points = { { 0.22, 1 }, { 0.36, 1 } } })
hl.curve("cin_out", { type = "bezier", points = { { 0.64, 0 }, { 0.78, 0 } } })
hl.animation({ leaf = "windowsIn", enabled = true, speed = 9, bezier = "cin_in", style = "popin 40%" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 6, bezier = "cin_out", style = "popin 40%" })
