-- # Desktop media, navigation and utilities

-- # Media.
o.bind("XF86AudioRaiseVolume", "Volume up", "terminus compat swayosd-client --output-volume raise", { locked = true, repeating = true })
o.bind("XF86AudioLowerVolume", "Volume down", "terminus compat swayosd-client --output-volume lower", { locked = true, repeating = true })
o.bind("XF86AudioMute", "Mute", "terminus compat swayosd-client --output-volume mute-toggle", { locked = true, repeating = true })
o.bind("XF86AudioMicMute", "Mute microphone", "terminus compat audio-input-mute", { locked = true, repeating = true })
o.bind("XF86MonBrightnessUp", "Brightness up", "terminus compat brightness-display +5%", { locked = true, repeating = true })
o.bind("XF86MonBrightnessDown", "Brightness down", "terminus compat brightness-display 5%-", { locked = true, repeating = true })
o.bind("SHIFT + XF86MonBrightnessUp", "Brightness maximum", "terminus compat brightness-display 100%", { locked = true, repeating = true })
o.bind("SHIFT + XF86MonBrightnessDown", "Brightness minimum", "terminus compat brightness-display 1%", { locked = true, repeating = true })
o.bind("XF86KbdBrightnessUp", "Keyboard brightness up", "terminus compat brightness-keyboard up", { locked = true, repeating = true })
o.bind("XF86KbdBrightnessDown", "Keyboard brightness down", "terminus compat brightness-keyboard down", { locked = true, repeating = true })
o.bind("XF86KbdLightOnOff", "Keyboard backlight cycle", "terminus compat brightness-keyboard cycle", { locked = true })
o.bind("XF86TouchpadToggle", "Toggle touchpad", "terminus compat toggle-touchpad", { locked = true })
o.bind("XF86TouchpadOn", "Enable touchpad", "terminus compat toggle-touchpad on", { locked = true })
o.bind("XF86TouchpadOff", "Disable touchpad", "terminus compat toggle-touchpad off", { locked = true })
o.bind("ALT + XF86AudioRaiseVolume", "Volume up precise", "terminus compat swayosd-client --output-volume +1", { locked = true, repeating = true })
o.bind("ALT + XF86AudioLowerVolume", "Volume down precise", "terminus compat swayosd-client --output-volume -1", { locked = true, repeating = true })
o.bind("ALT + XF86MonBrightnessUp", "Brightness up precise", "terminus compat brightness-display +1%", { locked = true, repeating = true })
o.bind("ALT + XF86MonBrightnessDown", "Brightness down precise", "terminus compat brightness-display 1%-", { locked = true, repeating = true })
o.bind("XF86AudioNext", "Next track", "terminus compat swayosd-client --playerctl next", { locked = true })
o.bind("XF86AudioPause", "Pause", "terminus compat swayosd-client --playerctl play-pause", { locked = true })
o.bind("XF86AudioPlay", "Play", "terminus compat swayosd-client --playerctl play-pause", { locked = true })
o.bind("XF86AudioPrev", "Previous track", "terminus compat swayosd-client --playerctl previous", { locked = true })
o.bind("SUPER + XF86AudioMute", "Switch audio output", "terminus compat audio-output-switch", { locked = true })

-- # Clipboard.
o.bind("SUPER + C", "Universal copy", hl.dsp.send_shortcut({ mods = "CTRL", key = "Insert", window = "activewindow" }))
o.bind("SUPER + X", "Universal cut", hl.dsp.send_shortcut({ mods = "CTRL", key = "X", window = "activewindow" }))
o.bind("SUPER + ALT + V", "Clipboard manager", "terminus-bar msg panel-toggle clipboard")

-- # Tiling and focus.
o.bind("SUPER + W", "Close window", hl.dsp.window.close())
o.bind("CTRL + ALT + DELETE", "Close all windows", "terminus compat hyprland-window-close-all")
o.bind("SUPER + J", "Toggle window split", hl.dsp.layout("togglesplit"))
o.bind("SUPER + P", "Pseudo window", hl.dsp.window.pseudo())
o.bind("SUPER + T", "Toggle window floating/tiling", hl.dsp.window.float({ action = "toggle" }))
o.bind("SUPER + ALT + F", "Full screen", hl.dsp.window.fullscreen({ mode = "fullscreen" }))
o.bind("SUPER + CTRL + F", "Full screen", hl.dsp.window.fullscreen({ mode = "fullscreen" }))
o.bind("SUPER + F", "Full width", hl.dsp.window.fullscreen({ mode = "maximized" }))
o.bind("SUPER + O", "Pop window out (float & pin)", "terminus compat hyprland-window-pop")

