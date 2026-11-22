--!nonstrict
-- Vitals panel: the click panel of the cat (cpu), mouse (gpu) and mem bar widgets, one panel with
-- three views in the terminal look of the bar's quick panels:
--   [ view ] ─────────────── [ status ]
--   hero: the animal (cpu, gpu), the big reading, a dim detail line
--   ¹history  band graph (min–max per bucket, bar = average) from the logger, event marks underneath; h cycles 1, 6, 24 hours and 7 days
--   footer key hints
-- The open context picks the view; Tab / Shift+Tab cycle it in place, t opens the task manager.
-- Hot path: update() runs every TICK_MS. Files, sysfs and the history tree are read and built once per sample
-- (STATS_MS) or event; a frame only advances the animal and, when its glyph changed, rebuilds the cheap header.

local gpuSysfs = require("./lib/gpu_sysfs.luau")

-- ── constants ──────────────────────────────────────────────────────────────────────────────

local FONT = "JetBrainsMonoNL Nerd Font"
local TEXT_PX = 13
local SMALL_PX = 10
local ROW_PX = 17

local VIEWS = { "cpu", "gpu", "mem" }
local TICK_MS = 80
local STATS_MS = 1000
local FETCH_MS = 60000
local QUERY = terminusbar.pluginDir() .. "/bin/vitals-query"

-- History graph: COLS columns in GRAPH_W px beside a 28 px axis, BAND_H px tall.
local COLS = 96
local GRAPH_W = 384
local AXIS_W = 28
local BAND_H = 72
local MARK_H = 2
local STRIP_H = 10
local SCALE_ROW_H = 12
local MIN_SPAN_S = 60
local CLEAR = "primary/0"
local CEILING_STEPS = { 5, 10, 25, 50 }

local HERO_H = 72
local ANIMAL_W = 84
local ANIMAL_PX = 72
local HERO_TEXT_W_WITH_ANIMAL = 320
local HERO_TEXT_W = 412

local DAY_S = 86400
local WINDOW_HOURS = { "1", "6", "24" }
local WEEK_HOURS = "168" -- offered once a day has been logged

local EVENT_GLYPH = { hot = "▲", oom = "✖", reboot = "↻" }
local SECTION_NUMERALS = { "¹", "²" }

-- Same walk/run thresholds and gait timings as the bar's cat and mouse.
local IDLE_DRAW_WATTS = 35
local WALK_THRESHOLD = 15
local RUN_THRESHOLD = 60
local WALK_MAX_MS, WALK_MIN_MS = 380, 180
local RUN_MAX_MS, RUN_MIN_MS = 160, 55

local GPU_READINGS_MAX = 60
local GPU_READINGS_MIN = 5

local ANIMALS = {
  cpu = { font = terminusbar.loadFont("../cat/fonts/catwalk2.otf"), frames = { "b", "c", "d", "e", "f" } },
  gpu = { font = terminusbar.loadFont("../mouse/fonts/mousewalk2.otf"), frames = { "b", "c", "d", "e" } },
}
local ASLEEP_GLYPH = "a"

-- ── state ──────────────────────────────────────────────────────────────────────────────────

local view = "cpu"
local win = "6" -- hours of history: "1", "6", "24" or "168" (7 days, once a day has been logged)
local data = {} -- [view .. win] = parsed vitals-query output
local fetchPending = false
local stats = nil
local meminfo = nil
local gpuReadings = {}
local latch = {} -- latched verdicts (the plugin VM stays loaded, so they survive the panel closing)

-- Derived from the state above by refresh(); render() only reads it.
local readout = { pace = "asleep", paceMs = WALK_MAX_MS, big = "—", word = "", detail = "", tone = nil, statusText = "" }
local lowerNodes = {} -- history graph .. footer, rebuilt with the readout

local statsElapsed = STATS_MS
local fetchElapsed = FETCH_MS
local frameIndex = 1
local frameElapsed = 0
local lastGlyph = nil

-- ── static machine facts ───────────────────────────────────────────────────────────────────

local cpuModel = (function()
  local info = terminusbar.readFile("/proc/cpuinfo") or ""
  local model = info:match("model name%s*:%s*([^\n]+)") or "cpu"
  model = model:gsub("%(R%)", ""):gsub("%(TM%)", ""):gsub("%s+%d+%-Core Processor", ""):gsub("%s+Processor", "")
  model = model:gsub("^AMD ", ""):gsub("^Intel ", ""):gsub("%s+", " ")
  model = model:gsub("%s+with Radeon.*$", "")
  return model
end)()

