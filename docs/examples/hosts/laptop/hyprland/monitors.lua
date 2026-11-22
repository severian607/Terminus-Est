-- # Physical displays of this machine

-- The only place that names this machine's outputs; other Lua modules read o.monitors.
o.monitors = { primary = "eDP-1", secondary = "HDMI-A-1" }
hl.env("GDK_SCALE", "1")

-- Built-in panel and anything docked: preferred mode, 100% scale.
hl.monitor({ output = "", mode = "preferred", position = "auto", scale = 1 })

-- An external display on HDMI mirrors the panel, so it shows whatever workspace is active.
hl.monitor({ output = o.monitors.secondary, mode = "preferred", position = "auto", scale = 1, mirror = o.monitors.primary })
