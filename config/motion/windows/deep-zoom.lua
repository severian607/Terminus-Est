-- label: Deep zoom
-- emoji: 🔍
-- category: Windows · Pop
-- slot: windows
-- tags: zoom, popin, scale, strong
-- description: Windows fly out from a tiny 25% size to full size and shrink back to a dot when closed.
-- source: original
-- license: original
-- demo: windows
hl.animation({ leaf = "windowsIn", enabled = true, speed = 4, bezier = "out", style = "popin 25%" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 2.5, bezier = "in", style = "popin 25%" })