local gpuName = "gpu"
terminusbar.runAsync("lspci -mm -d ::0300; lspci -mm -d ::0380", function(result)
  local device = (result.stdout or ""):match('^[^"]*"[^"]*" "[^"]*" "([^"]*)"')
  if device then
    local bracket = device:match("%[(.-)%]")
    gpuName = ((bracket or device):match("^(.-) /") or bracket or device)
  end
end)

-- ── formatting ─────────────────────────────────────────────────────────────────────────────

local function bytes(n)
  if n == nil then
    return "—"
  end
  if n >= 1024 ^ 3 then
    return string.format("%.1f G", n / 1024 ^ 3)
  end
  return string.format("%.0f M", n / 1024 ^ 2)
end

local function pct(v)
  return v and string.format("%d%%", math.floor(v + 0.5)) or "—"
end

local function degrees(tempC)
  return string.format("%d°C", math.floor(tempC + 0.5))
end

local function ageLabel(seconds)
  if seconds <= 0 then
    return ""
  elseif seconds >= 7200 then
    return string.format("%d h ago", math.floor(seconds / 3600 + 0.5))
  end
  return string.format("%d min ago", math.floor(seconds / 60))
end

-- ── readings ───────────────────────────────────────────────────────────────────────────────

local function readMeminfo()
  local text = terminusbar.readFile("/proc/meminfo")
  if not text then
    return nil
  end
  local kb = {}
  for key, value in text:gmatch("(%w+):%s+(%d+)") do
    kb[key] = tonumber(value) * 1024
  end
  -- htop's split: cache is page cache + reclaimable slab minus shared memory; used is the rest.
  local cache = (kb.Cached or 0) + (kb.SReclaimable or 0) - (kb.Shmem or 0)
  local used = (kb.MemTotal or 0) - (kb.MemFree or 0) - (kb.Buffers or 0) - cache
  return {
    total = kb.MemTotal or 0,
    available = kb.MemAvailable or 0,
    free = kb.MemFree or 0,
    cache = cache + (kb.Buffers or 0),
    used = used,
    swapTotal = kb.SwapTotal or 0,
    swapUsed = (kb.SwapTotal or 0) - (kb.SwapFree or 0),
  }
end

