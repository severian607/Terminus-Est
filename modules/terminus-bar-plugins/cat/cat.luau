--!nonstrict
-- Cat: an animated running cat bar widget. Pace reflects CPU usage, sampled from /proc/stat, with separately
-- configurable walk/run thresholds. Color follows the theme's secondary role, a pinned custom color, or a
-- health tone (amber warm at idle, red hot).
--
-- The shape is a pictographic font (fonts/catwalk2.otf, traced from the MIT-licensed CatWalk plasmoid by Driglu4it):
-- "a" = asleep, "b".."f" = the 5-frame run cycle. Advance widths are trimmed to the shared ink extent so the
-- bar neither overlaps neighbours nor leaves a gap.
-- Font registration is process-global and keyed by file path: rename the font file whenever its glyphs change.
-- Twin of the mouse plugin (GPU); plugins install separately, so the shared gait code is duplicated on purpose.
-- Hot path: update() every TICK_MS. The poll reads /proc/stat and the host stats once per poll interval; a frame
-- only steps the animation and re-renders when what is drawn changed.

local catFont = terminusbar.loadFont("fonts/catwalk2.otf")

local GLYPH_IDLE = "a"
local RUN_GLYPHS = { "b", "c", "d", "e", "f" }

local TICK_MS = 80
local WALK_MAX_MS = 380
local WALK_MIN_MS = 180
local RUN_MAX_MS = 160
local RUN_MIN_MS = 55
local PERCENT_LABEL_PX = 11
local GAP_PX = 4

-- Health tone, same rules as system/health.cpp: amber when the CPU is warm after a calm spell, red when hot.
-- Each rule turns on at its threshold and off a few degrees lower.
local CALM_LOAD_PERCENT = 10
local BUSY_LOAD_PERCENT = 15
local CALM_SECONDS = 180
local WARM_ON_C, WARM_OFF_C = 60, 57
local HOT_ON_C, HOT_OFF_C = 90, 87

-- /proc/stat "cpu" line: user nice system idle iowait irq softirq steal; guest and guest_nice (fields 9-10) are
-- already counted in user and nice, so they stay out of the total.
local STAT_IDLE_FIELD, STAT_IOWAIT_FIELD = 4, 5
local STAT_TOTAL_FIELDS = 8

local config = {}
local pollIntervalMs = 0

local function loadConfig()
  config.catSize = terminusbar.getConfig("cat_size")
  config.showCpuPercent = terminusbar.getConfig("show_cpu_percent")
  config.walkThreshold = terminusbar.getConfig("walk_threshold")
  config.runThreshold = terminusbar.getConfig("run_threshold")
  config.colorMode = terminusbar.getConfig("cat_color_mode")
  config.customColor = terminusbar.getConfig("cat_color")
  pollIntervalMs = terminusbar.getConfig("poll_interval") * 1000
end

loadConfig()

local frameIndex = 1
local frameElapsedMs = 0
local sampleElapsedMs = pollIntervalMs -- sample immediately on first tick
local cpuPercent = 0
local prevTotal, prevIdle = nil, nil

local pace = "idle" -- "idle" | "walk" | "run", derived from cpuPercent and the thresholds
local frameDurationMs = WALK_MAX_MS
local tone = nil -- "error", "warning" or nil
local calmSince = nil
local warmLatched = false
local hotLatched = false
local drawnKey = nil -- what the last render showed; a frame that would draw the same is skipped

-- ── pace ───────────────────────────────────────────────────────────────────────────────────

-- Walk speeds up linearly from the walk threshold, running from the run threshold.
local function updatePace()
  local load = cpuPercent
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

-- Returns the busy and idle jiffy totals of the aggregate "cpu" line, or nil when it cannot be read.
local function readCpuTimes()
  local stat = terminusbar.readFile("/proc/stat")
  if not stat then
    return nil
  end
  local line = stat:match("^cpu%s+(.-)\n") or stat:match("^cpu%s+(.-)$")
  if not line then
    return nil
  end

  local fields = {}
  for n in line:gmatch("%d+") do
    table.insert(fields, tonumber(n))
  end
  local idle = (fields[STAT_IDLE_FIELD] or 0) + (fields[STAT_IOWAIT_FIELD] or 0)
  local total = 0
  for i = 1, math.min(#fields, STAT_TOTAL_FIELDS) do
    total += fields[i]
  end
  return total, idle
end

local function updateTone()
  local stats = terminusbar.systemStats()
  local temp = stats and stats.cpu.tempC
  if temp == nil then
    tone = nil
    return
  end
  if cpuPercent < CALM_LOAD_PERCENT then
    calmSince = calmSince or os.time()
  elseif cpuPercent > BUSY_LOAD_PERCENT then
    calmSince = nil
  end
  local calm = calmSince ~= nil and os.time() - calmSince >= CALM_SECONDS
  hotLatched = hotLatched and temp > HOT_OFF_C or (not hotLatched and temp >= HOT_ON_C)
  warmLatched = warmLatched and (temp > WARM_OFF_C and calmSince ~= nil) or (not warmLatched and temp >= WARM_ON_C and calm)
  tone = hotLatched and "error" or (warmLatched and "warning" or nil)
end

local function sampleCpu()
  local total, idle = readCpuTimes()
  if not total then
    return
  end
  local totalDelta = prevTotal and total - prevTotal or 0
  if totalDelta > 0 then
    cpuPercent = math.clamp((totalDelta - (idle - prevIdle)) / totalDelta * 100, 0, 100)
  end
  prevTotal, prevIdle = total, idle
  updateTone()
  updatePace()
end

-- ── render ─────────────────────────────────────────────────────────────────────────────────

local function resolveColor()
  if tone ~= nil then
    return tone
  end
  if config.colorMode == "custom" and config.customColor and config.customColor ~= "" then
    return config.customColor
  end
  return "secondary"
end

local function render(force)
  local glyph = pace == "idle" and GLYPH_IDLE or RUN_GLYPHS[frameIndex]
  local percent = math.floor(cpuPercent)
  local vertical = barWidget.isVertical()
  local color = resolveColor()
  local key = `{glyph}|{percent}|{color}|{vertical}|{pace}`
  if key == drawnKey and not force then
    return
  end
  drawnKey = key

  local children = {
    ui.label({ text = glyph, fontFamily = catFont, baseline = "inkCentered", fontSize = config.catSize, color = color }),
  }
  if config.showCpuPercent then
    table.insert(children, ui.label({ text = `{percent}%`, fontSize = PERCENT_LABEL_PX, color = color }))
  end
  local container = vertical and ui.column or ui.row
  barWidget.render(container({ gap = GAP_PX, align = "center" }, children))
  barWidget.setTooltip(`Cat — CPU {percent}% ({pace})`)
end

-- ── entry points ───────────────────────────────────────────────────────────────────────────

updatePace()
terminusbar.setUpdateInterval(TICK_MS)

function update()
  sampleElapsedMs += TICK_MS
  if sampleElapsedMs >= pollIntervalMs then
    sampleElapsedMs = 0
    sampleCpu()
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
