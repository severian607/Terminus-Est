#include "system/health.h"

#include <charconv>
#include <chrono>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace health {

  namespace {

    using Clock = std::chrono::steady_clock;

    // On at `on`, off again at `off` (a few points on the safe side), staying put in between.
    struct Latch {
      bool active = false;
      bool update(bool above, double value, double on, double off) {
        if (above) {
          active = active ? value > off : value >= on;
        } else {
          active = active ? value < off : value <= on;
        }
        return active;
      }
    };

    struct State {
      Latch cpuError;
      Latch cpuWarm;
      bool calm = false;
      Clock::time_point calmSince{};
      Latch gpuError;
      Latch memSwap;
      Latch memPressure;
      Latch memStall;
      Latch memLow;
    };

    State& state() {
      static State s;
      return s;
    }

    std::unordered_map<std::string, std::pair<Latch, Latch>>& latches() {
      static std::unordered_map<std::string, std::pair<Latch, Latch>> map;
      return map;
    }

    struct MemoryPressure {
      double someAvg60 = 0.0;
      double fullAvg10 = 0.0;
    };

    // The number after `field` (e.g. "avg10=") in a PSI line, or 0 when absent or malformed.
    double psiField(std::string_view text, std::string_view field) {
      const auto at = text.find(field);
      if (at == std::string_view::npos) {
        return 0.0;
      }
      const std::string_view number = text.substr(at + field.size());
      double value = 0.0;
      const auto result = std::from_chars(number.data(), number.data() + number.size(), value);
      return result.ec == std::errc{} ? value : 0.0;
    }

    // One read of /proc/pressure/memory; both averages are 0 when PSI is unavailable.
    MemoryPressure readMemoryPressure() {
      MemoryPressure pressure;
      std::ifstream in("/proc/pressure/memory");
      std::string text;
      while (std::getline(in, text)) {
        if (text.rfind("some", 0) == 0) {
          pressure.someAvg60 = psiField(text, "avg60=");
        } else if (text.rfind("full", 0) == 0) {
          pressure.fullAvg10 = psiField(text, "avg10=");
        }
      }
      return pressure;
    }

  } // namespace

  Level cpuTemp(double tempC, double usagePercent) {
    State& s = state();
    const auto now = Clock::now();
    // "Calm" means under 10% load; load above 15% breaks it, so brief wobble around the line does not reset it.
    if (usagePercent < 10.0) {
      if (!s.calm) {
        s.calm = true;
        s.calmSince = now;
      }
    } else if (usagePercent > 15.0) {
      s.calm = false;
    }
    if (s.cpuError.update(true, tempC, 90.0, 87.0)) {
      return Level::Error;
    }
    const bool calmLongEnough = s.calm && now - s.calmSince >= std::chrono::minutes(3);
    // Once lit, the warning holds until the temperature drops to 57 C or the machine stops being calm.
    const bool hot = s.cpuWarm.active ? (tempC > 57.0 && s.calm) : (tempC >= 60.0 && calmLongEnough);
    s.cpuWarm.active = hot;
    return hot ? Level::Warning : Level::Ok;
  }

  Level gpuTemp(double tempC) { return state().gpuError.update(true, tempC, 90.0, 87.0) ? Level::Error : Level::Ok; }

  Level memory(double swapPercent, double availablePercent) {
    State& s = state();
    const MemoryPressure psi = readMemoryPressure();
    if (s.memStall.update(true, psi.fullAvg10, 5.0, 2.0) || s.memLow.update(false, availablePercent, 5.0, 8.0)) {
      return Level::Error;
    }
    const bool pressureOn = s.memPressure.update(true, psi.someAvg60, 10.0, 5.0);
    // Swap alone is not a signal (half full with RAM to spare is routine): it only counts with RAM running short.
    const bool swapOn = s.memSwap.active ? (swapPercent >= 45.0 && availablePercent < 20.0)
                                         : (swapPercent >= 50.0 && availablePercent < 15.0);
    s.memSwap.active = swapOn;
    return (pressureOn || swapOn) ? Level::Warning : Level::Ok;
  }

  Level disk(std::string_view key, double usedPercent) {
    auto& [warn, error] = latches()[std::string(key) + ":disk"];
    if (error.update(true, usedPercent, 90.0, 87.0)) {
      return Level::Error;
    }
    return warn.update(true, usedPercent, 80.0, 77.0) ? Level::Warning : Level::Ok;
  }

  Level battery(std::string_view key, double percent, bool charging) {
    auto& [warn, error] = latches()[std::string(key) + ":battery"];
    if (charging) {
      warn.active = false;
      error.active = false;
      return Level::Ok;
    }
    if (error.update(false, percent, 10.0, 13.0)) {
      return Level::Error;
    }
    return warn.update(false, percent, 20.0, 23.0) ? Level::Warning : Level::Ok;
  }

} // namespace health
