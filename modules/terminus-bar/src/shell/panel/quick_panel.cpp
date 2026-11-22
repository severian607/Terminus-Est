#include "shell/panel/quick_panel.h"

#include "core/deferred_call.h"
#include "render/core/renderer.h"
#include "shell/control_center/term_frame.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/style.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  // Keeps rounded-rect AA inside the clipped body, as the control center's tab viewport does.
  constexpr float kBodyClipInset = 1.0F;
  // One caption line of the title row and of the footer.
  constexpr float kCaptionLine = 18.0F;
  // Everything around the tab body, in logical px: the panel decoration's padding, the frame's padding and
  // border, the title row, the footer, and the two gaps between the frame's three children.
  constexpr float kChromeHeight = 2.0F * Style::panelPadding
      + 2.0F * (Style::spaceSm + Style::borderWidth)
      + 2.0F * kCaptionLine
      + 2.0F * Style::spaceXs
      + kBodyClipInset;
  constexpr float kChromeWidth = 2.0F * Style::panelPadding + 2.0F * (Style::spaceSm + Style::borderWidth);

} // namespace

QuickPanel::QuickPanel(
    std::string panelId, std::string title, std::unique_ptr<Tab> tab, float width, float height, std::string fullContext
)
    : m_panelId(std::move(panelId)), m_title(std::move(title)), m_tab(std::move(tab)), m_width(width), m_height(height),
      m_fullContext(std::move(fullContext)) {
  m_tab->setQuickPanelResizeRequest([this]() { onTabResizeRequest(); });
}

float QuickPanel::bodyWidth() const { return std::max(1.0F, m_width - kChromeWidth); }

float QuickPanel::bodyHeightFor(float panelHeight) const { return std::max(1.0F, panelHeight - kChromeHeight); }

float QuickPanel::preferredHeight() const {
  const float body = m_tab->quickPanelHeight(bodyWidth(), bodyHeightFor(m_height));
  return scaled(std::max(1.0F, body) + kChromeHeight);
}

void QuickPanel::onTabResizeRequest() {
  if (m_frame == nullptr) {
    return;
  }
  const float wanted = m_tab->quickPanelHeight(bodyWidth(), bodyHeightFor(m_height));
  if (std::abs(wanted - m_openedBodyHeight) < 0.5F) {
    return;
  }
  PanelManager::instance().reopenActivePanel(m_panelId);
}

std::string QuickPanel::footerText() const {
  std::string text = m_tab->quickPanelHints();
  const auto append = [&text](std::string_view hint) {
    if (!text.empty()) {
      text += "  ";
    }
    text += hint;
  };
  if (!m_fullContext.empty()) {
    append("f full");
  }
  append("Esc close");
  return text;
}

void QuickPanel::create() {
  const float scale = contentScale();

  m_tab->setContentScale(scale);
  m_tab->setInputDispatcher(inputDispatcher());
  m_tab->setPanelCardOpacity(panelCardOpacity());
  m_openedBodyHeight = m_tab->quickPanelHeight(bodyWidth(), bodyHeightFor(m_height));

  auto frame = term_frame::frame(scale, Style::spaceSm * scale, &m_frame);
  frame->setClipChildren(true);
  frame->addChild(term_frame::titleRow("", "[ " + m_title + " ]", scale, &m_chip));
  m_lastStatus.clear();

  auto body = ui::column({
      .out = &m_body,
      .align = FlexAlign::Stretch,
      .gap = 0.0F,
      .clipChildren = true,
      .flexGrow = 1.0F,
  });
  auto container = m_tab->create();
  container->setFlexGrow(1.0F);
  container->setParticipatesInLayout(false);
  m_tabContainer = container.get();
  body->addChild(std::move(container));
  frame->addChild(std::move(body));

  m_lastFooter = footerText();
  frame->addChild(
      ui::label({
          .out = &m_footer,
          .text = m_lastFooter,
          .fontSize = Style::fontSizeCaption * scale,
          .fontFamily = std::string(term_frame::kFont),
          .color = term_frame::dimColor(),
          .maxLines = 1,
      })
  );

  setRoot(std::move(frame));
  if (m_animations != nullptr) {
    root()->setAnimationManager(m_animations);
  }
}

void QuickPanel::onPanelCardOpacityChanged(float opacity) { m_tab->setPanelCardOpacity(opacity); }

void QuickPanel::doLayout(Renderer& renderer, float width, float height) {
  if (m_frame == nullptr || m_body == nullptr || m_tabContainer == nullptr) {
    return;
  }

  m_frame->setSize(width, height);
  m_frame->layout(renderer);

  const float bodyWidth = m_body->width();
  const float bodyHeight = std::max(1.0F, std::floor(m_body->height() - kBodyClipInset));
  m_tabContainer->setPosition(0.0F, 0.0F);
  m_tabContainer->setSize(bodyWidth, bodyHeight);
  m_tab->layout(renderer, bodyWidth, bodyHeight);
}

void QuickPanel::doUpdate(Renderer& renderer) {
  m_tab->update(renderer);
  if (m_chip != nullptr) {
    std::string status = m_tab->quickPanelStatus();
    if (status != m_lastStatus) {
      term_frame::setChip(*m_chip, status);
      m_lastStatus = std::move(status);
    }
  }
  if (m_footer != nullptr) {
    std::string footer = footerText();
    if (footer != m_lastFooter) {
      m_footer->setText(footer);
      m_lastFooter = std::move(footer);
    }
  }
}

void QuickPanel::onFrameTick(float deltaMs) { m_tab->onFrameTick(deltaMs); }

void QuickPanel::onOpen(std::string_view /*context*/) { m_tab->setActive(true); }

void QuickPanel::onClose() {
  m_tab->setActive(false);
  m_tab->onClose();
  m_frame = nullptr;
  m_body = nullptr;
  m_tabContainer = nullptr;
  m_chip = nullptr;
  m_footer = nullptr;
  clearReleasedRoot();
}

bool QuickPanel::dismissTransientUi() { return m_tab->dismissTransientUi(); }

bool QuickPanel::handleGlobalKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (preedit || sym == XKB_KEY_Escape || sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab) {
    return false;
  }
  if (!m_fullContext.empty() && sym == XKB_KEY_f && modifiers == 0) {
    if (pressed) {
      DeferredCall::callLater([context = m_fullContext]() {
        PanelOpenRequest request;
        request.context = context;
        PanelManager::instance().openPanel("control-center", request);
      });
    }
    return true;
  }
  return m_tab->handleQuickPanelKey(sym, modifiers, pressed, preedit);
}
