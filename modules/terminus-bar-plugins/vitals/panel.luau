--!nonstrict
-- Vitals panel: the click panel of the cat (cpu), mouse (gpu) and mem bar widgets, one panel with
-- three views in the terminal look of the bar's quick panels:
--   [ view ] ─────────────── [ status ]
--   hero: the animal (cpu, gpu), the big reading, a dim detail line
--   ¹history  band graph (min–max per bucket, bar = average) from the logger, event marks underneath; h cycles 1, 6, 24 hours and 7 days
--   footer key hints
-- The open context picks the view; Tab / Shift+Tab cycle it in place, t opens the task manager.

local gpuSysfs = require("./lib/gpu_sysfs.luau")

local FONT = "JetBrainsMonoNL Nerd Font"
local TEXT = 13
local ROW = 17

local VIEWS = { "cpu", "gpu", "mem" }
local TICK_MS = 80
local STATS_MS = 1000
local FETCH_MS = 60000
local QUERY = terminusbar.pluginDir() .. "/bin/vitals-query"
local COLS = 96
local BAND_H = 72

-- Same walk/run thresholds and gait timings as the bar's cat and mouse.
local IDLE_DRAW_WATTS = 35
local WALK_THRESHOLD = 15
local RUN_THRESHOLD = 60
local WALK_MAX_MS, WALK_MIN_MS = 380, 180
local RUN_MAX_MS, RUN_MIN_MS = 160, 55

local ANIMALS = {
  cpu = { font = terminusbar.loadFont("../cat/fonts/catwalk2.otf"), frames = { "b", "c", "d", "e", "f" } },
  gpu = { font = terminusbar.loadFont("../mouse/fonts/mousewalk2.otf"), frames = { "b", "c", "d", "e" } },
}

local view = "cpu"
local stats = nil
local win = "6" -- hours of history: "1", "6", "24" or "168" (7 days, once a day has been logged)
local data = {} -- [view .. win] = parsed vitals-query output
local fetchPending = false
local meminfo = nil

local statsElapsed = STATS_MS
local fetchElapsed = FETCH_MS
local frameIndex = 1
local frameElapsed = 0
local lastGlyph = nil

-- ── static machine facts ──────────────────────────────────────────────────────────────────

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

