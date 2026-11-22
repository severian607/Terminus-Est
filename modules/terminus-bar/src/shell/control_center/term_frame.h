#pragma once

#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/grid_view.h"
#include "ui/controls/label.h"
#include "ui/controls/separator.h"
#include "ui/controls/slider.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// btop-style building blocks for the Home dashboard: labelled square boxes, bordered tiles and
// read-only block meters. Geometry stays square, structure is 1px lines, brightness (Primary)
// marks state only.
namespace term_frame {

  inline constexpr const char* kFont = "JetBrainsMonoNL Nerd Font";
  inline constexpr std::string_view kMarkerOn = "●";
  inline constexpr std::string_view kMarkerOff = "○";
  inline constexpr std::string_view kMarkerNeutral = "▸";

  [[nodiscard]] inline ColorSpec lineColor() { return colorSpecFromRole(ColorRole::Outline, 0.7F); }
  [[nodiscard]] inline ColorSpec tileLineColor() { return colorSpecFromRole(ColorRole::Outline, 0.5F); }
  [[nodiscard]] inline ColorSpec dimColor() { return colorSpecFromRole(ColorRole::OnSurfaceVariant); }

  // Border + padding a box adds on each side; callers subtract twice from the outer width.
  [[nodiscard]] inline float boxInset(float scale) { return Style::spaceSm * scale + Style::borderWidth; }

  // Height a labelled box adds around its body: border and padding on both sides, the title row and the gap
  // under it. Unscaled, like kSectionGap, the vertical gap between two boxes of a panel.
  inline constexpr float kBoxChrome = 2.0F * (Style::spaceSm + Style::borderWidth) + 20.0F + Style::spaceXs;
  inline constexpr float kSectionGap = Style::spaceSm;

  // Shows or hides a node together with its place in the layout; true when that changed anything.
  inline bool setShown(Node* node, bool shown) {
    if (node == nullptr || (node->visible() == shown && node->participatesInLayout() == shown)) {
      return false;
    }
    node->setVisible(shown);
    node->setParticipatesInLayout(shown);
    return true;
  }

  // Width of `chars` monospace cells at `fontSize` (JetBrains Mono advances 0.6em).
  [[nodiscard]] inline float monoWidth(std::size_t chars, float fontSize) {
    return static_cast<float>(chars) * fontSize * 0.6F;
  }

  // ASCII-lowercased copy; bytes outside ASCII (other scripts) pass through untouched.
  [[nodiscard]] inline std::string lowerAscii(std::string value) {
    for (char& c : value) {
      if (c >= 'A' && c <= 'Z') {
        c = static_cast<char>(c - 'A' + 'a');
      }
    }
    return value;
  }

  // A square 1px-bordered column with no fill (the panel provides the glass).
  [[nodiscard]] inline std::unique_ptr<Flex> frame(float scale, float padding, Flex** out = nullptr) {
    return ui::column({
        .out = out,
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
        .padding = padding,
        .radius = 0.0F,
        .border = lineColor(),
        .borderWidth = Style::borderWidth,
    });
  }

  // `[ text ]` live-status chip; an empty text hides it.
  inline void setChip(Label& chip, std::string_view text) {
    const bool show = !text.empty();
    if (chip.visible() != show) {
      chip.setVisible(show);
    }
    if (show) {
      chip.setText("[ " + std::string(text) + " ]");
    }
  }

  // `numeral` is a superscript digit ("¹"); the rule after the title fills the row like btop's
  // labelled borders. With `chipOut` a dim `[ status ]` chip follows the rule.
  [[nodiscard]] inline std::unique_ptr<Flex>
  titleRow(std::string_view numeral, std::string_view title, float scale, Label** chipOut = nullptr) {
    auto row = ui::row(
        {.align = FlexAlign::Center, .gap = Style::spaceXs * scale},
        ui::label({
            .text = std::string(numeral) + std::string(title),
            .fontSize = Style::fontSizeCaption * scale,
            .fontFamily = std::string(kFont),
            .color = colorSpecFromRole(ColorRole::Primary),
            .maxLines = 1,
        }),
        ui::separator({
            .color = lineColor(),
            .thickness = Style::borderWidth,
            .orientation = SeparatorOrientation::HorizontalRule,
            .gradientEdges = false,
            .flexGrow = 1.0F,
        })
    );
    if (chipOut != nullptr) {
      row->addChild(
          ui::label({
              .out = chipOut,
              .fontSize = Style::fontSizeCaption * scale,
              .fontFamily = std::string(kFont),
              .color = dimColor(),
              .maxLines = 1,
              .visible = false,
          })
      );
    }
    return row;
  }

  // A frame whose first child is the title row; callers append content.
  [[nodiscard]] inline std::unique_ptr<Flex>
  box(std::string_view numeral, std::string_view title, float scale, Flex** out = nullptr, Label** chipOut = nullptr) {
    auto container = frame(scale, Style::spaceSm * scale, out);
    container->addChild(titleRow(numeral, title, scale, chipOut));
    return container;
  }

