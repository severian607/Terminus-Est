-- label: Snap
-- emoji: 🫰
-- category: Windows · Fast
-- slot: windows
-- tags: fast, gnomed, snap
-- description: Windows snap open from their centre in 150 ms and snap shut again.
-- source: original
-- license: original
-- demo: windows
hl.animation({ leaf = "windowsIn", enabled = true, speed = 1.5, bezier = "out", style = "gnomed" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 1, bezier = "in", style = "gnomed" })