local function pad(text, width)
  if #text >= width then
    return text
  end
  return text .. string.rep(" ", width - #text)
end

local function lpad(text, width)
  if #text >= width then
    return text
  end
  return string.rep(" ", width - #text) .. text
end

-- ── building blocks ────────────────────────────────────────────────────────────────────────

local function text(value, color, props)
  local p = props or {}
  p.text = value
  p.fontFamily = FONT
  p.fontSize = p.fontSize or TEXT
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
  return ui.row({ align = "center", gap = 6, height = ROW }, children)
end

local function keyRow(name, value)
  return ui.row({ align = "center", gap = 8, height = ROW }, {
    text(name, "on_surface", { flexGrow = 1 }),
    text(value, "on_surface_variant"),
  })
end

-- Where the data starts: a window that is not full yet is scaled to the data there is, so a fresh log fills the
-- graph instead of leaving a sliver on the right.
local function windowStart(d)
  local first = os.time()
  for _, b in ipairs(d.buckets) do
    first = math.min(first, b.t)
  end
  return math.max(os.time() - d.span, first)
end

-- cpu and gpu scale to the window's peak, rounded up to a round number, so a quiet machine still shows its shape;
-- memory keeps 0-100 because capacity is its ceiling.
local function ceilingFor(kind, d)
  if kind == "mem" or d == nil then
    return 100
  end
  local peak = 0
  for _, b in ipairs(d.buckets) do
    peak = math.max(peak, b.max)
  end
  for _, step in ipairs({ 5, 10, 25, 50 }) do
    if peak <= step then
      return step
    end
  end
  return 100
end

local GRAPH_W = 384
local CLEAR = "primary/0"

-- A band per column: faint from min to max, a bright 2 px mark at the average, so a short burst still shows.
-- Columns without data show a dim dotted baseline, the quiet part.
local function band(d, ceiling, kind)
  local cols = {}
  local start = d and windowStart(d) or os.time()
  local span = math.max(os.time() - start, 60)
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
  local filled = {}
  for c = 1, COLS do
    if cols[c] then
      for k = 0, width - 1 do
        if c + k <= COLS and (cols[c + k] == nil) then
          filled[c + k] = cols[c]
        end
      end
    end
  end
  for c, v in pairs(filled) do
    cols[c] = v
  end
  local colW = GRAPH_W / COLS
  local columns = {}
  for c = 1, COLS do
    local v = cols[c]
    local kids
    if v then
      local hi = math.clamp(v.max / ceiling, 0, 1)
      local lo = math.clamp(v.min / ceiling, 0, 1)
      local av = math.clamp(v.sum / v.n / ceiling, lo, hi)
      local top = (1 - hi) * (BAND_H - 2)
      local upper = (hi - av) * (BAND_H - 2)
      local lower = (av - lo) * (BAND_H - 2)
      if kind == "gpu" then
        -- Solid bar up to the average, a 2 px mark at the 95th percentile (stored as the bucket's max).
        local avH = av * (BAND_H - 2)
        local gap = math.max(0, top)
        kids = {
          ui.box({ height = gap, fill = CLEAR }),
          ui.box({ height = 2, fill = "on_surface_variant" }),
          ui.box({ height = math.max(0, (BAND_H - 2) - gap - avH), fill = CLEAR }),
          ui.box({ height = avH, fill = "primary" }),
        }
      else
        kids = {
          ui.box({ height = top, fill = CLEAR }),
          ui.box({ height = upper, fill = "primary/0.3" }),
          ui.box({ height = 2, fill = "primary" }),
          ui.box({ height = lower, fill = "primary/0.3" }),
        }
      end
    else
      kids = { ui.box({ height = BAND_H - 1, fill = CLEAR }), ui.box({ height = 1, fill = c % 2 == 0 and "outline" or CLEAR }) }
    end
    columns[c] = ui.column({ width = colW, height = BAND_H, gap = 0, align = "stretch" }, kids)
  end
  local axis = ui.column({ width = 28, height = BAND_H, justify = "space_between" }, {
    text(string.format("%d%%", ceiling), "on_surface_variant", { fontSize = 10 }),
    text("0", "on_surface_variant", { fontSize = 10 }),
  })
  return ui.row({ gap = 0, height = BAND_H }, { axis, ui.row({ gap = 0, height = BAND_H }, columns) })
end

-- One glyph per event, under the column it happened in: hot ▲, oom ✖, reboot ↻. Only the kinds that apply to the
-- view count (hot for cpu and gpu, oom for memory, reboot everywhere); nil when there is nothing to show.
local function relevant(kind, e)
  return e.kind == "reboot" or (e.kind == "hot" and e.detail == kind) or (e.kind == "oom" and kind == "mem")
end

local function eventsFor(kind, d)
  local list = {}
  for _, e in ipairs(d and d.events or {}) do
    if relevant(kind, e) then
      table.insert(list, e)
    end
  end
  return list
end

local function eventStrip(list, d)
  local chars = {}
  for c = 1, COLS do
    chars[c] = " "
  end
  local start = windowStart(d)
  local span = math.max(os.time() - start, 60)
  local glyph = { hot = "▲", oom = "✖", reboot = "↻" }
  for _, e in ipairs(list) do
    local c = math.clamp(math.floor((e.t - start) / span * COLS) + 1, 1, COLS)
    chars[c] = glyph[e.kind] or "•"
  end
  return ui.row({ height = 10 }, { ui.box({ width = 28, fill = CLEAR }), text(table.concat(chars), "error", { fontSize = 7 }) })
end

-- ── per-view content ───────────────────────────────────────────────────────────────────────

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
local gpuReadings = {}

local function gpuAverage()
  if #gpuReadings >= 5 then
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

local function glyphFor(kind)
  local animal = ANIMALS[kind]
  if animal == nil then
    return nil
  end
  local pace = paceFor(usageFor(kind))
  if pace == "asleep" then
    return "a"
  end
  return animal.frames[(frameIndex - 1) % #animal.frames + 1]
end

-- Latched verdict state (the plugin VM stays loaded, so it survives the panel closing).
local latch = {}

local function on(name, value, onAt, offAt, above)
  local active = latch[name] or false
  if above then
    active = active and value > offAt or (not active and value >= onAt)
  else
    active = active and value < offAt or (not active and value <= onAt)
  end
  latch[name] = active
  return active
end

local function pressure(line, field, resource)
  for l in (terminusbar.readFile("/proc/pressure/" .. (resource or "memory")) or ""):gmatch("[^\n]+") do
    if l:sub(1, #line) == line then
      return tonumber(l:match(field .. "=([%d%.]+)")) or 0
    end
  end
  return 0
end

-- "error", "warning" or nil for the view's current state.
local function verdict(kind, usage)
  if stats == nil then
    return nil
  end
  if kind == "cpu" then
    local temp = stats.cpu.tempC
    if temp == nil then
      return nil
    end
    if on("cpuError", temp, 90, 87, true) then
      return "error"
    end
    local d = data["cpu" .. win]
    local calm = d and d.calm or 0
    -- Warm at idle: 60 C once load has stayed under 10% for three minutes; holds until 57 C or real load.
    local warm = latch.cpuWarm
    warm = warm and (temp > 57 and (usage or 0) <= 15) or (not warm and temp >= 60 and calm >= 3)
    latch.cpuWarm = warm
    return warm and "warning" or nil
  elseif kind == "gpu" then
    local temp = stats.gpu.tempC
    if temp ~= nil and on("gpuError", temp, 90, 87, true) then
      return "error"
    end
    local watts = gpuSysfs.powerWatts()
    if (usage or 0) < WALK_THRESHOLD and watts ~= nil and on("gpuDraw", watts, IDLE_DRAW_WATTS, IDLE_DRAW_WATTS - 5, true) then
      return "warning"
    end
    return nil
  end
  -- Memory: pressure, or swap >= 50% with under 15% of RAM available.
  local swap = stats.swap.totalMb > 0 and stats.swap.usedMb / stats.swap.totalMb * 100 or 0
  local avail = 100 - stats.ram.usagePercent
  if on("memStall", pressure("full", "avg10"), 5, 2, true) or on("memLow", avail, 5, 8, false) then
    return "error"
  end
  local swapOn = latch.memSwap
  swapOn = swapOn and (swap >= 45 and avail < 20) or (not swapOn and swap >= 50 and avail < 15)
  latch.memSwap = swapOn
  if on("memPressure", pressure("some", "avg60"), 10, 5, true) or swapOn then
    return "warning"
  end
  return nil
end

local function hero(kind)
  local usage = usageFor(kind)
  local big, word, detail = pct(usage), "", ""
  if kind == "cpu" then
    word = (paceFor(usage))
    local parts = { cpuModel }
    if stats and stats.cpu.tempC then
      table.insert(parts, string.format("%d°C", math.floor(stats.cpu.tempC + 0.5)))
    end
    if stats and stats.cpu.freqMhz then
      table.insert(parts, string.format("%.2f GHz", stats.cpu.freqMhz / 1000))
    end
    detail = table.concat(parts, " · ")
  elseif kind == "gpu" then
    word = (paceFor(usage))
    -- Below the walk threshold the card can still be drawing a lot: pinned memory clocks (mixed refresh rates on
    -- Polaris) keep a card at tens of watts while "busy" reads 0, so "asleep" would be wrong.
    local watts = gpuSysfs.powerWatts()
    if word == "asleep" and watts ~= nil and watts >= IDLE_DRAW_WATTS then
      word = "idle, high draw"
    end
    local parts = { gpuName }
    if stats and stats.gpu.tempC then
      table.insert(parts, string.format("%d°C", math.floor(stats.gpu.tempC + 0.5)))
    end
    local clock = gpuSysfs.clockMhz()
    if clock then
      table.insert(parts, string.format("%d MHz", math.floor(clock + 0.5)))
    end
    local fan = gpuSysfs.fanRpm()
    if fan then
      table.insert(parts, `{fan} rpm`)
    end
    detail = table.concat(parts, " · ")
  else
    big = meminfo and string.format("%.1f GiB", meminfo.used / 1024 ^ 3) or "—"
    word = pct(usage)
    detail = meminfo and `{bytes(meminfo.available)} available of {bytes(meminfo.total)}` or ""
  end

  local children = {}
  local animal = ANIMALS[kind]
  if animal then
    table.insert(children, ui.column({ width = 84, align = "center" }, {
      ui.label({
        text = glyphFor(kind),
        fontFamily = animal.font,
        baseline = "inkCentered",
        fontSize = 72,
        color = "primary",
      }),
    }))
  end
  -- Tone for what is abnormal (same rules as system/health.cpp in the bar): Warning = look when you have time,
  -- Error = broken now. Each rule turns on at its threshold and off a few points below.
  local tone = verdict(kind, usage)
  local wordColor = tone == "error" and "error" or (tone == "warning" and "warning" or "on_surface_variant")
  if kind == "cpu" and tone == "warning" and word == "asleep" then
    word = "idle, running warm"
  end
  local detailColor = wordColor
  -- An explicit width: a lone flex-grown column in the row would shrink to its first line.
  table.insert(children, ui.column({ gap = 2, align = "start", width = animal and 320 or 412 }, {
    ui.row({ align = "end", gap = 10 }, {
      text(big, "primary", { fontSize = 30, fontWeight = "bold" }),
      text(word, wordColor),
    }),
    text(detail, detailColor, { maxWidth = 400 }),
  }))
  return ui.row({ align = "center", gap = 8, height = 72 }, children)
end

local function nextView(step)
  for i, name in ipairs(VIEWS) do
    if name == view then
      return VIEWS[(i - 1 + step) % #VIEWS + 1]
    end
  end
  return VIEWS[1]
end

local function statusFor(kind)
  if stats == nil then
    return ""
  end
  if kind == "cpu" then
    -- CPU pressure: the share of the last 10 s in which some task waited for a CPU. Unlike the load average it
    -- does not count sleeping kernel threads. Warning from 20% (off again below 10%).
    local psi = pressure("some", "avg10", "cpu")
    local tone = on("cpuPressure", psi, 20, 10, true) and "warning" or nil
    return string.format("cpu pressure %d%%", math.floor(psi + 0.5)), tone
  elseif kind == "gpu" then
    if stats.gpu.vramUsedBytes and stats.gpu.vramTotalBytes then
      return `vram {bytes(stats.gpu.vramUsedBytes)} / {bytes(stats.gpu.vramTotalBytes)}`
    end
    return ""
  end
  return meminfo and `{string.format("%.1f", meminfo.total / 1024 ^ 3)} GiB` or ""
end

-- ── render ─────────────────────────────────────────────────────────────────────────────────

-- 1, 6 and 24 hours always; 7 days once a day has been logged.
local function windowList(d)
  local list = { "1", "6", "24" }
  if d and d.first and os.time() - d.first > 86400 then
    table.insert(list, "168")
  end
  return list
end

local function render()
  local d = data[view .. win]
  local statusText, statusTone = statusFor(view)
  local children = {
    ui.row({ align = "center", gap = 6, height = ROW }, {
      text(`[ {view} ]`, "primary"),
      rule(),
      text(`[ {statusText} ]`, statusTone == "warning" and "warning" or "on_surface_variant"),
    }),
    hero(view),
  }

  local numerals = { "¹", "²", "³" }
  local n = 0
  local function nextTitle(name)
    n += 1
    return numerals[n] .. name
  end
  local span = win == "168" and "7 d" or (win .. " h")
  local windowText = span
  local info = span
  if d and d.minutes and d.minutes < tonumber(win) * 60 * 0.9 then
    windowText = `{d.minutes} min of {span}`
    info = windowText
  end
  if d and #d.buckets > 0 then
    local peak = 0
    for _, b in ipairs(d.buckets) do
      peak = math.max(peak, b.max)
    end
    if view == "mem" and meminfo then
      info ..= ` · peak {bytes(meminfo.total * peak / 100)}`
    else
      info ..= string.format(" · peak %d%%", math.floor(peak + 0.5))
      if d.temp then
        info ..= string.format(" · %d°C", math.floor(d.temp.max + 0.5))
      end
    end
  end
  table.insert(children, section(nextTitle("history"), info))
  table.insert(children, band(d, ceilingFor(view, d), view))
  local events = eventsFor(view, d)
  if #events > 0 then
    table.insert(children, eventStrip(events, d))
  end
  local legend = {}
  local seen = {}
  for _, e in ipairs(events) do
    if not seen[e.kind] then
      seen[e.kind] = true
      table.insert(legend, ({ hot = "▲ hot", oom = "✖ oom", reboot = "↻ reboot" })[e.kind] or e.kind)
    end
  end
  local started = d and #d.buckets > 0 and os.time() - windowStart(d) or 0
  table.insert(children, ui.row({ height = 12 }, {
    ui.box({ width = 28, fill = CLEAR }),
    text(started > 0 and (started >= 7200 and string.format("%d h ago", math.floor(started / 3600 + 0.5)) or string.format("%d min ago", math.floor(started / 60))) or "", "on_surface_variant", { fontSize = 10 }),
    ui.spacer({ flexGrow = 1 }),
    text(table.concat(legend, "  "), "error", { fontSize = 10 }),
    ui.spacer({ flexGrow = 1 }),
    text("now", "on_surface_variant", { fontSize = 10 }),
  }))

  local rows = {}
  for _, row in ipairs(d and d.top or {}) do
    table.insert(rows, keyRow(row.name, view == "cpu" and string.format("%.1f%%", row.value) or bytes(row.value)))
  end
  if #rows == 0 then
    table.insert(rows, text(d and "collecting…" or "loading…", "on_surface_variant"))
  end
  local topInfo = (view == "cpu" and "avg % of total" or (view == "gpu" and "peak vram" or "peak memory (pss)"))
    .. " · " .. windowText
  table.insert(children, section(nextTitle("top"), topInfo))
  table.insert(children, ui.column({ gap = 2 }, rows))
  table.insert(children, ui.spacer({ flexGrow = 1 }))
  local windows = {}
  for _, w in ipairs(windowList(d)) do
    table.insert(windows, text(w, w == win and "primary" or "on_surface_variant", { fontWeight = w == win and "bold" or "normal" }))
  end
  windows[#windows + 1] = text(`  Tab {nextView(1)}  t taskmanager  Esc close`, "on_surface_variant")
  table.insert(children, ui.row({ gap = 6, align = "center" }, {
    text("h hours", "on_surface_variant"),
    table.unpack(windows),
  }))

  panel.render(ui.column({ gap = 6, padding = 10, border = "outline", borderWidth = 1, radius = 0, flexGrow = 1 }, children))
end

-- ── sampling ───────────────────────────────────────────────────────────────────────────────

local function sample()
  stats = terminusbar.systemStats()
  meminfo = readMeminfo()
  local busy = gpuSysfs.busyPercent() or (stats and stats.gpu.usagePercent)
  if busy ~= nil then
    table.insert(gpuReadings, busy)
    if #gpuReadings > 60 then
      table.remove(gpuReadings, 1)
    end
  end
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
    render()
  end)
end

local function setView(kind)
  if kind ~= "cpu" and kind ~= "gpu" and kind ~= "mem" then
    kind = "cpu"
  end
  view = kind
  frameIndex, frameElapsed = 1, 0
  sample()
  fetchElapsed = 0
  fetch()
  render()
end

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
    local pace, duration = paceFor(usageFor(view))
    if pace ~= "asleep" then
      frameElapsed += TICK_MS
      if frameElapsed >= duration then
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
    setView(nextView(1))
  elseif chord == "shift+Tab" then
    setView(nextView(-1))
  elseif chord == "h" then
    local list = windowList(data[view .. win])
    local at = 1
    for i, w in ipairs(list) do
      if w == win then
        at = i
      end
    end
    win = list[at % #list + 1]
    fetchElapsed = 0
    fetch()
    render()
  elseif chord == "t" then
    terminusbar.runAsync({ "uwsm-app", "--", "ghostty", "-e", "terminus-taskmanager" })
    panel.close()
  end
end
