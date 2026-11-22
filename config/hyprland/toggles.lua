-- # Persistent desktop gap and aspect toggles
-- # Desktop helpers and the compositor share these state flags.
local home = assert(os.getenv("HOME"), "HOME is required")
local toggle_dir = home .. "/.local/state/terminus-est/toggles/hypr"

if o.file_exists(toggle_dir .. "/window-no-gaps.conf") then
  hl.config({
    general = { gaps_in = 0, gaps_out = 0, border_size = 0 },
    decoration = { rounding = 0 },
  })
end

if o.file_exists(toggle_dir .. "/single-window-aspect-ratio.conf") then
  hl.config({ layout = { single_window_aspect_ratio = { 1, 1 } } })
end

-- # Flags written by terminus-hyprland-monitor-internal, -internal-mirror and terminus-toggle-touchpad. They hold plain
-- # names, one per line, that are read as data and never executed: the output (or device) to act on, and for the
-- # mirror flag the external output on line 1 and the internal output it mirrors on line 2.
local function flag_lines(name)
  local file = io.open(toggle_dir .. "/" .. name .. ".conf", "r")
  if not file then
    return nil
  end
  local lines = {}
  for line in file:lines() do
    if line ~= "" then
      lines[#lines + 1] = line
    end
  end
  file:close()
  return lines
end

local internal_off = flag_lines("internal-monitor-disable")
if internal_off and internal_off[1] then
  hl.monitor({ output = internal_off[1], disabled = true })
end

local internal_mirror = flag_lines("internal-monitor-mirror")
if internal_mirror and internal_mirror[1] and internal_mirror[2] then
  hl.monitor({ output = internal_mirror[1], mode = "preferred", position = "auto", scale = 1, mirror = internal_mirror[2] })
end

local touchpad_off = flag_lines("touchpad-disabled")
if touchpad_off and touchpad_off[1] then
  hl.device({ name = touchpad_off[1], enabled = false })
end

-- # Screen shader modes (terminus-screenfx). The CLI composes the active effects into one generated shader and
-- # writes these state files. Shaders that displace samples (CRT curvature, glitch, wave) or animate leave
-- # stale regions on partial-damage redraws, so the CLI also sets screen-fx.damage to turn damage tracking off.
if o.file_exists(toggle_dir .. "/screen-fx.frag") then
  if o.file_exists(toggle_dir .. "/screen-fx.damage") then
    hl.config({ debug = { damage_tracking = 0 } })
  end
  hl.config({ decoration = { screen_shader = toggle_dir .. "/screen-fx.frag" } })
  -- # A hardware cursor sits on its own plane, outside the shader; software cursor gets shaded too.
  hl.config({ cursor = { no_hardware_cursors = 1 } })
end
