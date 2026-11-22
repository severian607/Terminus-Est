-- # Hyprland binding, window-rule and application helpers.

o = o or {}

function o.bind(keys, description, dispatcher, options)
  local opts = options or {}
  if description then
    opts.description = description
  end

  if type(dispatcher) == "string" then
    dispatcher = hl.dsp.exec_cmd(dispatcher)
  end

  return hl.bind(keys, dispatcher, opts)
end

function o.window(match, rules)
  rules.match = rules.match or {}

  if type(match) == "string" then
    rules.match.class = match
  else
    for key, value in pairs(match) do
      rules.match[key] = value
    end
  end

  return hl.window_rule(rules)
end

function o.launch(command)
  return "uwsm-app -- " .. command
end

function o.launch_on_start(command)
  hl.on("hyprland.start", function()
    hl.exec_cmd(o.launch(command))
  end)
end

function o.file_exists(path)
  local file = io.open(path, "r")
  if not file then
    return false
  end
  file:close()
  return true
end

function o.command_exists(command)
  local home = assert(os.getenv("HOME"), "HOME is required")
  local path = home .. "/Projects/Terminus-Est/bin:"
    .. home .. "/Projects/Terminus-Est/modules/desktop/bin:"
    .. home .. "/.local/bin:"
    .. (os.getenv("PATH") or "/usr/bin")

  for directory in path:gmatch("[^:]+") do
    if o.file_exists(directory .. "/" .. command) then
      return true
    end
  end

  return false
end
