--!nonstrict
-- Direct DRM sysfs reads for what the host's GPU probe does not report: the AMD busy percent
-- fallback, fan speed and shader clock. The card index differs per machine, so the first card
-- with a device directory that has the file wins; a machine without it gets nil.

local M = {}

local busyPath = nil
local hwmonDir = nil
local probed = false

local function probe()
  if probed then
    return
  end
  probed = true
  for i = 0, 3 do
    local device = `/sys/class/drm/card{i}/device`
    if busyPath == nil and terminusbar.readFile(`{device}/gpu_busy_percent`) then
      busyPath = `{device}/gpu_busy_percent`
    end
    if hwmonDir == nil then
      for h = 0, 15 do
        local dir = `{device}/hwmon/hwmon{h}`
        if terminusbar.readFile(`{dir}/name`) then
          hwmonDir = dir
          break
        end
      end
    end
  end
end

local function readNumber(path)
  if path == nil then
    return nil
  end
  return tonumber((terminusbar.readFile(path) or ""):match("%d+"))
end

function M.busyPercent()
  probe()
  local v = readNumber(busyPath)
  return v and math.clamp(v, 0, 100) or nil
end

function M.fanRpm()
  probe()
  return hwmonDir and readNumber(`{hwmonDir}/fan1_input`) or nil
end

function M.clockMhz()
  probe()
  local hz = hwmonDir and readNumber(`{hwmonDir}/freq1_input`) or nil
  return hz and hz / 1e6 or nil
end

function M.powerWatts()
  probe()
  local uw = hwmonDir and (readNumber(`{hwmonDir}/power1_average`) or readNumber(`{hwmonDir}/power1_input`)) or nil
  return uw and uw / 1e6 or nil
end

return M