o.bind("SUPER + LEFT", "Focus on left window", hl.dsp.focus({ direction = "l" }))
o.bind("SUPER + RIGHT", "Focus on right window", hl.dsp.focus({ direction = "r" }))
o.bind("SUPER + UP", "Focus on above window", hl.dsp.focus({ direction = "u" }))
o.bind("SUPER + DOWN", "Focus on below window", hl.dsp.focus({ direction = "d" }))

for workspace = 1, 10 do
  local key = "code:" .. tostring(workspace + 9)
  o.bind("SUPER + " .. key, "Switch to workspace " .. workspace, hl.dsp.focus({ workspace = tostring(workspace) }))
  o.bind("SUPER + SHIFT + " .. key, "Move window to workspace " .. workspace, hl.dsp.window.move({ workspace = tostring(workspace) }))
  o.bind("SUPER + SHIFT + ALT + " .. key, "Move window silently to workspace " .. workspace, hl.dsp.window.move({ workspace = tostring(workspace), follow = false }))
end

o.bind("SUPER + SHIFT + TAB", "Previous workspace", hl.dsp.focus({ workspace = "e-1" }))
o.bind("SUPER + CTRL + TAB", "Former workspace", hl.dsp.focus({ workspace = "previous" }))

o.bind("SUPER + SHIFT + ALT + LEFT", "Move workspace to left monitor", hl.dsp.workspace.move({ monitor = "l" }))
o.bind("SUPER + SHIFT + ALT + RIGHT", "Move workspace to right monitor", hl.dsp.workspace.move({ monitor = "r" }))
o.bind("SUPER + SHIFT + ALT + UP", "Move workspace to up monitor", hl.dsp.workspace.move({ monitor = "u" }))
o.bind("SUPER + SHIFT + ALT + DOWN", "Move workspace to down monitor", hl.dsp.workspace.move({ monitor = "d" }))

o.bind("SUPER + SHIFT + LEFT", "Move window left", hl.dsp.window.move({ direction = "l" }))
o.bind("SUPER + SHIFT + RIGHT", "Move window right", hl.dsp.window.move({ direction = "r" }))
o.bind("SUPER + SHIFT + UP", "Swap window up", hl.dsp.window.swap({ direction = "u" }))
o.bind("SUPER + SHIFT + DOWN", "Swap window down", hl.dsp.window.swap({ direction = "d" }))

o.bind("ALT + TAB", "Focus on next window", hl.dsp.window.cycle_next())
o.bind("ALT + SHIFT + TAB", "Focus on previous window", hl.dsp.window.cycle_next({ next = false }))
o.bind("ALT + TAB", "Reveal active window on top", hl.dsp.window.bring_to_top())
o.bind("ALT + SHIFT + TAB", "Reveal active window on top", hl.dsp.window.bring_to_top())
o.bind("CTRL + ALT + TAB", "Focus on next monitor", hl.dsp.focus({ monitor = "+1" }))
o.bind("CTRL + ALT + SHIFT + TAB", "Focus on previous monitor", hl.dsp.focus({ monitor = "-1" }))

o.bind("SUPER + code:20", "Expand window left", hl.dsp.window.resize({ x = -100, y = 0, relative = true }))
o.bind("SUPER + code:21", "Shrink window left", hl.dsp.window.resize({ x = 100, y = 0, relative = true }))
o.bind("SUPER + SHIFT + code:20", "Shrink window up", hl.dsp.window.resize({ x = 0, y = -100, relative = true }))
o.bind("SUPER + SHIFT + code:21", "Expand window down", hl.dsp.window.resize({ x = 0, y = 100, relative = true }))
o.bind("SUPER + mouse_down", "Scroll active workspace forward", hl.dsp.focus({ workspace = "e+1" }))
o.bind("SUPER + mouse_up", "Scroll active workspace backward", hl.dsp.focus({ workspace = "e-1" }))
o.bind("SUPER + mouse:272", "Move window", hl.dsp.window.drag(), { mouse = true })
o.bind("SUPER + mouse:273", "Resize window", hl.dsp.window.resize(), { mouse = true })

