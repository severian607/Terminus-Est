-- label: Pop
-- emoji: 🫧
-- category: Windows · Pop
-- slot: windows
-- tags: default, popin, scale, fade
-- description: Scale up from 90% and fade in. The Terminus-Est default, on the shared curves.
-- source: original
-- license: original
hl.animation({ leaf = "windowsIn", enabled = true, speed = 3, bezier = "out", style = "popin 90%" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 2, bezier = "in", style = "popin 90%" })
