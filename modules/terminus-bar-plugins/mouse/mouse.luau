--!nonstrict
-- Mouse: an animated running mouse bar widget. Pace reflects GPU usage, sampled
-- from amdgpu sysfs, with separately configurable walk/run thresholds. Color
-- follows the theme's secondary role by default, or a pinned custom color.
--
-- The mouse shape is a custom pictographic font (fonts/mousewalk2.otf, drawn
-- as a 4-frame gallop cycle plus a curled-up sleeper) so it can be colored
-- like any other bar text: "a" = idle/asleep, "b".."e" = the 4-frame run
-- cycle. Each glyph's advance width is trimmed to the shared ink extent
-- (non-negative left bearing, no dead trailing space) so bar layout doesn't
-- overlap the previous widget or leave a large gap before the next one.
--
-- Font registration is process-global and keyed by file path/family, so this
-- filename must change (not just its contents) whenever the glyph set changes
-- during development, or the shell keeps rendering whatever it cached the
-- first time it saw this path.

local mouseFont = terminusbar.loadFont("fonts/mousewalk2.otf")

local GLYPH_IDLE = "a"
local RUN_GLYPHS = { "b", "c", "d", "e" }

local TICK_MS = 80
local WALK_MAX_MS = 380
local WALK_MIN_MS = 180
local RUN_MAX_MS = 160
local RUN_MIN_MS = 55

local mouseSize = terminusbar.getConfig("mouse_size")
local showGpuPercent = terminusbar.getConfig("show_gpu_percent")
local walkThreshold = terminusbar.getConfig("walk_threshold")
local runThreshold = terminusbar.getConfig("run_threshold")
local pollIntervalMs = terminusbar.getConfig("poll_interval") * 1000
local colorMode = terminusbar.getConfig("mouse_color_mode")
local customColor = terminusbar.getConfig("mouse_color")

local frameIndex = 1
local frameElapsed = 0
local frameDurationMs = WALK_MAX_MS

local sampleElapsed = pollIntervalMs -- sample immediately on first tick
local gpuPercent = 0
-- AMD amdgpu exposes utilisation as an integer 0-100 in sysfs. The card index
-- differs per machine (card0/card1), so probe once and remember the first hit.
local gpuPath = nil
local function sampleGpu()
  if not gpuPath then
    for i = 0, 3 do
      local p = `/sys/class/drm/card{i}/device/gpu_busy_percent`
      if terminusbar.readFile(p) then
        gpuPath = p
        break
      end
    end
    if not gpuPath then
      return
    end
  end
  local v = tonumber((terminusbar.readFile(gpuPath) or ""):match("%d+"))
  if v then
    gpuPercent = math.clamp(v, 0, 100)
  end
end

-- Returns ("idle" | "walk" | "run", frame-duration-ms-for-that-pace).
local function paceFor(load)
  local runFloor = math.max(runThreshold, walkThreshold + 1)

  if load < walkThreshold then
    return "idle", WALK_MAX_MS
  elseif load < runFloor then
    local range = math.max(runFloor - walkThreshold, 1)
    local t = math.clamp((load - walkThreshold) / range, 0, 1)
    return "walk", WALK_MAX_MS - t * (WALK_MAX_MS - WALK_MIN_MS)
  else
    local range = math.max(100 - runFloor, 1)
    local t = math.clamp((load - runFloor) / range, 0, 1)
    return "run", RUN_MAX_MS - t * (RUN_MAX_MS - RUN_MIN_MS)
  end
end

-- Red from 90 C at any load (system/health.cpp), off again at 87 C. Checked once per poll, not per frame.
local hotLatched = false

local function updateTone()
  local stats = terminusbar.systemStats()
  local temp = stats and stats.gpu.tempC
  if temp ~= nil then
    hotLatched = hotLatched and temp > 87 or (not hotLatched and temp >= 90)
  end
end

local function resolveColor()
  if hotLatched then
    return "error"
  end
  if colorMode == "custom" and customColor and customColor ~= "" then
    return customColor
  end
  return "secondary"
end

local function render(pace)
  local glyph = pace == "idle" and GLYPH_IDLE or RUN_GLYPHS[frameIndex]
  local color = resolveColor()
  local children = {
    ui.label({ text = glyph, fontFamily = mouseFont, baseline = "inkCentered", fontSize = mouseSize, color = color }),
  }

  if showGpuPercent then
    table.insert(children, ui.label({ text = `{math.floor(gpuPercent)}%`, fontSize = 11, color = color }))
  end

  local container = barWidget.isVertical() and ui.column or ui.row
  barWidget.render(container({ gap = 4, align = "center" }, children))
  barWidget.setTooltip(`Mouse — GPU {math.floor(gpuPercent)}% ({pace})`)
end

terminusbar.setUpdateInterval(TICK_MS)

function update()
  sampleElapsed += TICK_MS
  if sampleElapsed >= pollIntervalMs then
    sampleElapsed = 0
    sampleGpu()
    updateTone()
  end

  local pace, duration = paceFor(gpuPercent)
  frameDurationMs = duration

  if pace == "idle" then
    frameElapsed = 0
    frameIndex = 1
  else
    frameElapsed += TICK_MS
    if frameElapsed >= frameDurationMs then
      frameElapsed = 0
      frameIndex = frameIndex % #RUN_GLYPHS + 1
    end
  end

  render(pace)
end

function onConfigChanged()
  mouseSize = terminusbar.getConfig("mouse_size")
  showGpuPercent = terminusbar.getConfig("show_gpu_percent")
  walkThreshold = terminusbar.getConfig("walk_threshold")
  runThreshold = terminusbar.getConfig("run_threshold")
  pollIntervalMs = terminusbar.getConfig("poll_interval") * 1000
  colorMode = terminusbar.getConfig("mouse_color_mode")
  customColor = terminusbar.getConfig("mouse_color")
  render((paceFor(gpuPercent)))
end
