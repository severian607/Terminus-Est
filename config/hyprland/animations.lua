-- # Window, layer and workspace motion
-- # One system: entries ease out, exits ease in and run about a third faster. Speed is in units of 100 ms.
-- # Terminus-Bar (Easing::Enter/Exit) and the launcher (curveEnter/curveExit) use the same two curves.
hl.curve("out", { type = "bezier", points = { { 0.16, 1 }, { 0.3, 1 } } })
hl.curve("in", { type = "bezier", points = { { 0.7, 0 }, { 0.84, 0 } } })

hl.animation({ leaf = "windows", enabled = true, speed = 3, bezier = "out", style = "slide" })
hl.animation({ leaf = "fade", enabled = true, speed = 2, bezier = "out" })
hl.animation({ leaf = "layersIn", enabled = true, speed = 2, bezier = "out", style = "fade" })
hl.animation({ leaf = "layersOut", enabled = true, speed = 1.5, bezier = "in", style = "fade" })
hl.animation({ leaf = "workspaces", enabled = true, speed = 3, bezier = "out", style = "slide" })
hl.animation({ leaf = "border", enabled = true, speed = 1, bezier = "linear" })
hl.animation({ leaf = "borderangle", enabled = true, speed = 30, bezier = "linear", style = "once" })

-- # Motion entries picked with Super+A (terminus-motionfx): one window preset, plus optional window and pointer
-- # effects. Each entry is a Lua file under config/motion that runs on top of the base above. The engine writes
-- # the active list; TERMINUS_MOTION_ACTIVE points the preview sandbox at its own list.
local home = assert(os.getenv("HOME"), "HOME is required")
local active = os.getenv("TERMINUS_MOTION_ACTIVE") or (home .. "/.local/state/terminus-est/toggles/hypr/motion.lua")
local entries = { home .. "/.config/terminus-est/motion/windows/pop.lua" }
if o.file_exists(active) then
  entries = dofile(active)
end
for _, entry in ipairs(entries) do
  local ok, problem = pcall(dofile, entry)
  if not ok then
    local warning = "[terminus-motion] " .. entry .. " failed: " .. tostring(problem)
    print(warning)
    hl.notification.create({ text = warning, timeout = 10000, icon = "warning" })
  end
end
