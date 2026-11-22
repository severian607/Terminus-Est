-- # Central visual policy
-- # Edit values with Terminus-Settings; generated/appearance.lua is the only value source.
-- # Keep user-openable opacity rules together below. Rule order is significant.
local home = assert(os.getenv("HOME"), "HOME is required")
local appearance_path = home .. "/.config/terminus-est/generated/appearance.lua"

-- # Built-in values, equal to the defaults in lib/settings.py. They keep the rest of the configuration (and with it
-- # the autostart that brings up the bar) loading when the generated file is missing or broken.
local appearance_defaults = {
  background_opacity = 0.42,
  active_opacity = 1.0,
  inactive_opacity = 0.97,
  default_active_opacity = 0.97,
  default_inactive_opacity = 0.9,
  global_active_opacity = 0.97,
  global_inactive_opacity = 0.95,
  blur = true,
  blur_strength = 6,
  brightness = 0.42,
  contrast = 0.87,
  dim_inactive = false,
  dim_strength = 0.5,
  bar_opacity = 1.0,
  background = "#0b0b0b",
  accent = "#86d993",
  active_border = "#86d993",
  inactive_border = "#262626",
  foreground = "#ececec",
  muted = "#8c8c8c",
  alert = "#e8a07a",
  glass_opacity = 0.46,
  chrome_opacity = 0.28,
  popover_opacity = 0.9,
  ui_font = "MonaspiceNe Nerd Font",
}
local appearance_warned = false
local function load_appearance()
  local loaded_ok, loaded = pcall(dofile, appearance_path)
  if loaded_ok and type(loaded) == "table" then
    appearance_warned = false
    return setmetatable(loaded, { __index = appearance_defaults })
  end
  if not appearance_warned then
    appearance_warned = true
    hl.notification.create({
      text = "Terminus-Est: generated/appearance.lua is missing or broken; using built-in looks. Run terminus-settings apply.",
      timeout = 10000,
      icon = "warning",
    })
  end
  return appearance_defaults
end
local appearance = load_appearance()
local function rgba(color, alpha)
  return "rgba(" .. color:sub(2) .. alpha .. ")"
end
local function rgb(color)
  return "rgb(" .. color:sub(2) .. ")"
end

-- # Colours live in one place. terminus-settings calls o.apply_colors() through
-- # `hyprctl eval` when only colours changed, so a wallpaper change recolours the
-- # borders without reloading the configuration (a reload drops runtime state).
function o.apply_colors()
  local colors = load_appearance()
  -- # Recessed edge (after denonmaryland/omarchy-dotfiles, dark variant): a black cut along the
  -- # top fading to a faint lit lower lip, plus a soft inner glow of black that sinks the window.
  local function a8(alpha)
    return string.format("%02x", math.floor(math.max(0, math.min(1, alpha)) * 255 + 0.5))
  end
  local depth, lip_alpha = 0.896, 0.64 * 0.2
  local function edge(scale)
    local a = depth * 0.85 * scale
    return {
      colors = {
        "rgba(000000" .. a8(a * 0.85) .. ")",
        "rgba(000000" .. a8(a * 0.15) .. ")",
        "rgba(ffffff" .. a8(lip_alpha * scale) .. ")",
      },
      angle = 90,
    }
  end
  local function inset(scale)
    local a = depth * 0.85 * scale
    return {
      colors = {
        "rgba(000000" .. a8(a) .. ")",
        "rgba(000000" .. a8(a * 0.85) .. ")",
        "rgba(000000" .. a8(a * 0.7) .. ")",
      },
      angle = 90,
    }
  end
  local active = edge(1)
  local inactive = edge(0.75)
  local locked = rgba(colors.inactive_border, "cc")
  hl.config({
    general = { col = { active_border = active, inactive_border = inactive } },
    decoration = {
      glow = { enabled = true, range = 9, render_power = 2, color = inset(0.6), color_inactive = inset(0.45) },
    },
    group = {
      col = {
        border_active = active,
        border_inactive = inactive,
        border_locked_active = locked,
        border_locked_inactive = locked,
      },
      groupbar = {
        text_color = rgb(colors.background),
        text_color_inactive = rgb(colors.muted),
        col = {
          active = rgb(colors.accent),
          inactive = rgb(colors.inactive_border),
          locked_active = rgb(colors.alert),
          locked_inactive = rgb(colors.inactive_border),
        },
      },
    },
  })
end

local solid = "1 1"
local solid_override = "1 override 1 override" -- absolute: not multiplied by the global opacity
local preserved_opacity = tostring(appearance.active_opacity) .. " " .. tostring(appearance.inactive_opacity)
local default_opacity = tostring(appearance.default_active_opacity) .. " " .. tostring(appearance.default_inactive_opacity)