-- One value of a /proc/pressure file: `line` is "some" or "full", `field` "avg10" or "avg60".
local function pressure(line, field, resource)
  for l in (terminusbar.readFile("/proc/pressure/" .. resource) or ""):gmatch("[^\n]+") do
    if l:sub(1, #line) == line then
      return tonumber(l:match(field .. "=([%d%.]+)")) or 0
    end
  end
  return 0
end

local function paceFor(usage)
  if usage == nil or usage < WALK_THRESHOLD then
    return "asleep", WALK_MAX_MS
  elseif usage < RUN_THRESHOLD then
    local t = (usage - WALK_THRESHOLD) / (RUN_THRESHOLD - WALK_THRESHOLD)
    return "walking", WALK_MAX_MS - t * (WALK_MAX_MS - WALK_MIN_MS)
  end
  local t = math.clamp((usage - RUN_THRESHOLD) / (100 - RUN_THRESHOLD), 0, 1)
  return "sprinting", RUN_MAX_MS - t * (RUN_MAX_MS - RUN_MIN_MS)
end

-- The GPU's load comes in bursts, so one reading is close to random: the headline is the average of the last
-- minute (a rolling window of the readings taken while the panel is open, else the newest logged minute).
local function gpuAverage()
  if #gpuReadings >= GPU_READINGS_MIN then
    local sum = 0
    for _, v in ipairs(gpuReadings) do
      sum += v
    end
    return sum / #gpuReadings
  end
  local d = data["gpu" .. win]
  return d and d.latest or nil
end

local function usageFor(kind)
  if stats == nil then
    return nil
  end
  if kind == "cpu" then
    return stats.cpu.usagePercent
  elseif kind == "gpu" then
    return gpuAverage() or stats.gpu.usagePercent or gpuSysfs.busyPercent()
  end
  return stats.ram.usagePercent
end

-- Hysteresis: turns on at `onAt`, off again at `offAt` (`above`: the bad side is high).
local function latched(name, value, onAt, offAt, above)
  local active = latch[name] or false
  if above then
    active = active and value > offAt or (not active and value >= onAt)
  else
    active = active and value < offAt or (not active and value <= onAt)
  end
  latch[name] = active
  return active
end

-- ── verdicts ───────────────────────────────────────────────────────────────────────────────
-- Same rules as system/health.cpp in the bar: Warning = look when you have time, Error = broken now.
-- Each rule turns on at its threshold and off a few points below. Result: "error", "warning" or nil.

local function cpuVerdict(usage)
  local temp = stats.cpu.tempC
  if temp == nil then
    return nil
  end
  if latched("cpuError", temp, 90, 87, true) then
    return "error"
  end
  local d = data["cpu" .. win]
  local calmMinutes = d and d.calm or 0
  -- Warm at idle: 60 C once load has stayed under 10% for three minutes; holds until 57 C or real load.
  local warm = latch.cpuWarm
  warm = warm and (temp > 57 and (usage or 0) <= 15) or (not warm and temp >= 60 and calmMinutes >= 3)
  latch.cpuWarm = warm
  return warm and "warning" or nil
end

local function gpuVerdict(usage)
  local temp = stats.gpu.tempC
  if temp ~= nil and latched("gpuError", temp, 90, 87, true) then
    return "error"
  end
  local watts = gpuSysfs.powerWatts()
  if (usage or 0) < WALK_THRESHOLD and watts ~= nil and latched("gpuDraw", watts, IDLE_DRAW_WATTS, IDLE_DRAW_WATTS - 5, true) then
    return "warning"
  end
  return nil
end

-- Memory: pressure, or swap >= 50% with under 15% of RAM available.
local function memVerdict()
  local swapPercent = stats.swap.totalMb > 0 and stats.swap.usedMb / stats.swap.totalMb * 100 or 0
  local availablePercent = 100 - stats.ram.usagePercent
  if
    latched("memStall", pressure("full", "avg10", "memory"), 5, 2, true)
    or latched("memLow", availablePercent, 5, 8, false)
  then
    return "error"
  end
  local swapOn = latch.memSwap
  swapOn = swapOn and (swapPercent >= 45 and availablePercent < 20)
    or (not swapOn and swapPercent >= 50 and availablePercent < 15)
  latch.memSwap = swapOn
  if latched("memPressure", pressure("some", "avg60", "memory"), 10, 5, true) or swapOn then
    return "warning"
  end
  return nil
end

local function verdictFor(kind, usage)
  if stats == nil then
    return nil
  elseif kind == "cpu" then
    return cpuVerdict(usage)
  elseif kind == "gpu" then
    return gpuVerdict(usage)
  end
  return memVerdict()
end

-- ── hero text per view ─────────────────────────────────────────────────────────────────────
-- Each returns the big reading, the pace word and the dim detail line.

local function cpuHero(usage, pace)
  local parts = { cpuModel }
  if stats and stats.cpu.tempC then
    table.insert(parts, degrees(stats.cpu.tempC))
  end
  if stats and stats.cpu.freqMhz then
    table.insert(parts, string.format("%.2f GHz", stats.cpu.freqMhz / 1000))
  end
  return pct(usage), pace, table.concat(parts, " · ")
end

local function gpuHero(usage, pace)
  -- Below the walk threshold the card can still be drawing a lot: pinned memory clocks (mixed refresh rates on
  -- Polaris) keep a card at tens of watts while "busy" reads 0, so "asleep" would be wrong.
  local watts = gpuSysfs.powerWatts()
  if pace == "asleep" and watts ~= nil and watts >= IDLE_DRAW_WATTS then
    pace = "idle, high draw"
  end
  local parts = { gpuName }
  if stats and stats.gpu.tempC then
    table.insert(parts, degrees(stats.gpu.tempC))
  end
  local clock = gpuSysfs.clockMhz()
  if clock then
    table.insert(parts, string.format("%d MHz", math.floor(clock + 0.5)))
  end
  local fan = gpuSysfs.fanRpm()
  if fan then
    table.insert(parts, `{fan} rpm`)
  end
  return pct(usage), pace, table.concat(parts, " · ")
end

local function memHero(usage)
  local big = meminfo and string.format("%.1f GiB", meminfo.used / 1024 ^ 3) or "—"
  local detail = meminfo and `{bytes(meminfo.available)} available of {bytes(meminfo.total)}` or ""
  return big, pct(usage), detail
end

local HEROES = { cpu = cpuHero, gpu = gpuHero, mem = memHero }

-- Top-right status of the header: text and optional tone.
local function statusFor(kind)
  if stats == nil then
    return ""
  end
  if kind == "cpu" then
    -- CPU pressure: the share of the last 10 s in which some task waited for a CPU. Unlike the load average it
    -- does not count sleeping kernel threads. Warning from 20% (off again below 10%).
    local psi = pressure("some", "avg10", "cpu")
    local tone = latched("cpuPressure", psi, 20, 10, true) and "warning" or nil
    return string.format("cpu pressure %d%%", math.floor(psi + 0.5)), tone
  elseif kind == "gpu" then
    if stats.gpu.vramUsedBytes and stats.gpu.vramTotalBytes then
      return `vram {bytes(stats.gpu.vramUsedBytes)} / {bytes(stats.gpu.vramTotalBytes)}`
    end
    return ""
  end
  return meminfo and `{string.format("%.1f", meminfo.total / 1024 ^ 3)} GiB` or ""
end

-- ── history model ──────────────────────────────────────────────────────────────────────────

-- Where the data starts: a window that is not full yet is scaled to the data there is, so a fresh log fills the
-- graph instead of leaving a sliver on the right. Returns the start time and the span in seconds.
local function timelineFor(d, now)
  if d == nil then
    return now, MIN_SPAN_S
  end
  local first = now
  for _, b in ipairs(d.buckets) do
    first = math.min(first, b.t)
  end
  local start = math.max(now - d.span, first)
  return start, math.max(now - start, MIN_SPAN_S)
end

local function peakOf(d)
  local peak = 0
  for _, b in ipairs(d and d.buckets or {}) do
    peak = math.max(peak, b.max)
  end
  return peak
end

-- cpu and gpu scale to the window's peak, rounded up to a round number, so a quiet machine still shows its shape;
-- memory keeps 0-100 because capacity is its ceiling.
local function ceilingFor(kind, d, peak)
  if kind == "mem" or d == nil then
    return 100
  end
  for _, step in ipairs(CEILING_STEPS) do
    if peak <= step then
      return step
    end
  end
  return 100
end

-- Merge the buckets into COLS columns of { min, max, sum, n }.
local function aggregateColumns(d, start, span)
  local cols = {}
  for _, b in ipairs(d and d.buckets or {}) do
    local c = math.clamp(math.floor((b.t - start) / span * COLS) + 1, 1, COLS)
    local cur = cols[c]
    if cur == nil then
      cols[c] = { min = b.min, max = b.max, sum = b.avg, n = 1 }
    else
      cur.min, cur.max = math.min(cur.min, b.min), math.max(cur.max, b.max)
      cur.sum, cur.n = cur.sum + b.avg, cur.n + 1
    end
  end
  -- A bucket spans several columns while the log is young: carry it over the columns up to the next bucket.
  local width = d and math.max(1, math.floor(d.bucket / span * COLS + 0.5)) or 1
  local carried = {}
  for c = 1, COLS do
    if cols[c] then
      for k = 0, width - 1 do
        if c + k <= COLS and cols[c + k] == nil then
          carried[c + k] = cols[c]
        end
      end
    end
  end
  for c, v in pairs(carried) do
    cols[c] = v
  end
  return cols
end

-- Events that apply to the view: hot for cpu and gpu, oom for memory, reboot everywhere.
local function eventsFor(kind, d)
  local list = {}
  for _, e in ipairs(d and d.events or {}) do
    if e.kind == "reboot" or (e.kind == "hot" and e.detail == kind) or (e.kind == "oom" and kind == "mem") then
      table.insert(list, e)
    end
  end
  return list
end

-- ── building blocks ────────────────────────────────────────────────────────────────────────

local function text(value, color, props)
  local p = props or {}
  p.text = value
  p.fontFamily = FONT
  p.fontSize = p.fontSize or TEXT_PX
  p.color = color or "on_surface"
  p.maxLines = 1
  return ui.label(p)
end

local function rule()
  return ui.box({ height = 1, fill = "outline", flexGrow = 1 })
end

local function section(title, info)
  local children = { text(title, "primary"), rule() }
  if info and info ~= "" then
    table.insert(children, text(info, "on_surface_variant"))
  end
  return ui.row({ align = "center", gap = 6, height = ROW_PX }, children)
end

local function keyRow(name, value)
  return ui.row({ align = "center", gap = 8, height = ROW_PX }, {
    text(name, "on_surface", { flexGrow = 1 }),
    text(value, "on_surface_variant"),
  })
end

local function indent()
  return ui.box({ width = AXIS_W, fill = CLEAR })
end

-- ── layout: history graph ──────────────────────────────────────────────────────────────────

-- The boxes stacked in one column, top to bottom. cpu and mem: faint band from min to max with a bright mark at
-- the average, so a short burst still shows. gpu: a solid bar up to the average and a mark at the 95th percentile
-- (stored as the bucket's max). A column without data is a dim dotted baseline, the quiet part.
local function columnBoxes(v, ceiling, kind, index)
  if v == nil then
    return { ui.box({ height = BAND_H - 1, fill = CLEAR }), ui.box({ height = 1, fill = index % 2 == 0 and "outline" or CLEAR }) }
  end
  local inner = BAND_H - MARK_H
  local hi = math.clamp(v.max / ceiling, 0, 1)
  local lo = math.clamp(v.min / ceiling, 0, 1)
  local av = math.clamp(v.sum / v.n / ceiling, lo, hi)
  local top = (1 - hi) * inner
  if kind == "gpu" then
    local barH = av * inner
    local gap = math.max(0, top)
    return {
      ui.box({ height = gap, fill = CLEAR }),
      ui.box({ height = MARK_H, fill = "on_surface_variant" }),
      ui.box({ height = math.max(0, inner - gap - barH), fill = CLEAR }),
      ui.box({ height = barH, fill = "primary" }),
    }
  end
  return {
    ui.box({ height = top, fill = CLEAR }),
    ui.box({ height = (hi - av) * inner, fill = "primary/0.3" }),
    ui.box({ height = MARK_H, fill = "primary" }),
    ui.box({ height = (av - lo) * inner, fill = "primary/0.3" }),
  }
end

local function band(kind, ceiling, cols)
  local colW = GRAPH_W / COLS
  local columns = {}
  for c = 1, COLS do
    columns[c] = ui.column({ width = colW, height = BAND_H, gap = 0, align = "stretch" }, columnBoxes(cols[c], ceiling, kind, c))
  end
  local axis = ui.column({ width = AXIS_W, height = BAND_H, justify = "space_between" }, {
    text(string.format("%d%%", ceiling), "on_surface_variant", { fontSize = SMALL_PX }),
    text("0", "on_surface_variant", { fontSize = SMALL_PX }),
  })
  return ui.row({ gap = 0, height = BAND_H }, { axis, ui.row({ gap = 0, height = BAND_H }, columns) })
end

-- One glyph per event under the column it happened in.
local function eventStrip(events, start, span)
  local chars = {}
  for c = 1, COLS do
    chars[c] = " "
  end
  for _, e in ipairs(events) do
    local c = math.clamp(math.floor((e.t - start) / span * COLS) + 1, 1, COLS)
    chars[c] = EVENT_GLYPH[e.kind]
  end
  return ui.row({ height = STRIP_H }, { indent(), text(table.concat(chars), "error", { fontSize = 7 }) })
end

local function eventLegend(events)
  local legend, seen = {}, {}
  for _, e in ipairs(events) do
    if not seen[e.kind] then
      seen[e.kind] = true
      table.insert(legend, `{EVENT_GLYPH[e.kind]} {e.kind}`)
    end
  end
  return table.concat(legend, "  ")
end

-- ── layout: sections ───────────────────────────────────────────────────────────────────────

-- 1, 6 and 24 hours always; 7 days once a day has been logged.
local function windowList(d, now)
  local list = table.clone(WINDOW_HOURS)
  if d and d.first and now - d.first > DAY_S then
    table.insert(list, WEEK_HOURS)
  end
  return list
end

local function historyInfo(d, peak, spanLabel)
  local info = spanLabel
  if d and #d.buckets > 0 then
    if view == "mem" and meminfo then
      info ..= ` · peak {bytes(meminfo.total * peak / 100)}`
    else
      info ..= string.format(" · peak %d%%", math.floor(peak + 0.5))
      if d.temp then
        info ..= ` · {degrees(d.temp.max)}`
      end
    end
  end
  return info
end

local function topRows(d)
  local rows = {}
  for _, row in ipairs(d and d.top or {}) do
    table.insert(rows, keyRow(row.name, view == "cpu" and string.format("%.1f%%", row.value) or bytes(row.value)))
  end
  if #rows == 0 then
    table.insert(rows, text(d and "collecting…" or "loading…", "on_surface_variant"))
  end
  return rows
end

local function footer(d, now)
  local children = { text("h hours", "on_surface_variant") }
  for _, w in ipairs(windowList(d, now)) do
    table.insert(children, text(w, w == win and "primary" or "on_surface_variant", { fontWeight = w == win and "bold" or "normal" }))
  end
  table.insert(children, text(`  Tab {readout.nextView}  t taskmanager  Esc close`, "on_surface_variant"))
  return ui.row({ gap = 6, align = "center" }, children)
end

-- Everything under the hero: history section, graph, event marks, top section, footer.
local function buildLower()
  local d = data[view .. win]
  local now = os.time()
  local start, span = timelineFor(d, now)
  local peak = peakOf(d)
  local events = eventsFor(view, d)

  local spanLabel = win == WEEK_HOURS and "7 d" or (win .. " h")
  local coverage = spanLabel
  if d and d.minutes and d.minutes < tonumber(win) * 60 * 0.9 then
    coverage = `{d.minutes} min of {spanLabel}`
  end
  local historyText = historyInfo(d, peak, coverage)
  local topText = (view == "cpu" and "avg % of total" or (view == "gpu" and "peak vram" or "peak memory (pss)")) .. " · " .. coverage
  local age = d and #d.buckets > 0 and ageLabel(now - start) or ""

  local nodes = {
    section(SECTION_NUMERALS[1] .. "history", historyText),
    band(view, ceilingFor(view, d, peak), aggregateColumns(d, start, span)),
  }
  if #events > 0 then
    table.insert(nodes, eventStrip(events, start, span))
  end
  table.insert(nodes, ui.row({ height = SCALE_ROW_H }, {
    indent(),
    text(age, "on_surface_variant", { fontSize = SMALL_PX }),
    ui.spacer({ flexGrow = 1 }),
    text(eventLegend(events), "error", { fontSize = SMALL_PX }),
    ui.spacer({ flexGrow = 1 }),
    text("now", "on_surface_variant", { fontSize = SMALL_PX }),
  }))
  table.insert(nodes, section(SECTION_NUMERALS[2] .. "top", topText))
  table.insert(nodes, ui.column({ gap = 2 }, topRows(d)))
  table.insert(nodes, ui.spacer({ flexGrow = 1 }))
  table.insert(nodes, footer(d, now))
  return nodes
end

-- ── layout: header and hero ────────────────────────────────────────────────────────────────

local function stepView(step)
  for i, name in ipairs(VIEWS) do
    if name == view then
      return VIEWS[(i - 1 + step) % #VIEWS + 1]
    end
  end
  return VIEWS[1]
end

local function glyphFor(kind)
  local animal = ANIMALS[kind]
  if animal == nil then
    return nil
  end
  if readout.pace == "asleep" then
    return ASLEEP_GLYPH
  end
  return animal.frames[(frameIndex - 1) % #animal.frames + 1]
end

local function header()
  return ui.row({ align = "center", gap = 6, height = ROW_PX }, {
    text(`[ {view} ]`, "primary"),
    rule(),
    text(`[ {readout.statusText} ]`, readout.statusTone == "warning" and "warning" or "on_surface_variant"),
  })
end

local function hero()
  local animal = ANIMALS[view]
  local children = {}
  if animal then
    table.insert(children, ui.column({ width = ANIMAL_W, align = "center" }, {
      ui.label({
        text = glyphFor(view),
        fontFamily = animal.font,
        baseline = "inkCentered",
        fontSize = ANIMAL_PX,
        color = "primary",
      }),
    }))
  end
  local toneColor = readout.tone == "error" and "error" or (readout.tone == "warning" and "warning" or "on_surface_variant")
  -- An explicit width: a lone flex-grown column in the row would shrink to its first line.
  table.insert(children, ui.column({ gap = 2, align = "start", width = animal and HERO_TEXT_W_WITH_ANIMAL or HERO_TEXT_W }, {
    ui.row({ align = "end", gap = 10 }, {
      text(readout.big, "primary", { fontSize = 30, fontWeight = "bold" }),
      text(readout.word, toneColor),
    }),
    text(readout.detail, toneColor, { maxWidth = 400 }),
  }))
  return ui.row({ align = "center", gap = 8, height = HERO_H }, children)
end

local function render()
  local children = { header(), hero() }
  for _, node in ipairs(lowerNodes) do
    table.insert(children, node)
  end
  panel.render(ui.column({ gap = 6, padding = 10, border = "outline", borderWidth = 1, radius = 0, flexGrow = 1 }, children))
end

-- ── sampling ───────────────────────────────────────────────────────────────────────────────

-- Recompute everything render() shows from the current state. Reads sysfs and /proc/pressure, so it runs per
-- sample or event, never per frame.
local function refresh()
  local usage = usageFor(view)
  local pace, paceMs = paceFor(usage)
  local big, word, detail = HEROES[view](usage, pace)
  local tone = verdictFor(view, usage)
  if view == "cpu" and tone == "warning" and word == "asleep" then
    word = "idle, running warm"
  end
  local statusText, statusTone = statusFor(view)
  readout = {
    pace = pace,
    paceMs = paceMs,
    big = big,
    word = word,
    detail = detail,
    tone = tone,
    statusText = statusText,
    statusTone = statusTone,
    nextView = stepView(1),
  }
  lowerNodes = buildLower()
end

local function sample()
  stats = terminusbar.systemStats()
  meminfo = readMeminfo()
  local busy = gpuSysfs.busyPercent() or (stats and stats.gpu.usagePercent)
  if busy ~= nil then
    table.insert(gpuReadings, busy)
    if #gpuReadings > GPU_READINGS_MAX then
      table.remove(gpuReadings, 1)
    end
  end
  refresh()
end

-- The history comes from the logger's database (see bin/vitals-log); one query per view and window.
local function fetch()
  if fetchPending then
    return
  end
  fetchPending = true
  local k, w = view, win
  terminusbar.runAsync({ "python3", QUERY, k, w }, function(result)
    fetchPending = false
    local decoded = result.exitCode == 0 and terminusbar.json.decode(result.stdout) or nil
    if decoded then
      data[k .. w] = decoded
    end
    -- The view or window changed while this query ran, and its own fetch() was skipped as pending.
    if k ~= view or w ~= win then
      fetch()
    end
    refresh()
    render()
  end)
end

local function setView(kind)
  view = table.find(VIEWS, kind) and kind or "cpu"
  frameIndex, frameElapsed = 1, 0
  sample()
  fetchElapsed = 0
  fetch()
  render()
end

local function cycleWindow()
  local list = windowList(data[view .. win], os.time())
  local at = table.find(list, win) or 1
  win = list[at % #list + 1]
  fetchElapsed = 0
  fetch()
  refresh()
  render()
end

-- ── entry points ───────────────────────────────────────────────────────────────────────────

terminusbar.setUpdateInterval(TICK_MS)

function onOpen(context)
  setView(context ~= nil and context ~= "" and context or view)
end

function update()
  local changed = false

  statsElapsed += TICK_MS
  if statsElapsed >= STATS_MS then
    statsElapsed = 0
    sample()
    changed = true
  end

  fetchElapsed += TICK_MS
  if fetchElapsed >= FETCH_MS then
    fetchElapsed = 0
    fetch()
  end

  if ANIMALS[view] then
    if readout.pace ~= "asleep" then
      frameElapsed += TICK_MS
      if frameElapsed >= readout.paceMs then
        frameElapsed = 0
        frameIndex += 1
      end
    end
    local glyph = glyphFor(view)
    if glyph ~= lastGlyph then
      lastGlyph = glyph
      changed = true
    end
  end

  if changed then
    render()
  end
end

function onKey(chord, pressed)
  if not pressed then
    return
  end
  if chord == "Tab" then
    setView(stepView(1))
  elseif chord == "shift+Tab" then
    setView(stepView(-1))
  elseif chord == "h" then
    cycleWindow()
  elseif chord == "t" then
    terminusbar.runAsync({ "uwsm-app", "--", "ghostty", "-e", "terminus-taskmanager" })
    panel.close()
  end
end
