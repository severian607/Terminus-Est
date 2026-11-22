-- label: Spring pop
-- emoji: 🪀
-- category: Windows · Pop
-- slot: windows
-- tags: spring, physics, popin
-- description: Windows pop up from 50% size on a spring and settle with a tiny wobble.
-- source: https://github.com/LinuxBeginnings/Hyprland-Dots (animations/Spring-curves.lua)
-- license: GPL-3.0
-- demo: windows
hl.curve("springpop_fast", { type = "spring", mass = 1, stiffness = 280, dampening = 26 })
hl.animation({ leaf = "windowsIn", enabled = true, speed = 1, spring = "springpop_fast", style = "popin 50%" })
hl.animation({ leaf = "windowsOut", enabled = true, speed = 1, spring = "springpop_fast", style = "popin" })
