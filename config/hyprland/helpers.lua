-- # Vocabulary for the other modules: o.bind, o.window, o.launch, o.classes and file/command probes.

o = o or {}

local project_root = assert(os.getenv("HOME"), "HOME is required") .. "/Projects/Terminus-Est"

-- # Window class patterns that both the behaviour rules (windows.lua) and the opacity rules (look_and_feel.lua) name.
o.classes = {
  web_calls = "(chrome-youtube\\.com__-Default|chrome-app\\.zoom\\.us__wc_home-Default)",
  opaque_media = "^(zoom|vlc|mpv|org.kde.kdenlive|com.obsproject.Studio|com.github.PintaProject.Pinta|imv|org.gnome.NautilusPreviewer)$",
  obsidian = "^md\\.obsidian\\.Obsidian$",
  zed = "^dev\\.zed\\.Zed$",
  strata = "^io\\.github\\.lgse\\.Strata$",
}

-- # o.bind(keys, description, action [, options]): a string action runs as a shell command. `options` is
-- # hl.bind's table (locked, repeating, mouse, ...); it is filled in with the description, so pass a fresh one.
function o.bind(keys, description, action, options)
  local opts = options or {}
  if description then
    opts.description = description
  end

  if type(action) == "string" then
    action = hl.dsp.exec_cmd(action)
  end

  return hl.bind(keys, action, opts)
end

-- # o.window(match, rules): `match` is a class pattern or a table of match fields (title, tag, float, ...).
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

function o.file_exists(path)
  local file = io.open(path, "r")
  if not file then
    return false
  end
  file:close()
  return true
end

-- # True when `command` is a file in a directory of the PATH the desktop sessions use (the same order as envs.lua).
function o.command_exists(command)
  local path = project_root .. "/bin:"
    .. project_root .. "/modules/desktop/bin:"
    .. os.getenv("HOME") .. "/.local/bin:"
    .. (os.getenv("PATH") or "/usr/bin")

  for directory in path:gmatch("[^:]+") do
    if o.file_exists(directory .. "/" .. command) then
      return true
    end
  end

  return false
end
