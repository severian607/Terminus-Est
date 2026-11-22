-- label: Rubber
-- emoji: 🧽
-- category: Windows · Bounce
-- slot: windows
-- tags: overshoot, rubber, elastic, popin
-- description: Windows overshoot to about 110% size when opening and settle back like rubber.
-- source: original
-- license: original
-- demo: windows
hl.curve("rubber_out", { type = "bezier", points = { { 0.34, 1.8 }, { 0.64, 1 } } })
hl.animation({ leaf = "windowsIn", enabled = true, speed = 4, bezier = "rubber_out", style = "popin 60%" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 2, bezier = "in", style = "popin 60%" })
