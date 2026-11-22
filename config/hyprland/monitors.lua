-- # Physical displays

-- The only place that names this machine's outputs; other Lua modules read o.monitors.
o.monitors = { primary = "DP-2", secondary = "HDMI-A-1" }
hl.env("GDK_SCALE", "1")

hl.monitor({ output = o.monitors.primary, mode = "2560x1440@144", position = "auto", scale = 1 })
hl.monitor({ output = o.monitors.secondary, mode = "1920x1080@75", position = "auto", scale = 1, transform = 3 })
