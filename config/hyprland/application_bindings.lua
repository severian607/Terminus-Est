-- # Personal application bindings.

-- # Terminals, editors, files and browsers.
o.bind("SUPER + RETURN",          "Terminal",          [=[uwsm-app -- ghostty --working-directory="$(terminus compat cmd-terminal-cwd)"]=])
o.bind("SUPER + ALT + RETURN",    "Tmux",              [=[uwsm-app -- ghostty --working-directory="$(terminus compat cmd-terminal-cwd)" -e bash -lc "tmux attach || tmux new -s Work"]=])
if o.command_exists("omp-launch-focused") then
  o.bind("SUPER + SHIFT + Q",       "Open OMP",          "omp-launch-focused")
end
o.bind("SUPER + SHIFT + E",       "Zed",               "uwsm-app -- zeditor")
o.bind("SUPER + R",               "Skwd-wall",         "uwsm-app -- skwd-wall-v2")
o.bind("SUPER + SHIFT + RETURN",  "Browser",           "terminus compat launch-browser")
if o.command_exists("yazi") then
  o.bind("SUPER + ALT + SHIFT + F", "File manager",      "ghostty -e yazi")
end
-- # Strata starts through its wrapper when this machine has one, else directly.
local strata = (o.command_exists("strata-launch") and "strata-launch") or (o.command_exists("strata") and "strata")
if strata then
  o.bind("SUPER + SHIFT + F",       "Strata (cwd)",      "uwsm-app -- " .. strata .. [=[ "$(terminus compat cmd-terminal-cwd)"]=])
end
o.bind("SUPER + SHIFT + ALT + B", "Browser (private)", "terminus compat launch-browser --private")

-- # Firefox: Ctrl+J/L switch tabs; Ctrl+Left/Right retain native word movement.
local function active_window_is_firefox()
  local window = hl.get_active_window()
  return window and window.class and window.class:lower() == "firefox"
end

local function send_shortcut(modifiers, key)
  hl.dispatch(hl.dsp.send_shortcut({ mods = modifiers, key = key }))
end

local function firefox_ctrl_tab_key(key, page_key)
  send_shortcut("CTRL", active_window_is_firefox() and page_key or key)
end

local function firefox_ctrl_page_key(key)
  if not active_window_is_firefox() then
    send_shortcut("CTRL", key)
  end
end

local function firefox_ctrl_arrow_key(key)
  send_shortcut(active_window_is_firefox() and "" or "CTRL", key)
end

o.bind("CTRL + J",     "Previous Firefox tab",              function() firefox_ctrl_tab_key("J", "Prior") end, { repeating = true })
o.bind("CTRL + L",     "Next Firefox tab",                  function() firefox_ctrl_tab_key("L", "Next") end, { repeating = true })
o.bind("CTRL + Prior", "Disable Ctrl+Page Up in Firefox",   function() firefox_ctrl_page_key("Prior") end, { repeating = true })
o.bind("CTRL + Next",  "Disable Ctrl+Page Down in Firefox", function() firefox_ctrl_page_key("Next") end, { repeating = true })
o.bind("CTRL + UP",    "Scroll up in Firefox",              function() firefox_ctrl_arrow_key("UP") end, { repeating = true })
o.bind("CTRL + DOWN",  "Scroll down in Firefox",            function() firefox_ctrl_arrow_key("DOWN") end, { repeating = true })

-- # Editors, notes and web apps.
o.bind("SUPER + SHIFT + N", "Editor",                     "terminus compat launch-editor")
-- # The Obsidian wrapper (transparent window) lives outside this repository; without it, plain obsidian.
local obsidian_wrapper_path = "/Projects/Obsidian-Gruvbox-Transparent/obsidian-transparent"
local obsidian_launcher
if o.file_exists(os.getenv("HOME") .. obsidian_wrapper_path) then
  obsidian_launcher = "$HOME" .. obsidian_wrapper_path
elseif o.command_exists("obsidian") then
  obsidian_launcher = "obsidian"
