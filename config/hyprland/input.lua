-- # Keyboard, mouse, touchpad and tablet
local desktop_keyboard_options = "compose:caps"

hl.config({
  input = {
    kb_layout = "us",
    kb_variant = "",
    kb_model = "",
    kb_options = desktop_keyboard_options,
    kb_rules = "",
    follow_mouse = 0,
    sensitivity = 0,
    accel_profile = "flat",
    float_switch_override_focus = 0,
    repeat_rate = 40,
    repeat_delay = 250,
    numlock_by_default = true,
    touchpad = {
      natural_scroll = false,
      clickfinger_behavior = true,
      scroll_factor = 0.4,
    },
    tablet = {
      output = o.monitors.primary,
      relative_input = false,
    },
  },
  misc = {
    key_press_enables_dpms = true,
    mouse_move_enables_dpms = true,
  },
})

-- Swap the physical Caps Lock/Escape keys only in the focused Neovim terminal.
-- Desktop panels retain the normal layout even while Neovim is underneath.
local function sync_neovim_keyboard()
  local window = hl.get_active_window()
  local neovim = window
    and window.class:match("^com%.mitchellh%.ghostty") ~= nil
    and window.title:match(" %- NVIM$") ~= nil

  if neovim then
    for _, layer in ipairs(hl.get_layers()) do
      if layer.mapped and layer.interactivity ~= 0 then
        neovim = false
        break
      end
    end
  end

  local options = neovim and "caps:swapescape" or desktop_keyboard_options
  if hl.get_config("input.kb_options") ~= options then
    hl.config({ input = { kb_options = options } })
    hl.exec_scheduled_prop_refresh_immediately()
  end
end

hl.on("window.active", sync_neovim_keyboard)
hl.on("window.title", sync_neovim_keyboard)
hl.on("layer.opened", sync_neovim_keyboard)
hl.on("layer.closed", sync_neovim_keyboard)
hl.on("config.reloaded", sync_neovim_keyboard)
hl.on("hyprland.start", sync_neovim_keyboard)
