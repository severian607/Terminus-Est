local home = assert(os.getenv("HOME"), "HOME is required")
local desktop_path = home .. "/Projects/Terminus-Est/modules/desktop"

-- # Hyprland can be started without UWSM importing ~/.config/uwsm/env. Keep
-- # Terminus-Est commands available to Lua exec bindings regardless of launch path.
local path = os.getenv("PATH") or "/usr/bin"
local function path_contains(directory)
  return (":" .. path .. ":"):find(":" .. directory .. ":", 1, true) ~= nil
end

if not path_contains(desktop_path .. "/bin") then
  path = desktop_path .. "/bin:" .. path
end
if not path_contains(home .. "/.local/bin") then
  path = path .. ":" .. home .. "/.local/bin"
end

path = home .. "/Projects/Terminus-Est/bin:" .. path
hl.env("TERMINUS_PATH", home .. "/Projects/Terminus-Est")
hl.env("TERMINUS_DESKTOP_PATH", desktop_path)
hl.env("PATH", path)

-- # Preserve the current theme's Gum variables. Legacy files escape # as ##.
local gum = home .. "/.config/terminus-est/current/theme/gum.env.conf"
local file = io.open(gum, "r")
if file then
  for line in file:lines() do
    local key, value = line:match("^%s*env%s*=%s*([^,]+),%s*(.-)%s*$")
    if key and value then
      value = value:gsub("^##", "#")
      hl.env(key, value)
    end
  end
  file:close()
end

hl.env("XCURSOR_SIZE", "24")
hl.env("HYPRCURSOR_SIZE", "24")
hl.env("GDK_BACKEND", "wayland,x11,*")
hl.env("QT_QPA_PLATFORM", "wayland;xcb")
hl.env("QT_QPA_PLATFORMTHEME", "qt6ct")
hl.env("MOZ_ENABLE_WAYLAND", "1")
hl.env("ELECTRON_OZONE_PLATFORM_HINT", "wayland")
hl.env("OZONE_PLATFORM", "wayland")
hl.env("XDG_SESSION_TYPE", "wayland")
hl.env("XDG_CURRENT_DESKTOP", "Hyprland")
hl.env("XDG_SESSION_DESKTOP", "Hyprland")
hl.env("XDG_MENU_PREFIX", "arch-")
hl.env("XCOMPOSEFILE", home .. "/.XCompose")

hl.config({
  xwayland = { force_zero_scaling = true },
  ecosystem = { no_update_news = true },
})
