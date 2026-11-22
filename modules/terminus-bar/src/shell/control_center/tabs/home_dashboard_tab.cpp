#include "shell/control_center/tabs/home_dashboard_tab.h"

#include "core/input/key_symbols.h"
#include "core/input/keybind_matcher.h"
#include "render/scene/input_dispatcher.h"
#include "shell/control_center/control_center_services.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/scroll_view.h"
#include "ui/scroll_into_view.h"
#include "ui/split_pane_focus.h"
#include "ui/style.h"

#include <algorithm>
#include <utility>

namespace {

  constexpr float kDashboardControlsWidth = 640.0F;
  constexpr float kDashboardBtopGrow = 4.0F;
  // The accent line under the focused tile spans the tile (minus a one-cell inset each side), 2 px thick.
  constexpr float kDashboardFocusThickness = 2.0F;

  std::unique_ptr<Flex> dashboardSection(float flexGrow, std::unique_ptr<Flex> content) {
    content->setFillHeight(true);
    content->setFlexGrow(flexGrow);
    return content;
  }

} // namespace

HomeDashboardTab::HomeDashboardTab(const ControlCenterServices& services) : m_home(services) {}

HomeDashboardTab::~HomeDashboardTab() = default;

void HomeDashboardTab::syncChildPresentation() {
  for (Tab* tab : {static_cast<Tab*>(&m_home), static_cast<Tab*>(&m_btop)}) {
    tab->setInputDispatcher(inputDispatcher());
    tab->setContentScale(contentScale());
    tab->setPanelCardOpacity(panelCardOpacity());
  }
}

std::unique_ptr<Flex> HomeDashboardTab::create() {
  syncChildPresentation();
  const float scale = contentScale();

  auto scroll = ui::scrollView({
      .out = &m_scroll,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .configure = [](ScrollView& view) {
        view.clearFill();
        view.clearBorder();
        view.content()->setAlign(FlexAlign::Stretch);
      },
  });

  auto dashboard = ui::row({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceMd * scale,
  });
  // The controls column is the home tiles (three rows of two, then the system strip), which fill it.
  auto controls = ui::column({
      .out = &m_controlsRoot,
      .align = FlexAlign::Stretch,
      .gap = Style::spaceXs * scale,
      .fillHeight = true,
  });

  auto home = m_home.create();
  home->setFlexGrow(1.0F);
  controls->addChild(std::move(home));

  auto btop = m_btop.create();
  m_btopRoot = btop.get();
  m_btopRoot->setFill(colorSpecFromRole(ColorRole::Surface, panelCardOpacity()));
  dashboard->addChild(dashboardSection(kDashboardBtopGrow, std::move(btop)));

  dashboard->insertChildAt(0, std::move(controls));

  dashboard->addChild(
      ui::box({
          .out = &m_focusIndicator,
          .fill = colorSpecFromRole(ColorRole::Primary),
          .radius = 0.0F,
          .width = 76.0F * scale,
          .height = Style::borderWidth * 1.5F * scale,
          .visible = false,
          .participatesInLayout = false,
          .configure = [](Box& line) {
            line.setHitTestVisible(false);
            line.setZIndex(10);
          },
      })
  );

  scroll->content()->addChild(std::move(dashboard));
  return scroll;
}

std::unique_ptr<Flex> HomeDashboardTab::createHeaderActions() { return m_home.createHeaderActions(); }

void HomeDashboardTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_root == nullptr || m_scroll == nullptr || m_controlsRoot == nullptr) {
    return;
  }

  const float scale = contentScale();
  syncChildPresentation();
  const bool compact = usesCompactLayout(contentWidth, scale);
  if (compact != m_compactLayout) {
    m_scroll->setScrollOffset(0.0F);
    m_lastRevealedFocus = nullptr;
    m_compactLayout = compact;
  }
  const float gap = m_root->gap();
  const float terminalHeight = compact ? kCompactBtopHeight * scale : bodyHeight;
  m_root->setDirection(compact ? FlexDirection::Vertical : FlexDirection::Horizontal);
  m_scroll->setScrollbarVisible(compact);
  m_scroll->setSize(contentWidth, bodyHeight);
  m_controlsRoot->setFillHeight(!compact);
  m_btopRoot->setFillHeight(!compact);

  const float controlsWidth = compact ? contentWidth : kDashboardControlsWidth * scale;
  m_controlsRoot->setFlexGrow(0.0F);
  m_controlsRoot->setMinWidth(controlsWidth);
  m_controlsRoot->setMaxWidth(controlsWidth);
  m_btopRoot->setFlexGrow(compact ? 0.0F : kDashboardBtopGrow);

  // The home tiles take the height their rows need: at least the panel height in wide mode, a fixed taller height in
  // compact mode. When they need more than the panel has, the page scrolls.
  const float controlsHint = compact ? kCompactControlsHeight * scale : bodyHeight;
  m_home.layout(renderer, controlsWidth, controlsHint);
  const float controlsHeight = std::max(controlsHint, m_home.naturalHeight());
  m_controlsRoot->setSize(controlsWidth, controlsHeight);
  m_scroll->setScrollbarVisible(compact || controlsHeight > bodyHeight + 0.5F);

  m_btopRoot->setSize(contentWidth, terminalHeight);
  m_root->setSize(contentWidth, compact ? controlsHeight + terminalHeight + gap : std::max(bodyHeight, controlsHeight));
  m_scroll->layout(renderer);
  m_btop.layout(renderer, m_btopRoot->width(), terminalHeight);

  syncFocusIndicator();
}

