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
