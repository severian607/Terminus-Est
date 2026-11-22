#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

// Enter and Exit are the desktop-wide motion curves, shared with Hyprland (config/hyprland/animations.lua)
// and the launcher: things arrive on Enter and leave on Exit. The in-out curves are for on-screen moves.
enum class Easing : std::uint8_t {
  Linear,
  Enter,          // cubic-bezier(0.16, 1, 0.3, 1)
  Exit,           // cubic-bezier(0.7, 0, 0.84, 0)
  EaseInOutQuad,
  EaseInOutCubic,
};

float applyEasing(Easing easing, float t);

struct Animation {
  float startValue = 0.0F;
  float endValue = 0.0F;
  float durationMs = 0.0F;
  float elapsedMs = 0.0F;
  std::chrono::steady_clock::time_point startedAt;
  Easing easing = Easing::Enter;
  std::function<void(float)> setter;
  std::function<void()> onComplete;
  bool finished = false;
};