end
if obsidian_launcher then
  o.bind("SUPER + SHIFT + O", "Obsidian",                   [=[terminus compat launch-or-focus '^md\.obsidian\.Obsidian$' "uwsm-app -- ]=] .. obsidian_launcher .. '"')
end
if o.command_exists("obsidian") then
  o.bind("SUPER + PERIOD",    "Toggle Obsidian light/dark", "obsidian command id=theme:toggle-light-dark")
end

o.bind("SUPER + SHIFT + Y",        "YouTube",                    [=[terminus compat launch-webapp "https://youtube.com/" ]=])
o.bind("SUPER + SHIFT + ALT + G",  "WhatsApp",                   [=[terminus compat launch-or-focus-webapp WhatsApp "https://web.whatsapp.com/" ]=])
o.bind("SUPER + SHIFT + CTRL + G", "Google Messages",            [=[terminus compat launch-or-focus-webapp "Google Messages" "https://messages.google.com/web/conversations"]=])
o.bind("SUPER + SHIFT + X",        "X",                          [=[terminus compat launch-webapp "https://x.com/" ]=])
o.bind("SUPER + SHIFT + ALT + X",  "X Post",                     [=[terminus compat launch-webapp "https://x.com/compose/post"]=])
-- # The dictation helper ships with the hyprwhspr package.
local hyprwhspr_tray = "/usr/lib/hyprwhspr/config/hyprland/hyprwhspr-tray.sh"
if o.file_exists(hyprwhspr_tray) then
  o.bind("SUPER + V",                "Toggle Hyprwhspr recording", hyprwhspr_tray .. " record")
end

-- # Panels and pickers.
-- # Panels open on the main monitor when monitors.lua names one, else where the focus is.
local function on_primary_monitor(command)
  return function()
    if o.monitors.primary then
      hl.dispatch(hl.dsp.focus({ monitor = o.monitors.primary }))
    end
    hl.dispatch(hl.dsp.exec_cmd(command))
  end
end

o.bind("SUPER + SPACE",     "Toggle launcher on the main monitor",  on_primary_monitor("terminus-launcher toggle"))
o.bind("SUPER + I",         "Insert Unicode symbol",                "terminus-symbols")
o.bind("SUPER + D",         "Choose screen shader",                 "terminus-shaders")
o.bind("SUPER + A",         "Choose animations and effects",        "terminus-motion")
o.bind("SUPER + S",         "Choose audio effects",                 "terminus-audio")
o.bind("SUPER + H",         "Show SUPER keybindings on a keyboard", "terminus-keys")
o.bind("SUPER + SHIFT + R", "Rnote",                                [=[terminus compat launch-or-focus '^com\.github\.flxzt\.rnote$' "uwsm-app -- rnote"]=])

o.bind("SUPER + SHIFT + SPACE", "Toggle Terminus-Bar bar", "terminus-bar msg bar-toggle default " .. (o.monitors.primary or ""))

o.bind("SUPER + ESCAPE",       "Toggle Terminus-Bar menu on the main monitor", on_primary_monitor("terminus-bar msg panel-toggle control-center"))
o.bind("SUPER + CTRL + SPACE", "Wallpaper panel on the main monitor",          on_primary_monitor("terminus-bar msg panel-toggle wallpaper"))

-- # Screensaver: while its submap is active, any key or mouse input dismisses it.
o.bind("SUPER + L", "Screensaver", "terminus-screensaver force")

local screensaver_dismiss_inputs = {
  "catchall",
  "mouse:272",
  "mouse:273",
  "mouse:274",
  "mouse:275",
  "mouse_up",
  "mouse_down",
  "mouse_left",
  "mouse_right",
}
local dismiss_screensaver = hl.dsp.exec_cmd("terminus-screensaver --stop")
hl.define_submap("screensaver", function()
  for _, input in ipairs(screensaver_dismiss_inputs) do
    hl.bind(input, dismiss_screensaver, { ignore_mods = true })
  end
end)

-- # Super+Tab opens the Terminus-Bar window switcher; releasing Super activates the selection, Tab again cycles.
o.bind("SUPER + TAB", "Window switcher", "terminus-bar msg window-switcher hold")

o.bind("SUPER + semicolon", "Toggle workspace layout", "terminus compat hyprland-workspace-layout-toggle")
