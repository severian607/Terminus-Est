--!nonstrict
-- Cat: an animated running cat bar widget. Pace reflects CPU usage, sampled
-- from /proc/stat, with separately configurable walk/run thresholds. Color
-- follows the theme's secondary role by default, or a pinned custom color.
--
-- The cat shape is a custom pictographic font (fonts/catwalk2.otf, traced
-- from the MIT-licensed CatWalk plasmoid by Driglu4it) so it can be colored
-- like any other bar text: "a" = idle/asleep, "b".."f" = the 5-frame run
-- cycle. Each glyph's advance width is trimmed to the shared ink extent
-- (non-negative left bearing, no dead trailing space) so bar layout doesn't
-- overlap the previous widget or leave a large gap before the next one.
--
-- Font registration is process-global and keyed by file path/family, so this
-- filename must change (not just its contents) whenever the glyph set changes
-- during development, or the shell keeps rendering whatever it cached the
-- first time it saw this path.

local catFont = terminusbar.loadFont("fonts/catwalk2.otf")

local GLYPH_IDLE = "a"
local RUN_GLYPHS = { "b", "c", "d", "e", "f" }

local TICK_MS = 80
local WALK_MAX_MS = 380
local WALK_MIN_MS = 180
local RUN_MAX_MS = 160
local RUN_MIN_MS = 55

local catSize = terminusbar.getConfig("cat_size")
local showCpuPercent = terminusbar.getConfig("show_cpu_percent")
local walkThreshold = terminusbar.getConfig("walk_threshold")
local runThreshold = terminusbar.getConfig("run_threshold")
local pollIntervalMs = terminusbar.getConfig("poll_interval") * 1000
local colorMode = terminusbar.getConfig("cat_color_mode")
local customColor = terminusbar.getConfig("cat_color")

local frameIndex = 1
local frameElapsed = 0
local frameDurationMs = WALK_MAX_MS

local sampleElapsed = pollIntervalMs -- sample immediately on first tick
local cpuPercent = 0
local prevTotal, prevIdle = nil, nil

local updateTone -- defined below, next to the colour logic

local function sampleCpu()
  local stat = terminusbar.readFile("/proc/stat")
  if not stat then
    return
  end

  local line = stat:match("^cpu%s+(.-)\n") or stat:match("^cpu%s+(.-)$")
  if not line then
    return
  end

  local fields = {}
  for n in line:gmatch("%d+") do
    table.insert(fields, tonumber(n))
  end

  -- guest and guest_nice (fields 9-10) are already counted in user and nice: leave them out of the total.
  local idle = (fields[4] or 0) + (fields[5] or 0)
  local total = 0
  for i = 1, math.min(#fields, 8) do
    total += fields[i]
  end

  if prevTotal then
    local totalDelta = total - prevTotal
    local idleDelta = idle - prevIdle
    if totalDelta > 0 then
      cpuPercent = math.clamp((totalDelta - idleDelta) / totalDelta * 100, 0, 100)
    end
  end

  prevTotal = total
  prevIdle = idle
  updateTone()
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

-- Abnormal states tint the cat (same rules as system/health.cpp): amber when the CPU is 60 C or more after three
-- calm minutes (load under 10%), red from 90 C at any load; each turns off a few degrees lower.
local calmSince = nil
local warmLatched = false
local hotLatched = false
local tone = nil

updateTone = function()
  local stats = terminusbar.systemStats()
  local temp = stats and stats.cpu.tempC
  if temp == nil then
    tone = nil
    return
  end
  if cpuPercent < 10 then
    calmSince = calmSince or os.time()
  elseif cpuPercent > 15 then
    calmSince = nil
  end
  hotLatched = hotLatched and temp > 87 or (not hotLatched and temp >= 90)
  local calm = calmSince ~= nil and os.time() - calmSince >= 180
  warmLatched = warmLatched and (temp > 57 and calmSince ~= nil) or (not warmLatched and temp >= 60 and calm)
  tone = hotLatched and "error" or (warmLatched and "warning" or nil)
end

local function resolveColor()
  if tone ~= nil then
    return tone
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
    ui.label({ text = glyph, fontFamily = catFont, baseline = "inkCentered", fontSize = catSize, color = color }),
  }

  if showCpuPercent then
    table.insert(children, ui.label({ text = `{math.floor(cpuPercent)}%`, fontSize = 11, color = color }))
  end

  local container = barWidget.isVertical() and ui.column or ui.row
  barWidget.render(container({ gap = 4, align = "center" }, children))
  barWidget.setTooltip(`Cat — CPU {math.floor(cpuPercent)}% ({pace})`)
end

terminusbar.setUpdateInterval(TICK_MS)

function update()
  sampleElapsed += TICK_MS
  if sampleElapsed >= pollIntervalMs then
    sampleElapsed = 0
    sampleCpu()
  end

  local pace, duration = paceFor(cpuPercent)
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
  catSize = terminusbar.getConfig("cat_size")
  showCpuPercent = terminusbar.getConfig("show_cpu_percent")
  walkThreshold = terminusbar.getConfig("walk_threshold")
  runThreshold = terminusbar.getConfig("run_threshold")
  pollIntervalMs = terminusbar.getConfig("poll_interval") * 1000
  colorMode = terminusbar.getConfig("cat_color_mode")
  customColor = terminusbar.getConfig("cat_color")
  render((paceFor(cpuPercent)))
end
