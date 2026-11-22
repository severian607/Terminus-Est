#pragma once

#include "shell/bar/widget.h"
#include "shell/bar/widget_custom_image.h"

#include <array>
#include <cstdint>
#include <string>

class Box;
class FileWatcher;
class Glyph;
class Image;
class InputArea;
class Label;

class CustomButtonWidget : public Widget {
public:
  struct Options {
    std::string glyph = "heart";
    std::string customImage;
    bool customImageColorize = false;
    std::string label;
    std::string tooltip;
    bool markup = false;
    // While `activeFile` holds the word "open", `activeLabel` (markup) fades in over `label` and a soft glow
    // of `glowColor` fades in behind it. A zero `glowWidth` or an empty `glowColor` draws no glow.
    std::string activeFile;
    std::string activeLabel;
    std::string glowColor;
    double glowWidth = 0.0;
    double glowOffset = 0.0;
  };

  CustomButtonWidget(Options options, FileWatcher* fileWatcher);
  ~CustomButtonWidget() override;

  void create() override;
  void onFrameTick(float deltaMs) override;
  [[nodiscard]] bool needsFrameTick() const override;

private:
  void doLayout(Renderer& renderer, float containerWidth, float containerHeight) override;
  [[nodiscard]] bool readActive() const;
  void applyActiveProgress();
  void refreshPaletteText();

  std::string m_glyphName;
  std::string m_labelText;
  std::string m_tooltip;
  WidgetCustomImage m_customImage;
  bool m_markup = false;
  Signal<>::ScopedConnection m_paletteConn;
  std::string m_activeFile;
  std::string m_activeLabelText;
  ColorSpec m_glowColor;
  float m_glowWidth = 0.0F;
  float m_glowOffset = 0.0F;
  FileWatcher* m_fileWatcher = nullptr;
  std::uint64_t m_watchId = 0;
  bool m_active = false;
  float m_progress = 0.0F;
  InputArea* m_area = nullptr;
  Glyph* m_glyph = nullptr;
  Image* m_image = nullptr;
  Label* m_label = nullptr;
  Label* m_overlay = nullptr;
  // Concentric rounded boxes, outermost first; together they fall off like a blur.
  std::array<Box*, 4> m_glow{};
};
