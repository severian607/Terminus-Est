-- # Cursor theme: chosen in Terminus-Motion (SUPER + A, Cursor · Theme), remembered in
-- # ~/.local/state/terminus-est/cursor/theme as "NAME SIZE". Default: the Terminus Est sword at 36.
local theme, size = "terminus-est", "36"
local file = io.open(assert(os.getenv("HOME"), "HOME is required") .. "/.local/state/terminus-est/cursor/theme")
if file then
  local saved_theme, saved_size = (file:read("*l") or ""):match("^(%S+)%s+(%d+)$")
  if saved_theme then
    theme, size = saved_theme, saved_size
  end
  file:close()
end
hl.env("HYPRCURSOR_THEME", theme)
hl.env("HYPRCURSOR_SIZE", size)
hl.env("XCURSOR_THEME", theme)
hl.env("XCURSOR_SIZE", size)

hl.config({ cursor = { enable_hyprcursor = true, hide_on_key_press = false, no_hardware_cursors = 1 } })