o.bind("SUPER + code:61", "Cycle monitor scaling", "terminus compat hyprland-monitor-scaling-cycle")
o.bind("SUPER + ALT + code:61", "Cycle monitor scaling backwards", "terminus compat hyprland-monitor-scaling-cycle --reverse")

-- # Direct Unicode entry; the symbol panel reads the same shortcut reminder.
local symbol_keys = {
  ["-"] = "minus", ["="] = "equal",
  ["<"] = "SHIFT + comma", [">"] = "SHIFT + period",
  ["S"] = "SHIFT + S",
}
for line in io.lines(os.getenv("HOME") .. "/.config/terminus-est/symbol-shortcuts.tsv") do
  local key, symbol, description = line:match("^([^\t]+)\t([^\t]+)\t(.+)$")
  o.bind("ALT + " .. (symbol_keys[key] or key), "Insert " .. symbol .. " — " .. description,
    "wtype -- " .. string.format("%q", symbol))
end

-- # Menus and utilities.
o.bind("SUPER + CTRL + E", "Emoji picker (Terminus-Bar launcher)", "terminus-bar msg panel-toggle launcher /emo")
o.bind("SUPER + CTRL + C", "Capture menu", "terminus compat menu capture")
o.bind("SUPER + CTRL + O", "Toggle menu", "terminus compat menu toggle")
o.bind("SUPER + CTRL + H", "Hardware menu", "terminus compat menu hardware")
o.bind("SUPER + ALT + SPACE", "Settings", "terminus-settings")
o.bind("SUPER + SHIFT + code:201", "Settings", "terminus-settings")
o.bind("XF86PowerOff", "Power menu", "terminus compat menu system", { locked = true })
o.bind("SUPER + CTRL + L", "Toggle light/dark (bar, Zed, Obsidian)", "$HOME/Projects/Terminus-Est/tools/terminus-theme-toggle")
o.bind("SUPER + K", "Pick screen color", "hyprpicker --autocopy --format=hex --lowercase-hex")
if o.command_exists("gnome-calculator") then
  o.bind("XF86Calculator", "Calculator", "gnome-calculator")
end

o.bind("SUPER + SHIFT + CTRL + SPACE", "Settings", "terminus-settings")
o.bind("SUPER + BACKSPACE", "Toggle window transparency", "terminus-transparency")
o.bind("SUPER + SHIFT + BACKSPACE", "Toggle window gaps", "terminus compat hyprland-window-gaps-toggle")
o.bind("SUPER + CTRL + BACKSPACE", "Toggle single-window square aspect", "terminus compat hyprland-window-single-square-aspect-toggle")

o.bind("SUPER + COMMA", "Dismiss last notification", "makoctl dismiss")
o.bind("SUPER + SHIFT + COMMA", "Dismiss all notifications", "makoctl dismiss --all")
o.bind("SUPER + CTRL + COMMA", "Toggle silencing notifications", "terminus compat toggle-notification-silencing")
o.bind("SUPER + ALT + COMMA", "Invoke last notification", "makoctl invoke")
o.bind("SUPER + SHIFT + ALT + COMMA", "Restore last notification", "makoctl restore")

o.bind("SUPER + CTRL + I", "Toggle locking on idle", "terminus compat toggle-idle")
o.bind("SUPER + CTRL + N", "Toggle nightlight", "terminus compat toggle-nightlight")
o.bind("SUPER + CTRL + Delete", "Toggle laptop display", "terminus compat hyprland-monitor-internal toggle")
o.bind("SUPER + CTRL + ALT + Delete", "Toggle laptop display mirroring", "terminus compat hyprland-monitor-internal-mirror toggle")
o.bind("switch:on:Lid Switch", nil, "terminus compat hw-external-monitors && terminus compat hyprland-monitor-internal off", { locked = true })
o.bind("switch:off:Lid Switch", nil, "terminus compat hyprland-monitor-internal on", { locked = true })