-- # Current frame, blur, shadow and grouping geometry
hl.config({
  general = {
    gaps_in = 8,
    gaps_out = 16,
    border_size = 0,
    resize_on_border = false,
    allow_tearing = false,
    layout = "dwindle",
  },
  decoration = {
    active_opacity = appearance.global_active_opacity,
    inactive_opacity = appearance.global_inactive_opacity,
    dim_inactive = appearance.dim_inactive,
    dim_strength = appearance.dim_strength,
    rounding = 20,
    blur = {
      enabled = appearance.blur,
      size = appearance.blur_strength,
      passes = 3,
      special = true,
      ignore_opacity = true,
      new_optimizations = true,
      xray = false,
      noise = 0.03,
      contrast = appearance.contrast,
      vibrancy = 0.03,
      vibrancy_darkness = 0.7,
      brightness = appearance.brightness,
      popups_ignorealpha = 0.5,
      input_methods_ignorealpha = 0.5,
    },
    shadow = {
      enabled = true,
      range = 25,
      render_power = 2,
      offset = "0 8",
      color = "rgba(00000080)",
      color_inactive = "rgba(00000033)",
    },
  },
  group = {
    groupbar = {
      font_size = 12,
      font_family = appearance.ui_font,
      font_weight_active = "ultraheavy",
      font_weight_inactive = "normal",
      indicator_height = 0,
      indicator_gap = 5,
      height = 22,
      gaps_in = 5,
      gaps_out = 0,
      gradients = true,
      gradient_rounding = 0,
      gradient_round_only_edges = false,
    },
  },
  animations = { enabled = true },
  dwindle = { preserve_split = true, force_split = 2 },
  scrolling = { column_width = 0.49 },
  master = { new_status = "master" },
  misc = {
    disable_hyprland_logo = true,
    disable_splash_rendering = true,
    disable_scale_notification = true,
    focus_on_activate = true,
    anr_missed_pings = 3,
    on_focus_under_fullscreen = 1,
  },
  cursor = { warp_on_change_workspace = 1 },
  binds = { hide_special_on_workspace_change = true },
})
o.apply_colors()

-- # Application opacity
-- # Browser and terminal native colors stay application-owned; these only affect composition.
o.window({ tag = "chromium-based-browser" }, { opacity = preserved_opacity })
o.window({ tag = "firefox-based-browser" }, { opacity = preserved_opacity })
o.window(o.classes.web_calls, { opacity = preserved_opacity })
o.window({ tag = "pip" }, { opacity = solid })
o.window("qemu", { opacity = solid })
o.window("com.libretro.RetroArch", { opacity = solid })
o.window("steam.*", { opacity = solid })
o.window(o.classes.opaque_media, { opacity = solid })
-- # Obsidian themes own background alpha; keep text and note planes fully solid.
o.window({ class = o.classes.obsidian }, { opacity = solid_override })
-- # Zed's theme owns its backgrounds (solid hex); compositor opacity would let the wallpaper tint the editor.
o.window({ class = o.classes.zed }, { opacity = solid_override })
-- # Strata's generated skin owns its background alpha; keep the window itself at 1.
o.window({ class = o.classes.strata }, { opacity = solid_override })
o.window({ tag = "default-opacity" }, { opacity = default_opacity })
o.window({ tag = "terminal" }, { tag = "-default-opacity", opacity = preserved_opacity })
-- # Rnote paints its own page colours; the default-opacity tag would let the wallpaper tint the canvas. Must come after the tag rules.
o.window({ class = "^com\\.github\\.flxzt\\.rnote$" }, { opacity = solid_override })

-- # Floating frames follow Super+T; tiled windows are rounded, fullscreen stays sharp.
o.window({ float = true }, { border_size = 1, rounding = 12 })
o.window({ float = false }, { border_size = 1, rounding = 20 })
o.window({ fullscreen_state_internal = 2 }, { border_size = 0, rounding = 0 })

-- # Layer surfaces
-- # Keep technical namespaces in sync with their actual providers.
hl.layer_rule({ match = { namespace = "selection" }, no_anim = true, animation = "none" })
hl.layer_rule({ match = { namespace = "^terminus-symbols$" }, no_anim = true, animation = "none" })
hl.layer_rule({ match = { namespace = "^terminus-launcher$" }, no_anim = true, animation = "none" })
-- # Wallpaper-only blur prevents a dark seam between the rail and attached panel.
hl.layer_rule({ match = { namespace = "^terminusbar-(bar-default|attached-panel)$" }, xray = true })
-- # mako notifications: blur behind the translucent glass fill, like windows.
hl.layer_rule({ match = { namespace = "^notifications$" }, blur = true, ignore_alpha = 0.2 })

-- # The control center / dashboard blurs what is behind it so window text never shows through; pixels below
-- # 20% alpha stay unblurred. The local plugin still supplies the menu-only blur for popups.
hl.layer_rule({
  match = { namespace = "^terminusbar-control-center$" },
  blur = true,
  blur_popups = false,
  xray = false,
  ignore_alpha = 0.2,
  dim_around = false,
})

-- # hl.plugin.load declares desired plugins, not an immediate load operation.
-- # Register on every reload: Hyprland deduplicates paths and unloads omitted ones.
local function register_menu_blur_plugin()
  local module = home .. "/Projects/Terminus-Est/modules/terminus-bar-menu-blur"
  local plugin_path = module .. "/build/terminus-bar-menu-blur.so"
  local loaded = false
  for _, plugin in ipairs(hl.get_loaded_plugins()) do
    if plugin.name == "terminus-bar-menu-blur" then
      loaded = true
      break
    end
  end
  local artifact, reason = io.open(plugin_path, "rb")
  if artifact then
    artifact:close()
    hl.plugin.load(plugin_path)
    if not loaded then
      -- # Native loading reports ABI/init failures through logs and notifications.
      print("[terminus-bar-menu-blur] Requested menu-only blur; if loading fails, rebuild against the running Hyprland headers/compiler: make -B -C " .. module)
    end
  else
    local warning = "[terminus-bar-menu-blur] Menu blur unavailable; keeping the no-blur fallback. Build with: make -B -C " .. module .. ". " .. tostring(reason)
    print(warning)
    hl.notification.create({ text = warning, timeout = 10000, icon = "warning" })
  end
end

register_menu_blur_plugin()
