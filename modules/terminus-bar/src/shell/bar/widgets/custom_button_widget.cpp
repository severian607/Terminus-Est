#include "shell/bar/widgets/custom_button_widget.h"

#include "config/color_spec.h"
#include "core/files/file_watcher.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "render/scene/node.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <fstream>
#include <memory>
#include <string_view>
#include <utility>

namespace {

  constexpr float kGlowFadeMs = 150.0F;
  // Opacity of each glow layer at full fade-in; the layers add up towards the centre.
  constexpr float kGlowLayerOpacity = 0.15F;
  constexpr float kGlowHeight = 20.0F;
  constexpr float kGlowSoftness = 6.0F;
  constexpr float kGlowLayerGrow = 2.0F;
  constexpr float kGlowLayerShrink = 2.0F;

  // `$XDG_RUNTIME_DIR` and a leading `~` are expanded so the same setting works on every machine.
  std::string expandStatePath(std::string path) {
    constexpr std::string_view kRuntimeDir = "$XDG_RUNTIME_DIR";
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (path.starts_with(kRuntimeDir) && runtime != nullptr) {
      path.replace(0, kRuntimeDir.size(), runtime);
    } else if (path.starts_with("~/")) {
      if (const char* home = std::getenv("HOME"); home != nullptr) {
        path.replace(0, 1, home);
      }
    }
    return path;
  }

  std::string toHex(const Color& color) {
    const auto channel = [](float value) {
      return static_cast<int>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
    };
    return std::format("#{:02x}{:02x}{:02x}", channel(color.r), channel(color.g), channel(color.b));
  }

  // Scales the HSV saturation (keeping hue and value), for palettes whose accent is too pale to read as a colour.
  Color saturate(const Color& color, float factor) {
    const float maxChannel = std::max({color.r, color.g, color.b});
    const float minChannel = std::min({color.r, color.g, color.b});
    if (factor == 1.0F || maxChannel <= minChannel) {
      return color;
    }
    const float scale = std::min(factor, maxChannel / (maxChannel - minChannel));
    return rgba(
        maxChannel - (maxChannel - color.r) * scale, maxChannel - (maxChannel - color.g) * scale,
        maxChannel - (maxChannel - color.b) * scale, color.a
    );
  }

  // Replaces `@role@`, `@role*SS@`, `@role/NN@` and `@role*SS/NN@` in markup with a `#rrggbb` taken from the current
  // palette, so labels follow the wallpaper palette. `SS` is the saturation in percent (default 100) and `NN` the
  // percentage of the colour mixed over the surface colour (default 100). Anything else between two `@` stays as
  // written.
  std::string expandPaletteTokens(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
      if (text[i] == '@') {
        if (const std::size_t end = text.find('@', i + 1); end != std::string_view::npos) {
          std::string_view token = text.substr(i + 1, end - i - 1);
          int percent = 100;
          if (const std::size_t slash = token.find('/'); slash != std::string_view::npos) {
            const std::string_view digits = token.substr(slash + 1);
            std::from_chars(digits.data(), digits.data() + digits.size(), percent);
            token = token.substr(0, slash);
          }
          int saturation = 100;
          if (const std::size_t star = token.find('*'); star != std::string_view::npos) {
            const std::string_view digits = token.substr(star + 1);
            std::from_chars(digits.data(), digits.data() + digits.size(), saturation);
            token = token.substr(0, star);
          }
          if (const auto role = colorRoleFromToken(token); role.has_value()) {
            Color color = saturate(colorForRole(*role), static_cast<float>(saturation) / 100.0F);
            if (percent < 100) {
              const Color base = colorForRole(ColorRole::Surface);
              const float t = static_cast<float>(std::clamp(percent, 0, 100)) / 100.0F;
              color = rgba(
                  color.r * t + base.r * (1.0F - t), color.g * t + base.g * (1.0F - t),
                  color.b * t + base.b * (1.0F - t)
              );
            }
            out += toHex(color);
            i = end + 1;
            continue;
          }
        }
      }
      out.push_back(text[i]);
      ++i;
    }
    return out;
  }

} // namespace

CustomButtonWidget::CustomButtonWidget(Options options, FileWatcher* fileWatcher)
    : m_glyphName(StringUtils::trim(options.glyph)), m_labelText(StringUtils::trim(options.label)),
      m_tooltip(StringUtils::trim(options.tooltip)),
      m_customImage(widget_custom_image::fromConfig(options.customImage, options.customImageColorize)),
      m_markup(options.markup), m_activeFile(expandStatePath(StringUtils::trim(options.activeFile))),
      m_activeLabelText(StringUtils::trim(options.activeLabel)), m_glowWidth(static_cast<float>(options.glowWidth)),
      m_glowOffset(static_cast<float>(options.glowOffset)), m_fileWatcher(fileWatcher) {
  if (const std::string glowColor = StringUtils::trim(options.glowColor); !glowColor.empty()) {
    m_glowColor = colorSpecFromConfigString(glowColor, "custom_button glow_color");
  } else {
    m_glowWidth = 0.0F;
  }
}