o.bind("PRINT", "Screenshot", "terminus compat capture-screenshot")
o.bind("ALT + PRINT", "Screen recording (start/stop)", "terminus compat capture-screenrecording")
o.bind("SUPER + PRINT", "Color picker", "pkill hyprpicker || hyprpicker -a")
o.bind("SUPER + CTRL + PRINT", "Extract text (OCR) from screenshot", "terminus compat capture-text-extraction")
o.bind("SUPER + CTRL + PERIOD", "Transcode", "terminus compat transcode")
o.bind("SUPER + CTRL + R", "Set reminder", "terminus compat menu reminder-set")
o.bind("SUPER + CTRL + ALT + R", "Show reminders", "terminus compat reminder show")
o.bind("SUPER + SHIFT + CTRL + R", "Clear reminders", "terminus compat reminder clear")

o.bind("SUPER + CTRL + ALT + T", "Show time", [=[notify-send -u low "    $(date +"%A %H:%M  ·  %d %B %Y  ·  Week %V")"]=])
o.bind("SUPER + CTRL + ALT + B", "Show battery remaining", [=[notify-send -u low "$(terminus compat battery-status)"]=])
o.bind("SUPER + CTRL + ALT + W", "Show weather", [=[notify-send -u low "$(terminus compat weather-status)"]=])
o.bind("SUPER + ALT + A", "Audio controls", "terminus-bar msg panel-toggle control-center audio")
o.bind("SUPER + ALT + B", "Bluetooth controls", "terminus-bar msg panel-toggle control-center bluetooth")
o.bind("SUPER + ALT + W", "Wifi controls", "terminus-bar msg panel-toggle control-center network")
o.bind("SUPER + ALT + C", "CPU panel (cat)", "terminus-bar msg panel-toggle terminus/vitals:vitals cpu")
o.bind("SUPER + ALT + G", "GPU panel (mouse)", "terminus-bar msg panel-toggle terminus/vitals:vitals gpu")
o.bind("SUPER + ALT + M", "Memory panel", "terminus-bar msg panel-toggle terminus/vitals:vitals mem")
o.bind("SUPER + ALT + K", "Calendar panel (clock)", "terminus-bar msg panel-toggle calendar")
o.bind("SUPER + ALT + D", "Display panel (brightness)", "terminus-bar msg panel-toggle brightness")
o.bind("SUPER + ALT + Y", "Weather panel", "terminus-bar msg panel-toggle weather")
o.bind("SUPER + ALT + P", "Power panel (battery)", "terminus-bar msg panel-toggle control-center power")
o.bind("SUPER + ALT + S", "Media panel", "terminus-bar msg panel-toggle media")
o.bind("SUPER + CTRL + T", "Activity", "terminus compat launch-tui btop")

o.bind("SUPER + CTRL + Z", "Zoom in", [=[hyprctl eval "hl.config({ cursor = { zoom_factor = $(hyprctl getoption cursor:zoom_factor -j | jq '.float + 1') } })"]=])
o.bind("SUPER + CTRL + ALT + Z", "Reset zoom", [=[hyprctl eval 'hl.config({ cursor = { zoom_factor = 1 } })']=])
o.bind("SUPER + CTRL + SHIFT + 1", "Toggle LSD screen mode", "terminus-screenfx lsd")
o.bind("SUPER + CTRL + SHIFT + 2", "Toggle CRT screen mode", "terminus-screenfx crt")
o.bind("SUPER + CTRL + SHIFT + 3", "Toggle invert screen mode", "terminus-screenfx invert")
o.bind("SUPER + CTRL + SHIFT + 4", "Toggle glitch screen mode", "terminus-screenfx glitch")
o.bind("SUPER + CTRL + SHIFT + 5", "Toggle Matrix screen mode", "terminus-screenfx matrix")
o.bind("SUPER + CTRL + SHIFT + 6", "Toggle wave screen mode", "terminus-screenfx wave")
o.bind("SUPER + CTRL + SHIFT + 7", "Toggle cyberpunk screen mode", "terminus-screenfx cyberpunk")
o.bind("SUPER + B", "Choose keyboard and mouse sound pack", "terminus-keysounds")
o.bind("SUPER + M", "Mood sounds play/pause", "terminus-mechvibes mood")
