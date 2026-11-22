#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

// Verdicts for "is this abnormal", shared by the bar items and the panels so they always agree.
//   Warning  look at this when you have time (battery low, warm at idle, root filling, memory pressure)
//   Error    something is broken now (critical temperature, failed unit, stalled memory, battery nearly empty)
// Every verdict has hysteresis: it turns on at the threshold and off a few points below, so a value sitting on the
// line does not blink. State is kept per key and the functions are meant to be polled from the main thread.
namespace health {

  enum class Level : std::uint8_t { Ok, Warning, Error };

  // CPU: Error from 90 C at any load; Warning from 60 C once load has stayed under 10% for three minutes.
  [[nodiscard]] Level cpuTemp(double tempC, double usagePercent);
  // GPU: Error from 90 C.
  [[nodiscard]] Level gpuTemp(double tempC);
  // Memory: Warning on pressure (/proc/pressure/memory some avg60 >= 10) or swap >= 50% with under 15% of RAM
  // available; Error when processes stall (full avg10 >= 5) or under 5% of RAM is available.
  [[nodiscard]] Level memory(double swapPercent, double availablePercent);
  // A filesystem: Warning above 80% used, Error above 90%. `key` keeps one state per mount.
  [[nodiscard]] Level disk(std::string_view key, double usedPercent);
  // A battery: Warning at 20% or below, Error at 10% or below; never while charging. `key` is the device.
  [[nodiscard]] Level battery(std::string_view key, double percent, bool charging);

} // namespace health
