#pragma once

#include "shell/control_center/tab.h"
#include "shell/panel/panel.h"

#include <memory>
#include <string>

class Flex;
class Label;
class Renderer;

// A small panel that drops from the bar and hosts exactly one control-center Tab inside a
// terminal-look frame: `[ title ]` header with the tab's live `[ status ]` chip, the tab body, and a
// muted key-hint footer (the tab's hints, then `f full` when a full view exists, then `Esc close`).
// The height comes from the tab (Tab::quickPanelHeight) when the panel opens; a tab whose content
// changes shape asks for a resize and the panel reopens in place at the new height.
class QuickPanel final : public Panel {
public:
  // `fullContext` names the control-center tab key `f` opens ("calendar", "media"); empty disables it.
  QuickPanel(
      std::string panelId, std::string title, std::unique_ptr<Tab> tab, float width, float height,
      std::string fullContext = {}
  );

  void create() override;
  void onFrameTick(float deltaMs) override;
  void onOpen(std::string_view context) override;
  void onClose() override;
  [[nodiscard]] bool dismissTransientUi() override;
  [[nodiscard]] bool handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

  [[nodiscard]] float preferredWidth() const override { return scaled(m_width); }
  [[nodiscard]] float preferredHeight() const override;
  [[nodiscard]] PanelPlacement panelPlacement() const noexcept override { return PanelPlacement::Attached; }
  // Anchored under the clicked bar widget; opened without an anchor it sits at the bar's right end.
  [[nodiscard]] bool panelOpenNearClick() const override { return true; }

private:
  void onPanelCardOpacityChanged(float opacity) override;
  void doLayout(Renderer& renderer, float width, float height) override;
  void doUpdate(Renderer& renderer) override;
  [[nodiscard]] float bodyWidth() const;
  [[nodiscard]] float bodyHeightFor(float panelHeight) const;
  [[nodiscard]] std::string footerText() const;
  void onTabResizeRequest();

  std::string m_panelId;
  std::string m_title;
  std::unique_ptr<Tab> m_tab;
  float m_width;
  float m_height;
  std::string m_fullContext;
  // Body height (logical px) the panel was opened with; a resize request reopens only when it differs.
  float m_openedBodyHeight = 0.0F;

  Flex* m_frame = nullptr;
  Flex* m_body = nullptr;
  Flex* m_tabContainer = nullptr;
  Label* m_chip = nullptr;
  Label* m_footer = nullptr;
  std::string m_lastStatus;
  std::string m_lastFooter;
};