CustomButtonWidget::~CustomButtonWidget() {
  if (m_fileWatcher != nullptr && m_watchId != 0) {
    m_fileWatcher->unwatch(m_watchId);
  }
}

bool CustomButtonWidget::readActive() const {
  std::ifstream in(m_activeFile);
  std::string word;
  in >> word;
  return word == "open";
}

bool CustomButtonWidget::needsFrameTick() const { return m_progress != (m_active ? 1.0F : 0.0F); }

void CustomButtonWidget::onFrameTick(float deltaMs) {
  const float target = m_active ? 1.0F : 0.0F;
  if (m_progress == target) {
    return;
  }
  const float step = deltaMs / kGlowFadeMs;
  m_progress = m_progress < target ? std::min(target, m_progress + step) : std::max(target, m_progress - step);
  applyActiveProgress();
  requestRedraw();
}

void CustomButtonWidget::applyActiveProgress() {
  const float eased = m_progress * m_progress * (3.0F - 2.0F * m_progress);
  if (m_overlay != nullptr) {
    m_overlay->setOpacity(eased);
  }
  for (Box* layer : m_glow) {
    if (layer != nullptr) {
      layer->setOpacity(eased * kGlowLayerOpacity);
    }
  }
}

void CustomButtonWidget::refreshPaletteText() {
  if (m_label != nullptr && m_markup) {
    m_label->setText(expandPaletteTokens(m_labelText));
  }
  if (m_overlay != nullptr) {
    m_overlay->setText(expandPaletteTokens(m_activeLabelText));
  }
  requestRedraw();
}

void CustomButtonWidget::create() {
  auto area = ui::inputArea({});

  if (!m_tooltip.empty()) {
    area->setTooltip(m_tooltip);
  }

  if (m_glowWidth > 0.0F) {
    for (Box*& layer : m_glow) {
      area->addChild(
          ui::box({
              .out = &layer,
              .fill = m_glowColor,
              .opacity = 0.0F,
          })
      );
    }
  }

  if (m_customImage.enabled()) {
    area->addChild(ui::image({.out = &m_image, .fit = ImageFit::Contain}));
  } else {
    area->addChild(
        ui::glyph({
            .out = &m_glyph,
            .glyph = m_glyphName,
            .glyphSize = Style::baseGlyphSize * m_contentScale,
            .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
            .visible = !m_glyphName.empty(),
        })
    );
  }

  area->addChild(
      ui::label({
          .out = &m_label,
          .text = m_markup ? expandPaletteTokens(m_labelText) : m_labelText,
          .fontSize = Style::fontSizeBody * fontScale(),
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
          .maxLines = 1,
          .visible = !m_labelText.empty(),
          .configure = [markup = m_markup](Label& label) { label.setUseMarkup(markup); },
      })
  );

  if (!m_activeLabelText.empty()) {
    area->addChild(
        ui::label({
            .out = &m_overlay,
            .text = expandPaletteTokens(m_activeLabelText),
            .fontSize = Style::fontSizeBody * fontScale(),
            .fontWeight = labelFontWeight(),
            .fontFamily = labelFontFamily(),
            .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
            .maxLines = 1,
            .opacity = 0.0F,
            .configure = [](Label& label) { label.setUseMarkup(true); },
        })
    );
  }

  if (!m_activeFile.empty()) {
    m_active = readActive();
    m_progress = m_active ? 1.0F : 0.0F;
    applyActiveProgress();
    if (m_fileWatcher != nullptr && m_watchId == 0) {
      m_watchId = m_fileWatcher->watch(
          m_activeFile,
          [this] {
            m_active = readActive();
            requestFrameTick();
          },
          FileWatcher::WatchTrigger::WriteCompleted
      );
    }
  }

  if (m_markup || m_overlay != nullptr) {
    m_paletteConn = paletteChanged().connect([this] { refreshPaletteText(); });
  }

  m_area = area.get();
  setRoot(std::move(area));
}

void CustomButtonWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  if (m_area == nullptr || m_label == nullptr) {
    return;
  }

  const bool isVertical = containerHeight > containerWidth;
  const bool showImage = m_image != nullptr;
  const bool showGlyph = !showImage && m_glyph != nullptr && !m_glyphName.empty();
  const bool showIcon = showImage || showGlyph;
  const bool showLabel = !m_labelText.empty();
  const float spacing = (showIcon && showLabel) ? Style::spaceXs * m_contentScale : 0.0F;

  if (m_glyph != nullptr) {
    m_glyph->setVisible(showGlyph);
  }
  m_label->setVisible(showLabel);
  // The active overlay and the glow are drawn only on horizontal bars.
  if (m_overlay != nullptr) {
    m_overlay->setVisible(showLabel && !isVertical);
  }
  for (Box* layer : m_glow) {
    if (layer != nullptr) {
      layer->setVisible(!isVertical);
    }
  }

  if (showImage) {
    widget_custom_image::sync(
        *m_image, renderer, m_customImage, m_contentScale, widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface))
    );
  } else if (showGlyph) {
    m_glyph->setGlyphSize(Style::baseGlyphSize * m_contentScale);
    m_glyph->setColor(widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_glyph->measure(renderer);
  }

  if (showLabel) {
    m_label->setFontSize((isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * fontScale());
    m_label->setFontWeight(labelFontWeight());
    m_label->setTextAlign(isVertical ? TextAlign::Center : TextAlign::Start);
    m_label->setMaxWidth(isVertical ? containerWidth : 0.0F);
    m_label->setColor(widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)));
    m_label->measure(renderer);
    if (m_overlay != nullptr) {
      m_overlay->setFontSize((isVertical ? Style::fontSizeCaption : Style::fontSizeBody) * fontScale());
      m_overlay->setFontWeight(labelFontWeight());
      m_overlay->setTextAlign(isVertical ? TextAlign::Center : TextAlign::Start);
      m_overlay->setMaxWidth(isVertical ? containerWidth : 0.0F);
      m_overlay->measure(renderer);
    }
  }

  if (isVertical) {
    float width = 0.0F;
    float height = 0.0F;
    if (showImage) {
      width = std::max(width, m_image->width());
      height += m_image->height();
    } else if (showGlyph) {
      width = std::max(width, m_glyph->width());
      height += m_glyph->height();
    }
    if (showLabel) {
      if (height > 0.0F) {
        height += spacing;
      }
      width = std::max(width, m_label->width());
      height += m_label->height();
    }

    float y = 0.0F;
    if (showImage) {
      m_image->setPosition(std::round((width - m_image->width()) * 0.5F), y);
      y += m_image->height() + spacing;
    } else if (showGlyph) {
      m_glyph->setPosition(std::round((width - m_glyph->width()) * 0.5F), y);
      y += m_glyph->height() + spacing;
    }
    if (showLabel) {
      m_label->setPosition(std::round((width - m_label->width()) * 0.5F), y);
    }
    m_area->setSize(width, height);
    return;
  }

  float width = 0.0F;
  float height = 0.0F;
  if (showImage) {
    width += m_image->width();
    height = std::max(height, m_image->height());
  } else if (showGlyph) {
    width += m_glyph->width();
    height = std::max(height, m_glyph->height());
  }
  if (showLabel) {
    if (width > 0.0F) {
      width += spacing;
    }
    width += m_label->width();
    height = std::max(height, m_label->height());
  }

  float x = 0.0F;
  if (showImage) {
    m_image->setPosition(x, std::round((height - m_image->height()) * 0.5F));
    x += m_image->width() + spacing;
  } else if (showGlyph) {
    m_glyph->setPosition(x, std::round((height - m_glyph->height()) * 0.5F));
    x += m_glyph->width() + spacing;
  }
  if (showLabel) {
    m_label->setPosition(x, std::round((height - m_label->height()) * 0.5F));
    if (m_overlay != nullptr) {
      m_overlay->setPosition(x, std::round((height - m_overlay->height()) * 0.5F));
    }
  }
  if (m_glowWidth > 0.0F) {
    const float centerY = height * 0.5F;
    for (std::size_t i = 0; i < m_glow.size(); ++i) {
      const float layerHeight = (kGlowHeight - static_cast<float>(i) * kGlowLayerShrink) * m_contentScale;
      const float grow = static_cast<float>(m_glow.size() - 1 - i) * kGlowLayerGrow * m_contentScale;
      const float layerWidth = m_glowWidth * m_contentScale + 2.0F * grow;
      m_glow[i]->setRadius(layerHeight * 0.5F);
      m_glow[i]->setSoftness(kGlowSoftness * m_contentScale);
      m_glow[i]->setSize(layerWidth, layerHeight);
      m_glow[i]->setPosition(x + m_glowOffset * m_contentScale - grow, std::round(centerY - layerHeight * 0.5F));
    }
  }
  m_area->setSize(width, height);
}