void HomeDashboardTab::doUpdate(Renderer& renderer) {
  m_home.update(renderer);
  m_btop.update(renderer);
  syncFocusIndicator();
}

void HomeDashboardTab::onFrameTick(float deltaMs) {
  m_home.onFrameTick(deltaMs);
  m_btop.onFrameTick(deltaMs);
}

void HomeDashboardTab::setActive(bool active) {
  m_home.setActive(active);
  m_btop.setActive(active);
}

void HomeDashboardTab::onClose() {
  m_home.onClose();
  m_btop.onClose();
  m_root = nullptr;
  m_scroll = nullptr;
  m_controlsRoot = nullptr;
  m_compactLayout = false;
  m_lastRevealedFocus = nullptr;
  m_btopRoot = nullptr;
  m_focusIndicator = nullptr;
  m_focusedSection = kSectionCount;
}

bool HomeDashboardTab::dismissTransientUi() {
  const bool homeDismissed = m_home.dismissTransientUi();
  const bool btopDismissed = m_btop.dismissTransientUi();
  return homeDismissed || btopDismissed;
}

void HomeDashboardTab::onPanelCardOpacityChanged(float /*opacity*/) {
  syncChildPresentation();
  if (m_btopRoot != nullptr) {
    m_btopRoot->setFill(colorSpecFromRole(ColorRole::Surface, panelCardOpacity()));
  }
}

bool HomeDashboardTab::btopOwnsKeyboardFocus() const noexcept { return m_btop.ownsKeyboardFocus(); }

bool HomeDashboardTab::handleBtopKey(
    std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers, bool pressed, bool preedit
) {
  return m_btop.handleKey(sym, utf32, modifiers, pressed, preedit);
}

Flex* HomeDashboardTab::sectionRoot(std::size_t index) const noexcept {
  switch (index) {
  case 0:
    return m_home.hostBox();
  case 1:
    return m_home.togglesBox();
  case 2:
    return m_home.upkeepBox();
  case 3:
    return m_home.todayBox();
  case 4:
    return m_home.inboxBox();
  case 5:
    return m_home.fleetBox();
  case 6:
    return m_home.systemBox();
  case 7:
    return m_btopRoot;
  default:
    return nullptr;
  }
}

std::size_t HomeDashboardTab::focusedSectionIndex(const InputDispatcher& dispatcher) const noexcept {
  InputArea* const focused = dispatcher.focusedArea();
  if (focused == nullptr) {
    return kSectionCount;
  }

  for (std::size_t index = 0; index < kSectionCount; ++index) {
    if (isNodeInSubtree(focused, sectionRoot(index))) {
      return index;
    }
  }
  return kSectionCount;
}

void HomeDashboardTab::syncFocusIndicator() {
  if (m_focusIndicator == nullptr || m_root == nullptr) {
    return;
  }

  const InputDispatcher* const dispatcher = inputDispatcher();
  const std::size_t focused = dispatcher != nullptr ? focusedSectionIndex(*dispatcher) : kSectionCount;
  revealFocusedSection();
  if (focused < kSectionCount) {
    m_focusedSection = focused;
  }

  Flex* const section = m_focusedSection < kSectionCount ? sectionRoot(m_focusedSection) : nullptr;
  if (section == nullptr) {
    m_focusIndicator->setVisible(false);
    return;
  }

  m_focusIndicator->setVisible(true);
  const float indicatorWidth = std::max(1.0F, section->width() - 2.0F * Style::spaceXs * contentScale());
  m_focusIndicator->setSize(indicatorWidth, kDashboardFocusThickness * contentScale());
  float sectionX = 0.0F;
  float sectionY = 0.0F;
  float rootX = 0.0F;
  float rootY = 0.0F;
  Node::absolutePosition(section, sectionX, sectionY);
  Node::absolutePosition(m_root, rootX, rootY);
  m_focusIndicator->setPosition(
      sectionX - rootX + (section->width() - m_focusIndicator->width()) * 0.5F,
      sectionY - rootY + section->height() - m_focusIndicator->height()
  );
}

