-- # Terminus-Est Hyprland entrypoint
-- # ~/.config/hypr/hyprland.lua is only the application-required link to this file.
-- # Ecosystem Hyprlang files belong to hypridle, hyprsunset and the portal, not Lua.
local home = assert(os.getenv("HOME"), "HOME is required")
local root = home .. "/.config/terminus-est"

-- # Reload only our modules, without accumulating a new package.path prefix.
for name in pairs(package.loaded) do
  if name:match("^terminus_hypr%.") then
    package.loaded[name] = nil
  end
end
local module_path = root .. "/hyprland/?.lua;"
if package.path:sub(1, #module_path) ~= module_path then
  package.path = module_path .. package.path
end
-- # Machine-specific hardware lives in hosts/<hostname>/hyprland/: a file there replaces the
-- # module of the same name (monitors), and host.lua is loaded after input for extra devices
-- # and host_bindings.lua after the shared bindings.
local hostname = "unknown"
do
  local file = io.open("/etc/hostname", "r")
  if file then
    hostname = file:read("l") or hostname
    file:close()
  end
end
local function readable(path)
  local file = io.open(path, "r")
  if file then
    file:close()
  end
  return file ~= nil
end
-- # The host's file when it has one, else the shared one (also when neither exists, for the error message).
local function module_file(name)
  local hosted = root .. "/hosts/" .. hostname .. "/hyprland/" .. name .. ".lua"
  if readable(hosted) then
    return hosted
  end
  return root .. "/hyprland/" .. name .. ".lua"
end
local function load_module(name, optional)
  local path = module_file(name)
  if not readable(path) then
    if optional then
      return
    end
    error("missing Hyprland module " .. path)
  end
  local key = "terminus_hypr." .. name
  package.preload[key] = function()
    return dofile(path)
  end
  require(key)
end

-- # Environment and input precede bindings; tags precede visual rules.
load_module("helpers")
load_module("envs")
load_module("monitors")
load_module("input")
load_module("host", true)
load_module("bindings")
load_module("application_bindings")
load_module("host_bindings", true)
load_module("windows")
load_module("look_and_feel")
load_module("animations")
load_module("cursor")
load_module("toggles")
load_module("autostart")
