#include "ui/palette.h"

#include "render/core/color.h"
#include "theme/builtin_palettes.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <string>

Palette palette = terminusbar::theme::findBuiltinPalette("TerminusBar")->dark.palette;
bool g_resolvedThemeLight = false;

namespace {

  // Serializes setPalette() writes against colorForRoleSnapshot() reads from the
  // script runtime thread. All other palette reads (colorForRole, rendering) stay
  // on the main thread, so this holds only while setPalette() is main-thread-only.
  std::mutex g_paletteMutex;

  std::string normalizedRoleToken(std::string_view token) {
    std::string normalized = StringUtils::trim(token);
    StringUtils::toLowerInPlace(normalized);
    return normalized;
  }

  struct Hsl {
    float h = 0.0F; // degrees
    float s = 0.0F;
    float l = 0.0F;
  };

  Hsl toHsl(const Color& c) {
    const float mx = std::max({c.r, c.g, c.b});
    const float mn = std::min({c.r, c.g, c.b});
    const float l = (mx + mn) * 0.5F;
    const float d = mx - mn;
    if (d < 1e-6F) {
      return {0.0F, 0.0F, l};
    }
    const float s = l > 0.5F ? d / (2.0F - mx - mn) : d / (mx + mn);
    float h = 0.0F;
    if (mx == c.r) {
      h = (c.g - c.b) / d + (c.g < c.b ? 6.0F : 0.0F);
    } else if (mx == c.g) {
      h = (c.b - c.r) / d + 2.0F;
    } else {
      h = (c.r - c.g) / d + 4.0F;
    }
    return {h * 60.0F, s, l};
  }

  Color fromHsl(const Hsl& v) {
    const float c = (1.0F - std::abs(2.0F * v.l - 1.0F)) * v.s;
    const float hp = std::fmod(v.h, 360.0F) / 60.0F;
    const float x = c * (1.0F - std::abs(std::fmod(hp, 2.0F) - 1.0F));
    float r = 0.0F;
    float g = 0.0F;
    float b = 0.0F;
    if (hp < 1.0F) {
      r = c;
      g = x;
    } else if (hp < 2.0F) {
      r = x;
      g = c;
    } else if (hp < 3.0F) {
      g = c;
      b = x;
    } else if (hp < 4.0F) {
      g = x;
      b = c;
    } else if (hp < 5.0F) {
      r = x;
      b = c;
    } else {
      r = c;
      b = x;
    }
    const float m = v.l - c * 0.5F;
    return rgba(r + m, g + m, b + m);
  }

  float contrastRatio(const Color& a, const Color& b) {
    const float la = relativeLuminance(a);
    const float lb = relativeLuminance(b);
    return (std::max(la, lb) + 0.05F) / (std::min(la, lb) + 0.05F);
  }

  // CIE76 distance in L*a*b* between two sRGB colours: how different they look, which a contrast ratio between two
  // light colours cannot say.
  float labDistance(const Color& a, const Color& b) {
    const auto lab = [](const Color& c) {
      const auto linear = [](float v) { return v <= 0.04045F ? v / 12.92F : std::pow((v + 0.055F) / 1.055F, 2.4F); };
      const float r = linear(c.r);
      const float g = linear(c.g);
      const float bl = linear(c.b);
      const float x = (0.4124564F * r + 0.3575761F * g + 0.1804375F * bl) / 0.95047F;
      const float y = 0.2126729F * r + 0.7151522F * g + 0.0721750F * bl;
      const float z = (0.0193339F * r + 0.1191920F * g + 0.9503041F * bl) / 1.08883F;
      const auto f = [](float t) { return t > 0.008856F ? std::cbrt(t) : 7.787F * t + 16.0F / 116.0F; };
      return std::array<float, 3>{116.0F * f(y) - 16.0F, 500.0F * (f(x) - f(y)), 200.0F * (f(y) - f(z))};
    };
    const auto la = lab(a);
    const auto lb = lab(b);
    return std::sqrt(
        (la[0] - lb[0]) * (la[0] - lb[0]) + (la[1] - lb[1]) * (la[1] - lb[1]) + (la[2] - lb[2]) * (la[2] - lb[2])
    );
  }

