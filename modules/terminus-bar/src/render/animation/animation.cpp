#include "render/animation/animation.h"

#include <algorithm>

namespace {

  // CSS cubic-bezier(x1, y1, x2, y2): find the curve parameter whose x is t, return its y.
  // x is monotonic for control points in [0, 1], so bisection always converges.
  float cubicBezier(float x1, float y1, float x2, float y2, float t) {
    const auto axis = [](float p1, float p2, float s) {
      const float inv = 1.0F - s;
      return 3.0F * inv * inv * s * p1 + 3.0F * inv * s * s * p2 + s * s * s;
    };
    float lo = 0.0F;
    float hi = 1.0F;
    for (int i = 0; i < 16; ++i) {
      const float mid = 0.5F * (lo + hi);
      if (axis(x1, x2, mid) < t) {
        lo = mid;
      } else {
        hi = mid;
      }
    }
    return axis(y1, y2, 0.5F * (lo + hi));
  }

} // namespace

float applyEasing(Easing easing, float t) {
  t = std::clamp(t, 0.0F, 1.0F);

  switch (easing) {
  case Easing::Linear:
    return t;

  case Easing::Enter:
    return cubicBezier(0.16F, 1.0F, 0.3F, 1.0F, t);

  case Easing::Exit:
    return cubicBezier(0.7F, 0.0F, 0.84F, 0.0F, t);

  case Easing::EaseInOutQuad:
    if (t < 0.5F) {
      return 2.0F * t * t;
    }
    return -1.0F + (4.0F - 2.0F * t) * t;

  case Easing::EaseInOutCubic:
    if (t < 0.5F) {
      return 4.0F * t * t * t;
    } else {
      const float f = 2.0F * t - 2.0F;
      return 0.5F * f * f * f + 1.0F;
    }
  }

  return t;
}
