-- label: Wobble
-- emoji: 🍮
-- category: Windows · Bounce
-- slot: windows
-- tags: spring, wobble, jelly, popin
-- description: Windows pop from 40% size and wobble several times on a loose spring before they rest.
-- source: original
-- license: original
-- demo: windows
hl.curve("wobble_spring", { type = "spring", mass = 1, stiffness = 200, dampening = 9 })
hl.animation({ leaf = "windowsIn", enabled = true, speed = 1, spring = "wobble_spring", style = "popin 40%" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 2, bezier = "in", style = "popin 40%" })
