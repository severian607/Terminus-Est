-- # Global window behavior
o.window(".*", { suppress_event = "maximize" })
o.window(".*", { tag = "+default-opacity" })

o.window({
  class = "^$",
  title = "^$",
  xwayland = true,
  float = true,
  fullscreen = false,
  pin = false,
}, { no_focus = true })

-- # Application behavior

-- # Password managers.
o.window("^(1[pP]assword)$", { no_screen_share = true, tag = "+floating-window" })
o.window("^(Bitwarden)$", { no_screen_share = true, tag = "+floating-window" })
o.window("chrome-nngceckbapebfimnlniiiahkandclblb-Default", { no_screen_share = true, tag = "+floating-window" })

-- # Browsers.
o.window("((google-)?[cC]hrom(e|ium)|[bB]rave-browser|[mM]icrosoft-edge|Vivaldi-stable|helium)", { tag = "+chromium-based-browser" })
o.window("([fF]irefox|zen|librewolf)", { tag = "+firefox-based-browser" })
o.window({ tag = "chromium-based-browser" }, { tag = "-default-opacity", tile = true, })
o.window({ tag = "firefox-based-browser" }, { tag = "-default-opacity", })
o.window("(chrome-youtube\\.com__-Default|chrome-app\\.zoom\\.us__wc_home-Default)", { tag = "-chromium-based-browser" })
o.window("(chrome-youtube\\.com__-Default|chrome-app\\.zoom\\.us__wc_home-Default)", { tag = "-default-opacity" })
o.window({ title = ".*is sharing.*" }, { workspace = "special silent" })


-- # JetBrains, LocalSend and PiP.
o.window("^(jetbrains-.*)$", { no_follow_mouse = true })
o.window("(Share|localsend)", { float = true, center = true })
o.window("localsend", { size = { 1100, 700 } })

o.window({ title = "(Picture.?in.?[Pp]icture)" }, { tag = "+pip" })
o.window({ tag = "pip" }, {
  tag = "-default-opacity",
  float = true,
  pin = true,
  size = { 600, 338 },
  keep_aspect_ratio = true,

  move = { "(monitor_w-window_w-40)", "(monitor_h*0.04)" },
})

-- # Games and streaming.
o.window("qemu", { tag = "-default-opacity", })
o.window("com.libretro.RetroArch", { fullscreen = true, tag = "-default-opacity", idle_inhibit = "fullscreen" })
o.window("steam", { float = true, idle_inhibit = "fullscreen" })
o.window({ class = "steam", title = "Steam" }, { center = true, size = { 1100, 700 } })
o.window("steam.*", { tag = "-default-opacity", })
o.window({ class = "steam", title = "Friends List" }, { size = { 460, 800 } })
o.window("GeForceNOW", { idle_inhibit = "fullscreen" })
o.window("com.moonlight_stream.Moonlight", { fullscreen = true, idle_inhibit = "fullscreen" })

-- # Shared floating-window behaviour.
o.window({ tag = "floating-window" }, { float = true })
o.window({ tag = "floating-window" }, { center = true })
o.window({ tag = "floating-window" }, { size = { 875, 600 } })

o.window("(org.terminus.bluetui|org.terminus.impala|org.terminus.wiremix|org.terminus.btop|org.terminus.terminal|org.terminus.bash|org.codeberg.dnkl.foot|org.gnome.NautilusPreviewer|org.gnome.Evince|com.gabm.satty|Terminus-Est|About|TUI.float|imv|mpv)", { tag = "+floating-window" })
o.window({
  class = "(xdg-desktop-portal-gtk|sublime_text|DesktopEditors)",
  title = "^(Open.*Files?|Open [Ff]older.*|Save.*Files?|Save.*As|Save|All Files|.*wants to (open|save).*|[Cc]hoose.*)",
}, { tag = "+floating-window" })
o.window("org.gnome.Calculator", { float = true })

-- # Screensaver and opaque media.
o.window("org.terminus.screensaver", { fullscreen = true, float = true, animation = "slide" })
o.window("^(zoom|vlc|mpv|org.kde.kdenlive|com.obsproject.Studio|com.github.PintaProject.Pinta|imv|org.gnome.NautilusPreviewer)$", { tag = "-default-opacity", })
o.window({ class = "^md\\.obsidian\\.Obsidian$" }, { tag = "-default-opacity" })
o.window({ class = "^dev\\.zed\\.Zed$" }, { tag = "-default-opacity" })
o.window({ class = "^io\\.github\\.lgse\\.Strata$" }, { tag = "-default-opacity" })
o.window({ tag = "noidle" }, { idle_inhibit = "always" })

-- # App-specific details.
o.window("org.telegram.desktop", { focus_on_activate = false })
o.window({ class = "^Typora$", title = "^Print$" }, { float = true, center = true })
o.window("(Alacritty|kitty|com.mitchellh.ghostty|foot)", { tag = "+terminal" })
o.window({ tag = "terminal" }, { tag = "-default-opacity", })

-- # Webcam overlay.
o.window({ title = "WebcamOverlay" }, {
  float = true,
  pin = true,
  no_initial_focus = true,
  no_dim = true,
  move = { "(monitor_w-window_w-40)", "(monitor_h-window_h-40)" },
})

-- # Per-application scrolling
o.window("(Alacritty|kitty|foot)", { scroll_touchpad = 1.5 })
o.window("com.mitchellh.ghostty", { scroll_touchpad = 0.2 })

-- # OMP placement
if o.monitors.secondary then
  o.window("^com\\.mitchellh\\.ghostty\\.omp$", { monitor = o.monitors.secondary })
end

-- # Workspace placement
for workspace = 1, 4 do
  hl.workspace_rule({ workspace = tostring(workspace), monitor = o.monitors.primary })
end
if o.monitors.secondary then
  hl.workspace_rule({ workspace = "5", monitor = o.monitors.secondary })
end
