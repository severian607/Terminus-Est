#pragma once

#include "shell/control_center/tab.h"
#include "shell/control_center/tabs/btop_tab.h"
#include "shell/control_center/tabs/home_tab.h"

#include <cstddef>
#include <cstdint>
#include <memory>

struct ControlCenterServices;

class InputDispatcher;
class Flex;
class Renderer;
class Box;
class Button;

class ScrollView;
class HomeDashboardTab final : public Tab {
public:
  explicit HomeDashboardTab(const ControlCenterServices& services);
  ~HomeDashboardTab() override;

  [[nodiscard]] static constexpr bool usesCompactLayout(float width, float scale) noexcept {
    return width < 1500.0F * scale;
  }
  static constexpr float kCompactControlsHeight = 740.0F;
  static constexpr float kCompactBtopHeight = 600.0F;
  static constexpr float kCompactPanelHeight = 1440.0F;

  std::unique_ptr<Flex> create() override;
  std::unique_ptr<Flex> createHeaderActions() override;
  void onFrameTick(float deltaMs) override;
  void setActive(bool active) override;
  void onClose() override;
  void preloadTaskManager() { m_btop.preload(); }
  bool dismissTransientUi() override;
  [[nodiscard]] bool btopOwnsKeyboardFocus() const noexcept;
  bool handleBtopKey(std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers, bool pressed, bool preedit);
  bool handleSectionPaging(
      InputDispatcher& dispatcher, std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit
  );
  bool handleArrowNavigation(
      InputDispatcher& dispatcher, std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit
  );
  bool
  handleHotkey(InputDispatcher& dispatcher, std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit);
  // Called for every key on the dashboard: anything but the confirming press disarms an armed action.
  void noteKey(InputDispatcher& dispatcher, std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit);

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;
  void onPanelCardOpacityChanged(float opacity) override;
  void syncChildPresentation();
  [[nodiscard]] Flex* sectionRoot(std::size_t index) const noexcept;
  [[nodiscard]] std::size_t focusedSectionIndex(const InputDispatcher& dispatcher) const noexcept;
  void syncFocusIndicator();
  void revealFocusedSection();

  // host, toggles, upkeep, today, inbox, fleet, system tiles, then btop.
  static constexpr std::size_t kSectionCount = 8;

  HomeTab m_home;
  BtopTab m_btop;

  Flex* m_root = nullptr;
  ScrollView* m_scroll = nullptr;
  Flex* m_controlsRoot = nullptr;
  bool m_compactLayout = false;
  InputArea* m_lastRevealedFocus = nullptr;
  Flex* m_btopRoot = nullptr;
  Box* m_focusIndicator = nullptr;
  std::size_t m_focusedSection = kSectionCount;
};