  // Amber, strongly saturated, with the accent's lightness where that works. Two things must hold: text contrast of
  // 4.5:1 against the surface, and a L*a*b* distance of at least 55 from the body text, so a warning never reads as
  // body text. Lightness is walked outward from the accent's own until both hold; if no lightness does, the one
  // farthest from the body text that still reads on the surface wins.
  Color deriveWarning(const Palette& p) {
    constexpr float kHue = 40.0F;
    constexpr float kSaturation = 0.95F;
    constexpr float kMinSurfaceContrast = 4.5F;
    constexpr float kMinBodyDistance = 55.0F;
    const float start = std::clamp(toHsl(p.primary).l, 0.25F, 0.8F);
    Color fallback = fromHsl({kHue, kSaturation, start});
    float fallbackDistance = -1.0F;
    for (int i = 0; i <= 60; ++i) {
      for (const float sign : {1.0F, -1.0F}) {
        const float l = start + sign * 0.01F * static_cast<float>(i);
        if (l < 0.2F || l > 0.85F) {
          continue;
        }
        const Color candidate = fromHsl({kHue, kSaturation, l});
        if (contrastRatio(candidate, p.surface) < kMinSurfaceContrast) {
          continue;
        }
        const float distance = labDistance(candidate, p.onSurface);
        if (distance >= kMinBodyDistance) {
          return candidate;
        }
        if (distance > fallbackDistance) {
          fallbackDistance = distance;
          fallback = candidate;
        }
      }
    }
    return fallback;
  }

  // Some palettes (the wallpaper-derived ones) land their error colour next to the accent: M3's pink error beside a
  // salmon primary reads as the same colour, which makes "broken" indistinguishable from "selected". When the two
  // are close in hue and lightness, error becomes a saturated red instead.
  Color distinctError(const Palette& p) {
    const Hsl accent = toHsl(p.primary);
    const Hsl err = toHsl(p.error);
    float dh = std::abs(accent.h - err.h);
    dh = std::min(dh, 360.0F - dh);
    if (dh >= 25.0F || std::abs(accent.l - err.l) >= 0.12F) {
      return p.error;
    }
    const bool lightSurface = relativeLuminance(p.surface) > 0.179F;
    Hsl w{356.0F, 0.88F, lightSurface ? 0.42F : 0.60F};
    const float step = lightSurface ? -0.02F : 0.02F;
    Color out = fromHsl(w);
    for (int i = 0; i < 40 && contrastRatio(out, p.surface) < 4.5F; ++i) {
      w.l = std::clamp(w.l + step, 0.05F, 0.95F);
      out = fromHsl(w);
    }
    return out;
  }

  // `derive(palette)`, recomputed only when the palette changes. Per thread: the script runtime reads colours through
  // colorForRoleSnapshot() on its own thread.
  template <Color (*derive)(const Palette&)> const Color& derivedColor() {
    thread_local Palette cachedFor{};
    thread_local Color cached{};
    thread_local bool valid = false;
    if (!valid || !(cachedFor == palette)) {
      cachedFor = palette;
      cached = derive(palette);
      valid = true;
    }
    return cached;
  }

} // namespace