  // Square bordered tile. At rest a dim 1px edge; hover/focus and the selected (active) state use the
  // lit edge (Primary 12% fill + Primary border). Extra Label/Glyph children follow the state colour.
  [[nodiscard]] inline Button::ButtonPalette tilePalette(bool dimLabel) {
    using State = Button::ButtonStateColors;
    const ColorSpec rest = dimLabel ? dimColor() : colorSpecFromRole(ColorRole::OnSurface);
    return {
        .borderWidth = Style::borderWidth,
        .normal = State{.bg = clearColorSpec(), .border = tileLineColor(), .label = rest},
        .hover =
            State{
                .bg = colorSpecFromRole(ColorRole::Primary, 0.12F),
                .border = colorSpecFromRole(ColorRole::Primary, 0.7F),
                .label = colorSpecFromRole(ColorRole::Primary),
            },
        .pressed =
            State{
                .bg = colorSpecFromRole(ColorRole::Primary, 0.22F),
                .border = colorSpecFromRole(ColorRole::Primary, 0.9F),
                .label = colorSpecFromRole(ColorRole::Primary),
            },
        .disabled =
            State{
                .bg = clearColorSpec(),
                .border = colorSpecFromRole(ColorRole::Outline, 0.3F),
                .label = colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.5F),
            },
        .selected = State{
            .bg = colorSpecFromRole(ColorRole::Primary, 0.12F),
            .border = colorSpecFromRole(ColorRole::Primary, 0.7F),
            .label = colorSpecFromRole(ColorRole::Primary),
        },
    };
  }

  inline void setMarker(Label& marker, bool toggle, bool on) {
    marker.setText(toggle ? (on ? kMarkerOn : kMarkerOff) : kMarkerNeutral);
  }

  struct TileSpec {
    std::string glyph;
    float glyphSize = 0.0F;
    // Empty label keeps a glyph-only tile (the rail); the tooltip names it.
    std::string label;
    std::string tooltip;
    // When set, a state marker is right-aligned and written to *marker.
    Label** marker = nullptr;
    bool toggle = false;
    bool on = false;
    bool dimWhenOff = false;
    bool center = false;
    std::function<void()> onClick;
    std::function<void()> onRightClick;
  };

  [[nodiscard]] inline std::unique_ptr<Button> tile(TileSpec spec, float scale, Button** out = nullptr) {
    const bool hasLabel = !spec.label.empty();
    auto button = ui::button({
        .out = out,
        .text = hasLabel ? std::optional<std::string>{spec.label} : std::nullopt,
        .glyph = spec.glyph.empty() ? std::nullopt : std::optional<std::string>{spec.glyph},
        .fontSize = Style::fontSizeCaption * scale,
        .glyphSize = spec.glyphSize > 0.0F ? std::optional<float>{spec.glyphSize} : std::nullopt,
        .selected = spec.on,
        .contentAlign = spec.center ? ButtonContentAlign::Center : ButtonContentAlign::Start,
        .variant = ButtonVariant::Outline,
        .customPalette = tilePalette(spec.dimWhenOff),
        .surfaceOpacity = 1.0F,
        .tooltip = spec.tooltip,
        .minHeight = 0.0F,
        .paddingV = Style::spaceXs * scale,
        .paddingH = Style::spaceSm * scale,
        .gap = Style::spaceSm * scale,
        .radius = 0.0F,
        .onClick = std::move(spec.onClick),
        .onRightClick = std::move(spec.onRightClick),
        .configure = [](Button& control) {
          control.setDirection(FlexDirection::Horizontal);
          control.setAlign(FlexAlign::Center);
          if (control.label() != nullptr) {
            control.label()->setFontFamily(kFont);
            control.label()->setMaxLines(1);
            control.label()->setEllipsize(TextEllipsize::End);
            control.label()->setFlexGrow(1.0F);
          }
        },
    });
    if (spec.marker != nullptr) {
      auto marker = ui::label({
          .fontSize = Style::fontSizeCaption * scale,
          .fontFamily = std::string(kFont),
          .maxLines = 1,
      });
      setMarker(*marker, spec.toggle, spec.on);
      *spec.marker = marker.get();
      button->addChild(std::move(marker));
    }
    return button;
  }

  // Dim fixed-width tag + display-only block meter + value, as in btop's meter rows.
  struct MeterRow {
    Flex* row = nullptr;
    Label* tag = nullptr;
    Slider* slider = nullptr;
    Label* value = nullptr;
  };

  [[nodiscard]] inline std::unique_ptr<Flex> meterRow(std::string_view tag, float scale, MeterRow& out) {
    auto row = ui::row({
        .out = &out.row,
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .fillWidth = true,
    });
    row->addChild(
        ui::label({
            .out = &out.tag,
            .text = std::string(tag),
            .fontSize = Style::fontSizeCaption * scale,
            .fontFamily = std::string(kFont),
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 1,
        })
    );
    row->addChild(
        ui::slider({
            .out = &out.slider,
            .minValue = 0.0F,
            .maxValue = 1.0F,
            .step = 0.001F,
            .trackHeight = Style::sliderTrackHeight * scale,
            .controlHeight = 22.0F * scale,
            .width = 1.0F,
            .flexGrow = 1.0F,
            .configure = [](Slider& meter) { meter.setReadOnly(true); },
        })
    );
    row->addChild(
        ui::label({
            .out = &out.value,
            .fontSize = Style::fontSizeCaption * scale,
            .fontFamily = std::string(kFont),
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 1,
            .textAlign = TextAlign::End,
        })
    );
    return row;
  }

  // ---- Shared character grid -------------------------------------------------------------------
  // Every dashboard row is kRowHeight tall; label, value and marker columns have fixed widths in
  // monospace cells so meters in different boxes start and end on the same x positions.
  inline constexpr float kRowHeight = 22.0F;
  inline constexpr std::size_t kTagChars = 8;
  inline constexpr std::size_t kValueChars = 5;
  inline constexpr std::size_t kKeyChars = 6;

  // Dim `key` + value on a kRowHeight row (the fetch-style rows of the weather tab).
  struct KeyValueRow {
    Flex* row = nullptr;
    Label* key = nullptr;
    Label* value = nullptr;
  };

  [[nodiscard]] inline std::unique_ptr<Flex>
  keyValueRow(std::string_view key, float scale, KeyValueRow& out, std::size_t keyChars = kKeyChars) {
    const float font = Style::fontSizeCaption * scale;
    auto row = ui::row({
        .out = &out.row,
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .minHeight = kRowHeight * scale,
        .fillWidth = true,
    });
    row->addChild(
        ui::label({
            .out = &out.key,
            .text = std::string(key),
            .fontSize = font,
            .fontFamily = std::string(kFont),
            .color = dimColor(),
            .minWidth = monoWidth(keyChars, font),
            .maxLines = 1,
        })
    );
    row->addChild(
        ui::label({
            .out = &out.value,
            .fontSize = font,
            .fontFamily = std::string(kFont),
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 1,
            .ellipsize = TextEllipsize::End,
        })
    );
    return row;
  }

  // Pins the key column and caps the value to the rest of `width`; the row is `rowHeight` tall.
  inline void
  layoutKeyValueRowH(KeyValueRow& kv, float width, float rowHeight, float scale, std::size_t keyChars = kKeyChars) {
    const float font = Style::fontSizeCaption * scale;
    const float keyWidth = monoWidth(keyChars, font);
    const float gap = Style::spaceSm * scale;
    kv.row->setGap(gap);
    kv.key->setFontSize(font);
    kv.key->setMinWidth(keyWidth);
    kv.value->setFontSize(font);
    kv.value->setMaxWidth(std::max(1.0F, width - keyWidth - gap));
    kv.row->setMinHeight(rowHeight);
    kv.row->setMaxHeight(rowHeight);
    kv.row->setSize(width, rowHeight);
  }

  // Sizes a tag | meter | value [| trail] row to `width` x kRowHeight on explicit column widths and returns the
  // height. `trailWidth` 0 omits the trailing column from the arithmetic (the label may be null).
  inline float layoutMeterCells(
      Flex& row, Label& tag, Slider& slider, Label& value, Label* trail, float width, float tagWidth, float valueWidth,
      float trailWidth, float scale
  ) {
    const float height = kRowHeight * scale;
    const float font = Style::fontSizeCaption * scale;
    const float gap = Style::spaceSm * scale;
    const bool hasTrail = trail != nullptr && trailWidth > 0.0F;
    const float cells = hasTrail ? 4.0F : 3.0F;
    const float track =
        std::max(1.0F, width - tagWidth - valueWidth - (hasTrail ? trailWidth : 0.0F) - (cells - 1.0F) * gap);
    row.setGap(gap);
    for (auto [label, cellWidth] : {std::pair{&tag, tagWidth}, std::pair{&value, valueWidth}}) {
      label->setFontSize(font);
      label->setMinWidth(cellWidth);
      label->setMaxWidth(std::max(1.0F, cellWidth));
      label->setMaxLines(1);
      label->setEllipsize(TextEllipsize::End);
    }
    if (trail != nullptr) {
      trail->setFontSize(font);
      trail->setMinWidth(trailWidth);
      trail->setMaxWidth(std::max(1.0F, trailWidth));
      trail->setMaxLines(1);
    }
    slider.setMinWidth(1.0F);
    slider.setMaxWidth(track);
    slider.setControlHeight(height);
    slider.setTrackHeight(Style::sliderTrackHeight * scale);
    slider.setSize(track, height);
    row.setMinHeight(height);
    row.setMaxHeight(height);
    row.setSize(width, height);
    return height;
  }

  // Pins a Flex to an exact frame (min = max = size) so its parent cannot grow or shrink it.
  inline void pinSize(Flex& node, float width, float height) {
    node.setMinWidth(width);
    node.setMaxWidth(width);
    node.setMinHeight(height);
    node.setMaxHeight(height);
    node.setSize(width, height);
  }

  // ---- btop box and keywords -------------------------------------------------------------------
  // The embedded btop (assets/btop/terminusbar.conf with the generated theme) is the reference: box lines are
  // the theme's Outline, the title Primary, other text OnSurface, 14 px JetBrainsMonoNL, and the title and the
  // info on the right are set into the top border between small ticks.
  inline constexpr float kBtopFontSize = 14.0F;

  [[nodiscard]] inline float btopTitleHeight(float scale) { return std::round(kBtopFontSize * scale * 1.45F); }

  // Pango markup needs hex colours; resolved from the live palette, so callers refresh on palette changes.
  [[nodiscard]] inline std::string colorHex(const Color& color) {
    const auto channel = [](float value) {
      return static_cast<int>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
    };
    return std::format("#{:02x}{:02x}{:02x}", channel(color.r), channel(color.g), channel(color.b));
  }

  [[nodiscard]] inline std::string paletteHex(ColorRole role) { return colorHex(colorForRole(role)); }

  // `text` with the characters Pango markup reserves escaped.
  [[nodiscard]] inline std::string escapeMarkup(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
      switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      default:
        out += c;
      }
    }
    return out;
  }

  // Padding a btop frame adds around its content: half the title above the top border plus the box inset on both
  // sides.
  [[nodiscard]] inline float btopChrome(float scale) {
    return std::round(btopTitleHeight(scale) * 0.5F) + 2.0F * boxInset(scale);
  }

  class BtopFrame final : public Flex {
  public:
    BtopFrame(std::string_view numeral, std::string_view title, float scale) : m_scale(scale) {
      setDirection(FlexDirection::Vertical);
      setAlign(FlexAlign::Stretch);
      setGap(Style::spaceXs * scale);
      const float inset = boxInset(scale);
      setPadding(std::round(btopTitleHeight(scale) * 0.5F) + inset, inset, inset, inset);
      for (Box*& line : m_lines) {
        addChild(
            ui::box({
                .out = &line,
                .fill = colorSpecFromRole(ColorRole::Outline),
                .radius = 0.0F,
                .participatesInLayout = false,
            })
        );
      }
      addChild(
          ui::label({
              .out = &m_title,
              .text = std::string(numeral) + std::string(title),
              .fontSize = kBtopFontSize * scale,
              .fontFamily = std::string(kFont),
              .color = colorSpecFromRole(ColorRole::Primary),
              .maxLines = 1,
              .participatesInLayout = false,
          })
      );
      addChild(
          ui::label({
              .out = &m_info,
              .fontSize = kBtopFontSize * scale,
              .fontFamily = std::string(kFont),
              .color = colorSpecFromRole(ColorRole::OnSurface),
              .maxLines = 1,
              .visible = false,
              .participatesInLayout = false,
          })
      );
    }

    // Text set into the top border on the right; empty hides it.
    void setInfo(std::string_view text) {
      const bool show = !text.empty();
      if (m_info->visible() != show) {
        m_info->setVisible(show);
      }
      if (show) {
        m_info->setText(text);
      }
    }

    // Colour of the text in the top border; callers pass the warning role for "needs action".
    void setInfoColor(const ColorSpec& color) { m_info->setColor(color); }

  protected:
    void doLayout(Renderer& renderer) override {
      Flex::doLayout(renderer);
      placeChrome(renderer);
    }
    void doArrange(Renderer& renderer, const LayoutRect& rect) override {
      Flex::doArrange(renderer, rect);
      placeChrome(renderer);
    }

  private:
    enum Line : std::size_t {
      Left,
      Right,
      Bottom,
      TopStart,
      TopMid,
      TopEnd,
      TitleTickLeft,
      TitleTickRight,
      InfoTickLeft,
      InfoTickRight,
      LineCount,
    };

    void put(Line line, float x, float y, float width, float height) {
      Box* box = m_lines[line];
      const bool show = width > 0.0F && height > 0.0F;
      box->setVisible(show);
      if (show) {
        box->setPosition(x, y);
        box->setSize(width, height);
      }
    }

    void placeChrome(Renderer& renderer) {
      const float w = width();
      const float h = height();
      if (w <= 0.0F || h <= 0.0F) {
        return;
      }
      const float t = Style::borderWidth;
      const float titleH = btopTitleHeight(m_scale);
      const float lineY = std::round(titleH * 0.5F);
      const float tickH = std::round(titleH * 0.4F);
      const float dash = std::round(Style::spaceMd * m_scale);
      const float pad = std::round(Style::spaceXs * m_scale);

      m_title->measure(renderer);
      const float titleTickLeftX = dash;
      const float titleX = titleTickLeftX + t + pad;
      const float titleTickRightX = titleX + m_title->width() + pad;
      m_title->setPosition(titleX, std::round(lineY - m_title->height() * 0.5F));

      const bool info = m_info->visible();
      float midEnd = w - t;
      if (info) {
        m_info->measure(renderer);
        const float infoTickRightX = w - dash - t;
        const float infoX = infoTickRightX - pad - m_info->width();
        const float infoTickLeftX = infoX - pad - t;
        m_info->setPosition(infoX, std::round(lineY - m_info->height() * 0.5F));
        midEnd = infoTickLeftX;
        put(InfoTickLeft, infoTickLeftX, lineY, t, tickH);
        put(InfoTickRight, infoTickRightX, lineY, t, tickH);
        put(TopEnd, infoTickRightX, lineY, w - infoTickRightX, t);
      } else {
        put(InfoTickLeft, 0, 0, 0, 0);
        put(InfoTickRight, 0, 0, 0, 0);
        put(TopEnd, 0, 0, 0, 0);
      }
      put(Left, 0, lineY, t, h - lineY);
      put(Right, w - t, lineY, t, h - lineY);
      put(Bottom, 0, h - t, w, t);
      put(TopStart, 0, lineY, titleTickLeftX + t, t);
      put(TitleTickLeft, titleTickLeftX, lineY, t, tickH);
      put(TitleTickRight, titleTickRightX, lineY, t, tickH);
      put(TopMid, titleTickRightX, lineY, std::max(0.0F, midEnd - titleTickRightX + (info ? t : 0.0F)), t);
    }

    float m_scale = 1.0F;
    std::array<Box*, LineCount> m_lines{};
    Label* m_title = nullptr;
    Label* m_info = nullptr;
  };

  [[nodiscard]] inline std::unique_ptr<BtopFrame>
  btopBox(std::string_view numeral, std::string_view title, float scale, BtopFrame** out = nullptr) {
    auto frame = std::make_unique<BtopFrame>(numeral, title, scale);
    if (out != nullptr) {
      *out = frame.get();
    }
    return frame;
  }

  // A btop keyword: a plain word with its hotkey letter in the accent colour. `block` turns it into a solid
  // accent block with dark text (the active state, as the bar's workspace number) showing `blockText`.
  struct Keyword {
    Button* button = nullptr;
    std::string word;
    std::size_t hot = 0;
    bool block = false;
    std::string blockText;
    std::function<void()> activate; // what a click (or the hotkey) does
    bool warning = false;           // drawn in the warning role (reboot, shutdown)
  };

  inline constexpr float kKeywordPaddingH = 3.0F;
  inline constexpr float kKeywordRestAlpha = 0.6F;
  inline constexpr float kActionKeyPaddingH = 1.5F;
  inline constexpr float kActionKeyRestAlpha = 0.85F;

  inline void refreshKeyword(Keyword& keyword) {
    if (keyword.button == nullptr) {
      return;
    }
    std::string markup;
    if (keyword.block) {
      markup = keyword.blockText;
    } else if (keyword.hot >= keyword.word.size()) {
      // No hotkey letter (`hot` past the word), as for the timer presets.
      markup = keyword.word;
    } else {
      markup = keyword.word.substr(0, keyword.hot);
      markup += "<span foreground=\""
          + paletteHex(keyword.warning ? ColorRole::Warning : ColorRole::Tertiary)
          + "\" font_weight=\"bold\">";
      markup += keyword.word.substr(keyword.hot, 1);
      markup += "</span>";
      markup += keyword.word.substr(keyword.hot + 1);
    }
    keyword.button->setText(markup);
    keyword.button->setSelected(keyword.block);
  }

  // `restAlpha` is how bright a keyword rests; the action rows sit next to the bright names of the other tiles and
  // rest a step higher than the inline keywords.
  [[nodiscard]] inline Button::ButtonPalette keywordPalette(float restAlpha = kKeywordRestAlpha, bool warning = false) {
    using State = Button::ButtonStateColors;
    if (warning) {
      const State rest{
          .bg = clearColorSpec(), .border = clearColorSpec(), .label = colorSpecFromRole(ColorRole::Warning, 0.8F)
      };
      const State lit{
          .bg = clearColorSpec(), .border = clearColorSpec(), .label = colorSpecFromRole(ColorRole::Warning)
      };
      const State block{
          .bg = colorSpecFromRole(ColorRole::Warning),
          .border = colorSpecFromRole(ColorRole::Warning),
          .label = colorSpecFromRole(ColorRole::Surface),
      };
      return {.borderWidth = 0.0F, .normal = rest, .hover = lit, .pressed = lit, .disabled = rest, .selected = block};
    }
    const State rest{
        .bg = clearColorSpec(),
        .border = clearColorSpec(),
        .label = colorSpecFromRole(ColorRole::OnSurfaceVariant, restAlpha)
    };
    const State lit{.bg = clearColorSpec(), .border = clearColorSpec(), .label = colorSpecFromRole(ColorRole::Primary)};
    const State block{
        .bg = colorSpecFromRole(ColorRole::Primary),
        .border = colorSpecFromRole(ColorRole::Primary),
        .label = colorSpecFromRole(ColorRole::OnPrimary),
    };
    return {.borderWidth = 0.0F, .normal = rest, .hover = lit, .pressed = lit, .disabled = rest, .selected = block};
  }

  [[nodiscard]] inline std::unique_ptr<Button> makeKeyword(
      Keyword& keyword, std::string word, std::size_t hot, float scale, std::function<void()> onClick,
      std::function<void()> onLeave = nullptr, float paddingH = kKeywordPaddingH, float restAlpha = kKeywordRestAlpha,
      bool warning = false
  ) {
    keyword.warning = warning;
    keyword.activate = onClick;
    keyword.word = std::move(word);
    keyword.hot = hot;
    auto button = ui::button({
        .out = &keyword.button,
        .text = keyword.word,
        .fontSize = kBtopFontSize * scale,
        .contentAlign = ButtonContentAlign::Start,
        .variant = ButtonVariant::Ghost,
        .customPalette = keywordPalette(restAlpha, warning),
        .surfaceOpacity = 1.0F,
        .minHeight = 0.0F,
        .paddingV = 0.0F,
        .paddingH = paddingH * scale,
        .radius = 0.0F,
        .onClick = std::move(onClick),
        .onLeave = std::move(onLeave),
        .configure = [](Button& control) {
          if (control.label() != nullptr) {
            control.label()->setFontFamily(kFont);
            control.label()->setUseMarkup(true);
            control.label()->setMaxLines(1);
          }
        },
    });
    refreshKeyword(keyword);
    return button;
  }

  // ---- btop info rows and meters ---------------------------------------------------------------
  // Dim key on the left, value right-aligned, one btop-sized line.
  struct InfoRow {
    Flex* row = nullptr;
    Label* key = nullptr;
    Label* value = nullptr;
  };

  [[nodiscard]] inline std::unique_ptr<Flex> infoRow(std::string_view key, float scale, InfoRow& out) {
    auto row = ui::row({
        .out = &out.row,
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .minHeight = kRowHeight * scale,
        .fillWidth = true,
    });
    row->addChild(
        ui::label({
            .out = &out.key,
            .text = std::string(key),
            .fontSize = kBtopFontSize * scale,
            .fontFamily = std::string(kFont),
            .color = dimColor(),
            .maxLines = 1,
        })
    );
    row->addChild(
        ui::label({
            .out = &out.value,
            .fontSize = kBtopFontSize * scale,
            .fontFamily = std::string(kFont),
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .maxLines = 1,
            .textAlign = TextAlign::End,
            .ellipsize = TextEllipsize::End,
            .flexGrow = 1.0F,
        })
    );
    return row;
  }

  // btop's meter: filled cells in the theme gradient (Primary, Secondary, Tertiary along the full bar), the rest
  // as dim dots. Text only, so it carries no knob and follows the palette when refreshed.
  struct DottedMeter {
    Label* label = nullptr;
    std::size_t cells = 0;
    double fraction = 0.0;
    bool solid = false; // one colour (Primary) instead of the theme gradient
  };

  [[nodiscard]] inline std::size_t dottedMeterCells(float width, float scale) {
    const float cell = monoWidth(1, kBtopFontSize * scale);
    return cell > 0.0F ? static_cast<std::size_t>(std::max(0.0F, std::floor(width / cell))) : 0;
  }

  [[nodiscard]] inline std::string dottedMeterMarkup(std::size_t cells, double fraction, bool solid = false) {
    const Color primary = colorForRole(ColorRole::Primary);
    const std::array<Color, 3> stops{
        primary, solid ? primary : colorForRole(ColorRole::Secondary),
        solid ? primary : colorForRole(ColorRole::Tertiary)
    };
    const std::size_t filled = std::min(
        cells, static_cast<std::size_t>(std::lround(std::clamp(fraction, 0.0, 1.0) * static_cast<double>(cells)))
    );
    std::string out;
    for (std::size_t i = 0; i < filled; ++i) {
      const float position = cells > 1 ? static_cast<float>(i) / static_cast<float>(cells - 1) * 2.0F : 0.0F;
      const std::size_t from = std::min<std::size_t>(1, static_cast<std::size_t>(position));
      const float t = position - static_cast<float>(from);
      const Color& a = stops[from];
      const Color& b = stops[from + 1];
      out += "<span foreground=\""
          + colorHex(rgba(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t))
          + "\">■</span>";
    }
    if (filled < cells) {
      out += "<span foreground=\"" + paletteHex(ColorRole::Outline) + "\">";
      for (std::size_t i = filled; i < cells; ++i) {
        out += "·";
      }
      out += "</span>";
    }
    return out;
  }

  [[nodiscard]] inline std::unique_ptr<Label> dottedMeter(DottedMeter& out, float scale) {
    return ui::label({
        .out = &out.label,
        .fontSize = kBtopFontSize * scale,
        .fontFamily = std::string(kFont),
        .color = colorSpecFromRole(ColorRole::OnSurface),
        .maxLines = 1,
        .flexGrow = 1.0F,
        .configure = [](Label& meter) { meter.setUseMarkup(true); },
    });
  }

  inline void refreshDottedMeter(DottedMeter& meter) {
    if (meter.label != nullptr && meter.cells > 0) {
      meter.label->setText(dottedMeterMarkup(meter.cells, meter.fraction, meter.solid));
    }
  }

  // `tag  ■■■■■·····  42%`: the btop meter row of the weather tab.
  inline constexpr std::size_t kMeterTagChars = 4;
  inline constexpr std::size_t kMeterValueChars = 4;

  struct DottedMeterRow {
    Flex* row = nullptr;
    DottedMeter meter;
    Label* value = nullptr;
  };

  [[nodiscard]] inline std::unique_ptr<Flex> dottedMeterRow(std::string_view tag, float scale, DottedMeterRow& out) {
    const float font = kBtopFontSize * scale;
    auto row = ui::row({
        .out = &out.row,
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .minHeight = kRowHeight * scale,
        .fillWidth = true,
    });
    row->addChild(
        ui::label({
            .text = std::string(tag),
            .fontSize = font,
            .fontFamily = std::string(kFont),
            .color = dimColor(),
            .minWidth = monoWidth(kMeterTagChars, font),
            .maxLines = 1,
        })
    );
    row->addChild(dottedMeter(out.meter, scale));
    row->addChild(
        ui::label({
            .out = &out.value,
            .fontSize = font,
            .fontFamily = std::string(kFont),
            .color = colorSpecFromRole(ColorRole::OnSurface),
            .minWidth = monoWidth(kMeterValueChars, font),
            .maxLines = 1,
            .textAlign = TextAlign::End,
        })
    );
    return row;
  }

  // Sizes the meter to the cells that fit `width`; one cell of slack keeps it from ellipsising when the real glyph
  // widths differ from the estimate.
  inline void layoutDottedMeterRow(DottedMeterRow& row, float width, float scale) {
    const float font = kBtopFontSize * scale;
    const float meterWidth =
        width - monoWidth(kMeterTagChars + kMeterValueChars, font) - 2.0F * Style::spaceSm * scale - monoWidth(1, font);
    row.meter.cells = dottedMeterCells(meterWidth, scale);
    refreshDottedMeter(row.meter);
  }

  inline void setDottedMeterRow(DottedMeterRow& row, double fraction, std::string_view value) {
    if (row.value != nullptr && row.value->text() != value) {
      row.value->setText(std::string(value));
    }
    if (row.meter.fraction != fraction) {
      row.meter.fraction = fraction;
      refreshDottedMeter(row.meter);
    }
  }

  // ---- toggle rows -----------------------------------------------------------------------------
  // A clickable btop row: the name on the left, the state on the right. On is a solid accent block with dark
  // text, off is dim text.
  struct ToggleRow {
    Button* button = nullptr;
    Flex* stateBox = nullptr;
    Label* state = nullptr;
    Label* detail = nullptr;
    bool lit = false;
    bool known = false;
  };

  [[nodiscard]] inline Button::ButtonPalette toggleRowPalette() {
    using State = Button::ButtonStateColors;
    const State rest{
        .bg = clearColorSpec(), .border = clearColorSpec(), .label = colorSpecFromRole(ColorRole::OnSurface)
    };
    const State lit{.bg = clearColorSpec(), .border = clearColorSpec(), .label = colorSpecFromRole(ColorRole::Primary)};
    const State off{
        .bg = clearColorSpec(),
        .border = clearColorSpec(),
        .label = colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.5F),
    };
    return {.borderWidth = 0.0F, .normal = rest, .hover = lit, .pressed = lit, .disabled = off, .selected = rest};
  }

  [[nodiscard]] inline std::unique_ptr<Button> toggleRow(
      std::string name, float scale, ToggleRow& out, std::function<void()> onClick,
      std::function<void()> onRightClick = nullptr
  ) {
    auto button = ui::button({
        .out = &out.button,
        .text = std::move(name),
        .fontSize = kBtopFontSize * scale,
        .contentAlign = ButtonContentAlign::Start,
        .variant = ButtonVariant::Ghost,
        .customPalette = toggleRowPalette(),
        .surfaceOpacity = 1.0F,
        .minHeight = kRowHeight * scale,
        .paddingV = 0.0F,
        .paddingH = 0.0F,
        .gap = Style::spaceSm * scale,
        .radius = 0.0F,
        .onClick = std::move(onClick),
        .onRightClick = std::move(onRightClick),
        .configure = [](Button& control) {
          control.setDirection(FlexDirection::Horizontal);
          control.setAlign(FlexAlign::Center);
          if (control.label() != nullptr) {
            control.label()->setFontFamily(kFont);
            control.label()->setMaxLines(1);
            control.label()->setEllipsize(TextEllipsize::End);
            control.label()->setFlexGrow(1.0F);
          }
        },
    });
    button->addChild(
        ui::row(
            {.out = &out.stateBox, .align = FlexAlign::Center, .paddingH = Style::spaceXs * scale, .radius = 0.0F},
            ui::label({
                .out = &out.state,
                .fontSize = kBtopFontSize * scale,
                .fontFamily = std::string(kFont),
                .color = dimColor(),
                .maxLines = 1,
            })
        )
    );
    button->addChild(
        ui::label({
            .out = &out.detail,
            .fontSize = kBtopFontSize * scale,
            .fontFamily = std::string(kFont),
            .color = dimColor(),
            .maxLines = 1,
        })
    );
    return button;
  }

  // `detail` is a short dim note after the state ("· on at 21:00"); empty hides it.
  inline void
  setToggleRow(ToggleRow& row, bool enabled, bool lit, std::string_view text, std::string_view detail = {}) {
    if (row.button == nullptr || row.state == nullptr) {
      return;
    }
    row.button->setEnabled(enabled);
    row.state->setText(text);
    if (row.detail != nullptr) {
      const bool show = !detail.empty();
      row.detail->setText(detail);
      if (row.detail->visible() != show) {
        row.detail->setVisible(show);
        row.detail->setParticipatesInLayout(show);
      }
    }
    if (!row.known || row.lit != lit) {
      row.known = true;
      row.lit = lit;
      if (lit) {
        row.stateBox->setFill(colorSpecFromRole(ColorRole::Primary));
        row.state->setColor(colorSpecFromRole(ColorRole::OnPrimary));
      } else {
        row.stateBox->clearFill();
        row.state->setColor(dimColor());
      }
    }
  }

  // ---- action rows -----------------------------------------------------------------------------
  // A keyword with a status on the right, and an optional wrapped line below it that says what a press will do
  // (while armed) or why the last one failed. Only the keyword takes focus; the rest is text.
  enum class Tone : std::uint8_t { Dim, Normal, Warning, Error };

  [[nodiscard]] inline ColorSpec toneColor(Tone tone) {
    switch (tone) {
    case Tone::Warning:
      return colorSpecFromRole(ColorRole::Warning);
    case Tone::Error:
      return colorSpecFromRole(ColorRole::Error);
    case Tone::Normal:
      return colorSpecFromRole(ColorRole::OnSurface);
    case Tone::Dim:
      break;
    }
    return dimColor();
  }

  inline void setLabelTone(Label* label, std::string_view text, Tone tone) {
    if (label == nullptr) {
      return;
    }
    label->setText(text);
    label->setColor(toneColor(tone));
  }

  struct ActionRow {
    Flex* group = nullptr;
    Flex* row = nullptr;
    Keyword key;
    Label* value = nullptr;
    Label* detail = nullptr;
  };

  [[nodiscard]] inline std::unique_ptr<Flex> actionRow(
      std::string word, std::size_t hot, float scale, ActionRow& out, std::function<void()> onClick,
      std::function<void()> onLeave = nullptr, bool warning = false
  ) {
    auto row = ui::row({.out = &out.row, .align = FlexAlign::Center, .gap = Style::spaceSm * scale});
    row->addChild(makeKeyword(
        out.key, std::move(word), hot, scale, std::move(onClick), std::move(onLeave), kActionKeyPaddingH,
        kActionKeyRestAlpha, warning
    ));
    row->addChild(
        ui::label({
            .out = &out.value,
            .fontSize = kBtopFontSize * scale,
            .fontFamily = std::string(kFont),
            .color = dimColor(),
            .maxLines = 1,
            .textAlign = TextAlign::End,
            .flexGrow = 1.0F,
        })
    );
    auto group = ui::column({.out = &out.group, .align = FlexAlign::Stretch, .gap = 0.0F});
    group->addChild(std::move(row));
    group->addChild(
        ui::label({
            .out = &out.detail,
            .fontSize = kBtopFontSize * scale,
            .fontFamily = std::string(kFont),
            .color = dimColor(),
            .maxLines = 3,
            .visible = false,
            .participatesInLayout = false,
        })
    );
    return group;
  }

  inline void setActionValue(ActionRow& row, std::string_view text, Tone tone) { setLabelTone(row.value, text, tone); }

  // Returns true when the line appeared or went away, so the caller knows the layout changed.
  inline bool setActionDetail(ActionRow& row, std::string_view text, Tone tone) {
    if (row.detail == nullptr) {
      return false;
    }
    const bool show = !text.empty();
    const bool changed = row.detail->visible() != show;
    if (show) {
      setLabelTone(row.detail, text, tone);
    }
    if (changed) {
      row.detail->setVisible(show);
      row.detail->setParticipatesInLayout(show);
    }
    return changed;
  }

} // namespace term_frame