void HomeDashboardTab::revealFocusedSection() {
  if (!m_compactLayout || m_scroll == nullptr || inputDispatcher() == nullptr) {
    return;
  }
  InputArea* const focused = inputDispatcher()->focusedArea();
  if (focused == m_lastRevealedFocus) {
    return;
  }
  m_lastRevealedFocus = focused;
  const std::size_t index = focusedSectionIndex(*inputDispatcher());
  Flex* const section = sectionRoot(index);
  if (section == nullptr) {
    return;
  }

  // Reveal the whole section when possible; on short surfaces follow the
  // focused control within it instead of jumping back to the section's top.
  const Node& target = section->height() > m_scroll->contentViewportHeight() && focused != nullptr
      ? static_cast<const Node&>(*focused)
      : static_cast<const Node&>(*section);
  scrollNodeIntoScrollView(*m_scroll, nullptr, target, 0.0F);
}

bool HomeDashboardTab::handleSectionPaging(
    InputDispatcher& dispatcher, std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit
) {
  const bool backwards = KeySymbol::isPageUp(sym);
  if (preedit || modifiers != 0U || (!backwards && !KeySymbol::isPageDown(sym))) {
    return false;
  }
  if (dispatcher.focusedArea() != nullptr && dispatcher.focusedArea()->textInputClient() != nullptr) {
    return false;
  }
  if (!pressed) {
    return true;
  }

  const std::size_t focused = focusedSectionIndex(dispatcher);
  const std::size_t current = focused < kSectionCount ? focused : m_focusedSection;
  std::size_t target = current >= kSectionCount ? (backwards ? kSectionCount - 1 : 0)
                                                : (current + (backwards ? kSectionCount - 1 : 1)) % kSectionCount;

  for (std::size_t attempt = 0; attempt < kSectionCount; ++attempt) {
    if (auto* focus = dispatcher.firstTabFocusUnder(sectionRoot(target)); focus != nullptr) {
      dispatcher.setFocus(focus);
      m_focusedSection = target;
      syncFocusIndicator();
      return true;
    }
    target = (target + (backwards ? kSectionCount - 1 : 1)) % kSectionCount;
  }
  return true;
}

bool HomeDashboardTab::handleHotkey(
    InputDispatcher& dispatcher, std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit
) {
  if (preedit) {
    return false;
  }
  InputArea* const focused = dispatcher.focusedArea();
  if (focused != nullptr && focused->textInputClient() != nullptr) {
    return false;
  }
  // The tile numbers follow the grid, row by row: 1 host, 2 toggles, 3 upkeep, 4 today, 5 inbox, 6 fleet, 7 system;
  // 8 the task manager. Each jumps the keyboard focus into that tile (sectionRoot() order).
  if (modifiers == 0U && sym >= '1' && sym <= '8') {
    if (pressed) {
      const std::size_t section = sym - '1';
      InputArea* const target =
          section == kSectionCount - 1 ? m_btop.focusArea() : dispatcher.firstTabFocusUnder(sectionRoot(section));
      if (target != nullptr) {
        dispatcher.setFocus(target);
        m_focusedSection = section;
        syncFocusIndicator();
      }
    }
    return true;
  }
  return m_home.handleHotkey(sym, modifiers, pressed);
}

bool HomeDashboardTab::handleArrowNavigation(
    InputDispatcher& dispatcher, std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit
) {
  if (!pressed || preedit) {
    return false;
  }
  InputArea* const focused = dispatcher.focusedArea();
  if (focused != nullptr && focused->textInputClient() != nullptr) {
    return false;
  }
  // Tile sections have no arrow handling of their own: Left/Right walk the rows of the sections with side-by-side
  // keys (today's timer line, the system strip) like Up/Down.
  for (Flex* const tileSection : {m_home.todayBox(), m_home.systemBox()}) {
    if (tileSection == nullptr || focused == nullptr || !isNodeInSubtree(focused, tileSection)) {
      continue;
    }
    if (KeybindMatcher::matches(KeybindAction::Left, sym, modifiers)) {
      return dispatcher.cycleTabFocusInSubtree(tileSection, true);
    }
    if (KeybindMatcher::matches(KeybindAction::Right, sym, modifiers)) {
      return dispatcher.cycleTabFocusInSubtree(tileSection, false);
    }
  }
  for (std::size_t section = 0; section < kSectionCount - 1; ++section) { // the home tiles, not the task manager
    Flex* const verticalSection = sectionRoot(section);
    if (verticalSection == nullptr || focused == nullptr || !isNodeInSubtree(focused, verticalSection)) {
      continue;
    }
    if (KeybindMatcher::matches(KeybindAction::Up, sym, modifiers)) {
      return dispatcher.cycleTabFocusInSubtree(verticalSection, true);
    }
    if (KeybindMatcher::matches(KeybindAction::Down, sym, modifiers)) {
      return dispatcher.cycleTabFocusInSubtree(verticalSection, false);
    }
  }
  return false;
}

void HomeDashboardTab::noteKey(
    InputDispatcher& dispatcher, std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit
) {
  if (!preedit) {
    m_home.noteKey(sym, modifiers, pressed, dispatcher.focusedArea());
  }
}