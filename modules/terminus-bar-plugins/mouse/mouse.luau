--!nonstrict
-- Mouse: an animated running mouse bar widget. Pace reflects GPU usage, sampled from amdgpu sysfs, with
-- separately configurable walk/run thresholds. Color follows the theme's secondary role, a pinned custom color,
-- or red when the GPU is hot.
--
-- The shape is a pictographic font (fonts/mousewalk2.otf, a 4-frame gallop plus a curled-up sleeper):
-- "a" = asleep, "b".."e" = the 4-frame run cycle. Advance widths are trimmed to the shared ink extent so the
-- bar neither overlaps neighbours nor leaves a gap.
-- Font registration is process-global and keyed by file path: rename the font file whenever its glyphs change.
-- Twin of the cat plugin (CPU); plugins install separately, so the shared gait code is duplicated on purpose.
-- Hot path: update() every TICK_MS. The poll reads sysfs and the host stats once per poll interval; a frame
-- only steps the animation and re-renders when what is drawn changed.

local mouseFont = terminusbar.loadFont("fonts/mousewalk2.otf")

local GLYPH_IDLE = "a"
local RUN_GLYPHS = { "b", "c", "d", "e" }

local TICK_MS = 80
local WALK_MAX_MS = 380
local WALK_MIN_MS = 180
local RUN_MAX_MS = 160
local RUN_MIN_MS = 55
local PERCENT_LABEL_PX = 11
local GAP_PX = 4

-- Red from 90 C at any load (system/health.cpp), off again at 87 C.
local HOT_ON_C, HOT_OFF_C = 90, 87

-- amdgpu exposes utilisation as an integer 0-100. The card index differs per machine, so probe once.
local GPU_CARD_COUNT = 4
local GPU_BUSY_PATH = "/sys/class/drm/card%d/device/gpu_busy_percent"

local config = {}
local pollIntervalMs = 0

local function loadConfig()
  config.mouseSize = terminusbar.getConfig("mouse_size")
  config.showGpuPercent = terminusbar.getConfig("show_gpu_percent")
  config.walkThreshold = terminusbar.getConfig("walk_threshold")
  config.runThreshold = terminusbar.getConfig("run_threshold")
  config.colorMode = terminusbar.getConfig("mouse_color_mode")
  config.customColor = terminusbar.getConfig("mouse_color")
  pollIntervalMs = terminusbar.getConfig("poll_interval") * 1000
end

loadConfig()

local frameIndex = 1
local frameElapsedMs = 0
local sampleElapsedMs = pollIntervalMs -- sample immediately on first tick
local gpuPercent = 0
local gpuBusyPath = nil

local pace = "idle" -- "idle" | "walk" | "run", derived from gpuPercent and the thresholds
local frameDurationMs = WALK_MAX_MS
local hotLatched = false
local drawnKey = nil -- what the last render showed; a frame that would draw the same is skipped

-- ── pace ───────────────────────────────────────────────────────────────────────────────────

-- Walk speeds up linearly from the walk threshold, running from the run threshold.
local function updatePace()
  local load = gpuPercent
  local walkFloor = config.walkThreshold
  local runFloor = math.max(config.runThreshold, walkFloor + 1)

  if load < walkFloor then
    pace, frameDurationMs = "idle", WALK_MAX_MS
  elseif load < runFloor then
    local t = math.clamp((load - walkFloor) / math.max(runFloor - walkFloor, 1), 0, 1)
    pace, frameDurationMs = "walk", WALK_MAX_MS - t * (WALK_MAX_MS - WALK_MIN_MS)
  else
    local t = math.clamp((load - runFloor) / math.max(100 - runFloor, 1), 0, 1)
    pace, frameDurationMs = "run", RUN_MAX_MS - t * (RUN_MAX_MS - RUN_MIN_MS)
  end
end

-- ── sampling ───────────────────────────────────────────────────────────────────────────────

local function findGpuBusyPath()
  for card = 0, GPU_CARD_COUNT - 1 do
    local path = string.format(GPU_BUSY_PATH, card)
    if terminusbar.readFile(path) then
      return path
    end
  end
  return nil
end

local function sampleGpu()
  gpuBusyPath = gpuBusyPath or findGpuBusyPath()
  if not gpuBusyPath then
    return
  end
  local busy = tonumber((terminusbar.readFile(gpuBusyPath) or ""):match("%d+"))
  if busy then
    gpuPercent = math.clamp(busy, 0, 100)
  end
end

local function updateTone()
  local stats = terminusbar.systemStats()
  local temp = stats and stats.gpu.tempC
  if temp ~= nil then
    hotLatched = hotLatched and temp > HOT_OFF_C or (not hotLatched and temp >= HOT_ON_C)
  end
end

-- ── render ─────────────────────────────────────────────────────────────────────────────────

local function resolveColor()
  if hotLatched then
    return "error"
  end
  if config.colorMode == "custom" and config.customColor and config.customColor ~= "" then
    return config.customColor
  end
  return "secondary"
end

local function render(force)
  local glyph = pace == "idle" and GLYPH_IDLE or RUN_GLYPHS[frameIndex]
  local percent = math.floor(gpuPercent)
  local vertical = barWidget.isVertical()
  local color = resolveColor()
  local key = `{glyph}|{percent}|{color}|{vertical}|{pace}`
  if key == drawnKey and not force then
    return
  end
  drawnKey = key

  local children = {
    ui.label({ text = glyph, fontFamily = mouseFont, baseline = "inkCentered", fontSize = config.mouseSize, color = color }),
  }
  if config.showGpuPercent then
    table.insert(children, ui.label({ text = `{percent}%`, fontSize = PERCENT_LABEL_PX, color = color }))
  end
  local container = vertical and ui.column or ui.row
  barWidget.render(container({ gap = GAP_PX, align = "center" }, children))
  barWidget.setTooltip(`Mouse — GPU {percent}% ({pace})`)
end

-- ── entry points ───────────────────────────────────────────────────────────────────────────

updatePace()
terminusbar.setUpdateInterval(TICK_MS)

function update()
  sampleElapsedMs += TICK_MS
  if sampleElapsedMs >= pollIntervalMs then
    sampleElapsedMs = 0
    sampleGpu()
    updateTone()
    updatePace()
  end

  if pace == "idle" then
    frameElapsedMs = 0
    frameIndex = 1
  else
    frameElapsedMs += TICK_MS
    if frameElapsedMs >= frameDurationMs then
      frameElapsedMs = 0
      frameIndex = frameIndex % #RUN_GLYPHS + 1
    end
  end

  render(false)
end

function onConfigChanged()
  loadConfig()
  updatePace()
  render(true)
end
