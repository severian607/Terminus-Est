#pragma once

#include "core/ui_phase.h"
#include "ui/palette.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class Flex;
class InputDispatcher;
class Label;
class Renderer;

namespace control_center {

  void applySectionCardStyle(Flex& card, float scale = 1.0F, float fillOpacity = 1.0F);
  Label* addTitle(Flex& parent, const std::string& text, float scale = 1.0F);
  void addBody(Flex& parent, const std::string& text, float scale = 1.0F);

  // Card header row: a card-title-styled label (flexGrow) ready for trailing
  // controls appended by the caller. Used for titled group cards.
  std::unique_ptr<Flex> makeCardHeaderRow(
      const std::string& title, float scale = 1.0F, const ColorSpec& color = colorSpecFromRole(ColorRole::OnSurface)
  );

} // namespace control_center

class Tab {
public:
  virtual ~Tab() = default;

  // Creates and returns the tab's root Flex container.
  // The returned node is owned by the caller (added to m_tabBodies).
  // Implementations may cache a raw pointer to it for later use.
  virtual std::unique_ptr<Flex> create() = 0;

  // Optional trailing header actions shown in the shared control-center header
  // while this tab is active.
  virtual std::unique_ptr<Flex> createHeaderActions();

  // Called by ControlCenterPanel::layout() with the available content dimensions.
  void layout(Renderer& renderer, float contentWidth, float bodyHeight) {
    UiPhaseScope layoutPhase(UiPhase::Layout);
    doLayout(renderer, contentWidth, bodyHeight);
  }

  // Called by ControlCenterPanel::update() every frame.
  void update(Renderer& renderer) {
    UiPhaseScope updatePhase(UiPhase::Update);
    doUpdate(renderer);
  }

  // Called every Wayland frame callback with elapsed milliseconds. Default is a no-op.
  // Tabs override this to advance per-frame animations independently of data arrival.
  virtual void onFrameTick(float deltaMs) { (void)deltaMs; }

  // Called when the tab becomes visible or hidden.
  virtual void setActive(bool active) { (void)active; }

  // Called when the panel closes. Null out all raw pointers to freed nodes.
  virtual void onClose() {}

  // Allows a tab to close transient UI (menus/popovers) when external empty
  // chrome areas are clicked.
  virtual bool dismissTransientUi() { return false; }

  // ---- Hosting in a QuickPanel (a small bar-attached panel) ----
  // Height in logical px (unscaled) the tab wants inside a QuickPanel whose body is `width` logical px wide;
  // `fallback` is the registered height. Read when the panel opens and after requestQuickPanelResize().
  [[nodiscard]] virtual float quickPanelHeight(float width, float fallback) const {
    (void)width;
    return fallback;
  }
  // The tab's own key hints for the footer, `key label` pairs two spaces apart ("←→ seek  o player"); the host
  // appends its own keys in the same style.
  [[nodiscard]] virtual std::string quickPanelHints() const { return {}; }
  // Live status set into the title rule as a dim `[ status ]` chip; empty hides it. Polled on every update.
  [[nodiscard]] virtual std::string quickPanelStatus() const { return {}; }
  // Keys while hosted in a QuickPanel (xkb keysym, modifiers as in Panel::handleGlobalKey). Return true when
  // consumed. Escape and the host's own keys never reach the tab.
  [[nodiscard]] virtual bool handleQuickPanelKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
    (void)sym;
    (void)modifiers;
    (void)pressed;
    (void)preedit;
    return false;
  }
  void setQuickPanelResizeRequest(std::function<void()> request) { m_quickPanelResizeRequest = std::move(request); }

  void setInputDispatcher(InputDispatcher* dispatcher) noexcept { m_inputDispatcher = dispatcher; }
  void setContentScale(float scale) noexcept { m_contentScale = scale; }
  void setPanelCardOpacity(float opacity) noexcept {
    const float clamped = std::clamp(opacity, 0.0F, 1.0F);
    if (m_panelCardOpacity == clamped) {
      return;
    }
    m_panelCardOpacity = clamped;
    onPanelCardOpacityChanged(clamped);
  }

protected:
  [[nodiscard]] InputDispatcher* inputDispatcher() const noexcept { return m_inputDispatcher; }
  [[nodiscard]] float contentScale() const noexcept { return m_contentScale; }
  [[nodiscard]] float panelCardOpacity() const noexcept { return m_panelCardOpacity; }
  // Asks the hosting QuickPanel to re-read quickPanelHeight(); it reopens at the new size when that changed.
  void requestQuickPanelResize() const {
    if (m_quickPanelResizeRequest) {
      m_quickPanelResizeRequest();
    }
  }
  [[nodiscard]] float scaled(float value) const noexcept { return value * m_contentScale; }
  virtual void onPanelCardOpacityChanged(float opacity) { (void)opacity; }
  virtual void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
    (void)renderer;
    (void)contentWidth;
    (void)bodyHeight;
  }
  virtual void doUpdate(Renderer& renderer) { (void)renderer; }

private:
  float m_contentScale = 1.0F;
  float m_panelCardOpacity = 1.0F;
  InputDispatcher* m_inputDispatcher = nullptr;
  std::function<void()> m_quickPanelResizeRequest;
};
