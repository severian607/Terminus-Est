#pragma once

#include "render/core/color.h"
#include "ui/palette.h"

namespace semantic_accent {

  // Sakura Mochi: fixed colours for the file dialog and the audio tab.
  inline constexpr Color files = rgbHex(0x6B4A82);
  inline constexpr Color filesBright = rgbHex(0xA07ABE);
  inline constexpr Color audio = rgbHex(0xD7BE96); // warm support cream

  // System state reads as the green root; it follows the palette.
  [[nodiscard]] inline ColorSpec systemSpec(float alpha = 1.0F) { return colorSpecFromRole(ColorRole::Primary, alpha); }

  [[nodiscard]] inline ColorSpec filesSpec(float alpha = 1.0F) {
    auto spec = fixedColorSpec(files);
    spec.alpha = alpha;
    return spec;
  }

  [[nodiscard]] inline ColorSpec filesBrightSpec(float alpha = 1.0F) {
    auto spec = fixedColorSpec(filesBright);
    spec.alpha = alpha;
    return spec;
  }

  [[nodiscard]] inline ColorSpec audioSpec(float alpha = 1.0F) {
    auto spec = fixedColorSpec(audio);
    spec.alpha = alpha;
    return spec;
  }

} // namespace semantic_accent