const Color& colorForRole(ColorRole role) noexcept {
  switch (role) {
  case ColorRole::Primary:
    return palette.primary;
  case ColorRole::OnPrimary:
    return palette.onPrimary;
  case ColorRole::Secondary:
    return palette.secondary;
  case ColorRole::OnSecondary:
    return palette.onSecondary;
  case ColorRole::Tertiary:
    return palette.tertiary;
  case ColorRole::OnTertiary:
    return palette.onTertiary;
  case ColorRole::Error:
    return derivedColor<distinctError>();
  case ColorRole::OnError:
    return palette.onError;
  case ColorRole::Warning:
    return derivedColor<deriveWarning>();
  case ColorRole::Surface:
    return palette.surface;
  case ColorRole::OnSurface:
    return palette.onSurface;
  case ColorRole::SurfaceVariant:
    return palette.surfaceVariant;
  case ColorRole::OnSurfaceVariant:
    return palette.onSurfaceVariant;
  case ColorRole::Outline:
    return palette.outline;
  case ColorRole::Shadow:
    return palette.shadow;
  case ColorRole::Hover:
    return palette.hover;
  case ColorRole::OnHover:
    return palette.onHover;
  }

  return palette.onSurface;
}

Color colorForRole(ColorRole role, float alpha) noexcept {
  Color color = colorForRole(role);
  color.a *= alpha;
  return color;
}

std::optional<ColorRole> colorRoleFromToken(std::string_view token) {
  const std::string normalized = normalizedRoleToken(token);
  for (const auto& option : kColorRoleTokens) {
    if (normalized == option.token) {
      return option.role;
    }
  }
  return std::nullopt;
}

std::string_view colorRoleToken(ColorRole role) noexcept {
  for (const auto& option : kColorRoleTokens) {
    if (option.role == role) {
      return option.token;
    }
  }
  return "on_surface";
}

ColorSpec colorSpecFromRole(ColorRole role, float alpha) noexcept {
  return ColorSpec{.role = role, .fixed = clearColor(), .alpha = alpha};
}

ColorSpec fixedColorSpec(const Color& color) noexcept {
  return ColorSpec{.role = std::nullopt, .fixed = color, .alpha = 1.0F};
}

Color resolveColorSpec(const ColorSpec& color) noexcept {
  Color resolved = color.role.has_value() ? colorForRole(*color.role) : color.fixed;
  resolved.a *= color.alpha;
  return resolved;
}

bool isLightPalette() noexcept { return relativeLuminance(palette.surface) > 0.179F; }

bool isResolvedLightTheme() noexcept { return g_resolvedThemeLight; }

void setResolvedThemeLight(bool light) noexcept { g_resolvedThemeLight = light; }

Signal<>& paletteChanged() {
  static Signal<> signal;
  return signal;
}

void setPalette(const Palette& p) {
  {
    std::scoped_lock lock(g_paletteMutex);
    if (palette == p) {
      return;
    }
    palette = p;
  }
  paletteChanged().emit();
}

Color colorForRoleSnapshot(ColorRole role) {
  std::scoped_lock lock(g_paletteMutex);
  return colorForRole(role);
}

Palette lerpPalette(const Palette& a, const Palette& b, float t) {
  return Palette{
      .primary = lerpColor(a.primary, b.primary, t),
      .onPrimary = lerpColor(a.onPrimary, b.onPrimary, t),
      .secondary = lerpColor(a.secondary, b.secondary, t),
      .onSecondary = lerpColor(a.onSecondary, b.onSecondary, t),
      .tertiary = lerpColor(a.tertiary, b.tertiary, t),
      .onTertiary = lerpColor(a.onTertiary, b.onTertiary, t),
      .error = lerpColor(a.error, b.error, t),
      .onError = lerpColor(a.onError, b.onError, t),
      .surface = lerpColor(a.surface, b.surface, t),
      .onSurface = lerpColor(a.onSurface, b.onSurface, t),
      .surfaceVariant = lerpColor(a.surfaceVariant, b.surfaceVariant, t),
      .onSurfaceVariant = lerpColor(a.onSurfaceVariant, b.onSurfaceVariant, t),
      .outline = lerpColor(a.outline, b.outline, t),
      .shadow = lerpColor(a.shadow, b.shadow, t),
      .hover = lerpColor(a.hover, b.hover, t),
      .onHover = lerpColor(a.onHover, b.onHover, t),
  };
}
