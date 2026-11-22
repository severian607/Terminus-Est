-- label: Blink
-- emoji: ⚡
-- category: Windows · Fast
-- slot: windows
-- tags: fast, instant, popin, 120ms
-- description: Windows appear in 120 ms with a barely visible 90% scale and vanish just as fast.
-- source: original
-- license: original
-- demo: windows
hl.animation({ leaf = "windowsIn", enabled = true, speed = 1.2, bezier = "out", style = "popin 90%" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 1, bezier = "in", style = "popin 90%" })
