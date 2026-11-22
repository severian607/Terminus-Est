#include "shell/bar/widgets/media_widget.h"

#include "core/log.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "i18n/i18n.h"
#include "net/http_client.h"
#include "render/core/renderer.h"
#include "render/scene/input_area.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <wayland-client-protocol.h>

using namespace mpris;

namespace {

  const Logger kLog{"media"};

} // namespace

MediaWidget::MediaWidget(MprisService* mpris, HttpClient* httpClient, wl_output* /*output*/, Options options)
    : m_mpris(mpris), m_httpClient(httpClient), m_maxWidth(static_cast<float>(options.maxWidth)),
      m_minWidth(static_cast<float>(options.minWidth)), m_artSize(static_cast<float>(options.artSize)),
      m_titleScrollMode(options.titleScrollMode), m_hideWhenNoMedia(options.hideWhenNoMedia),
      m_hideWhenIdle(options.hideWhenIdle), m_rotateAlbumArt(options.rotateAlbumArt),
      m_albumArtOnly(options.albumArtOnly), m_hideAlbumArt(options.hideAlbumArt), m_hideArtist(options.hideArtist),
      m_artistFirst(options.artistFirst), m_stacked(options.stacked), m_showControls(options.showControls),
      m_showProgress(options.showProgress) {}

void MediaWidget::create() {
  auto area = ui::inputArea({});
  area->setClipChildren(true);
  area->setOnEnter([this](const InputArea::PointerData&) {
    applyTitleScrollMode(m_label != nullptr && m_label->visible());
    this->requestUpdate();
  });
  area->setOnLeave([this]() {
    applyTitleScrollMode(m_label != nullptr && m_label->visible());
    this->requestUpdate();
  });
  m_area = area.get();

  area->addChild(
      ui::progressBar(
          {.out = &m_progressBar,
           .fill = scaleAlpha(widgetForegroundOr(colorSpecFromRole(ColorRole::Primary)), 0.25F),
           .track = clearColorSpec(),
           .visible = false,
           .participatesInLayout = false,
           .configure = [](ProgressBar& bar) {
             bar.setZIndex(-1);
             bar.setHitTestVisible(false);
             bar.setProgress(0.0F);
           }}
      )
  );
  area->addChild(
      ui::image({
          .out = &m_art,
          .fit = ImageFit::Cover,
          .radius = (m_artSize * m_contentScale) * 0.5F,
          .width = m_artSize * m_contentScale,
          .height = m_artSize * m_contentScale,
      })
  );

  area->addChild(
      ui::label({
          .out = &m_label,
          .fontSize = Style::fontSizeBody * fontScale(),
          .fontWeight = labelFontWeight(),
          .fontFamily = labelFontFamily(),
          .color = widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface)),
          .maxWidth = m_maxWidth * m_contentScale,
          .maxLines = 1,
          .autoScroll = false,
      })
  );

  if (m_stacked) {
    area->addChild(
        ui::label({
            .out = &m_secondaryLabel,
            .fontSize = Style::fontSizeBody * 0.85F * fontScale(),
            .fontWeight = labelFontWeight(),
            .fontFamily = labelFontFamily(),
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxWidth = m_maxWidth * m_contentScale,
            .maxLines = 1,
            .autoScroll = false,
        })
    );
  }

  if (m_showControls && !m_albumArtOnly) {
    constexpr std::array<const char*, 3> glyphs{"media-prev", "media-play", "media-next"};
    for (std::size_t i = 0; i < m_controls.size(); ++i) {
      auto control = ui::inputArea({
          .out = &m_controls[i],
          .acceptedButtons = InputArea::buttonMask({BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA}),
          .propagateEvents = false,
          .visible = false,
      });
      control->setAcceptedScrollDirections(0);
      control->setOnClick([this, i](const InputArea::PointerData& event) {
        if (event.button != BTN_LEFT || m_mpris == nullptr || !m_controlAvailable[i]) {
          return;
        }
        if (i == 0) {
          (void)m_mpris->previous(m_lastBusName);
        } else if (i == 1) {
          (void)m_mpris->playPause(m_lastBusName);
        } else {
          (void)m_mpris->next(m_lastBusName);
        }
      });
      control->setOnEnter([this](const InputArea::PointerData&) {
        applyTitleScrollMode(m_label->visible());
        requestRedraw();
      });
      control->setOnLeave([this] {
        applyTitleScrollMode(m_label->visible());
        requestRedraw();
      });
      control->addChild(
          ui::glyph({
              .out = &m_controlGlyphs[i],
              .glyph = glyphs[i],
              .glyphSize = Style::baseGlyphSize * m_contentScale,
              .color = widgetIconColorOr(colorSpecFromRole(ColorRole::OnSurface)),
          })
      );
      area->addChild(std::move(control));
    }
  }

  area->addChild(
      ui::glyph({
          .out = &m_emptyGlyph,
          .glyph = "music-off",
          .glyphSize = Style::baseGlyphSize * m_contentScale,
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
          .visible = false,
      })
  );

  setRoot(std::move(area));
}

void MediaWidget::doLayout(Renderer& renderer, float containerWidth, float containerHeight) {
  auto* rootNode = root();
  if (rootNode == nullptr
      || m_art == nullptr
      || m_label == nullptr
      || m_emptyGlyph == nullptr
      || m_progressBar == nullptr) {
    return;
  }
  const bool wasVertical = m_isVertical;
  m_isVertical = containerHeight > containerWidth;
  const bool isVertical = m_isVertical;
  const auto active = activePlayer();
  syncState(renderer, active);

  const bool artOnly = isVertical || m_albumArtOnly;
  const float crossLength = std::max(0.0F, isVertical ? containerWidth : containerHeight);
  const float maxLength = std::max(0.0F, std::min(m_maxWidth * m_contentScale, containerWidth));
  const float minLength = std::clamp(m_minWidth * m_contentScale, 0.0F, maxLength);
  const bool showProgressFill = progressFillEligible(active);
  const float gap = Style::spaceXs * (m_stacked || m_showControls ? m_contentScale : 1.0F);

  m_label->setColor(
      m_lastPlaybackStatus == "Playing" ? widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface))
                                        : colorSpecFromRole(ColorRole::OnSurfaceVariant)
  );
  m_label->measure(renderer);
  if (m_secondaryLabel != nullptr) {
    m_secondaryLabel->measure(renderer);
  }
  const float lineGap = 2.0F * m_contentScale;
  const bool showSecondary = !artOnly
      && m_secondaryLabel != nullptr
      && !m_secondaryLabel->text().empty()
      && m_label->height() + lineGap + m_secondaryLabel->height() <= crossLength;
  m_emptyGlyph->setGlyph(active.has_value() ? "disc" : "music-off");
  m_emptyGlyph->setGlyphSize(std::min(Style::baseGlyphSize * m_contentScale, std::min(crossLength, maxLength)));
  m_emptyGlyph->setColor(colorSpecFromRole(ColorRole::OnSurfaceVariant));
  m_emptyGlyph->measure(renderer);

  const bool hideAlbumArt = m_hideAlbumArt && !isVertical;
  const bool showArtSlot = !hideAlbumArt && m_art->hasImage();
  const bool showEmptyGlyph = !showArtSlot && !hideAlbumArt;
  float artSize = 0.0F;
  if (showArtSlot) {
    const float requestedArtSize = std::min(m_artSize * m_contentScale, crossLength);
    artSize = artOnly || m_stacked ? requestedArtSize : std::min(requestedArtSize, m_label->height());
    if (!artOnly) {
      artSize = std::min(artSize, maxLength);
    }
  }
  m_art->setVisible(showArtSlot);
  m_art->setSize(artSize, artSize);
  m_art->setRadius(m_stacked && !artOnly ? std::min(4.0F * m_contentScale, artSize * 0.5F) : artSize * 0.5F);
  m_emptyGlyph->setVisible(showEmptyGlyph);

  const float leadingWidth = showArtSlot ? artSize : (showEmptyGlyph ? m_emptyGlyph->width() : 0.0F);
  const float spacing = !artOnly && leadingWidth > 0.0F ? gap : 0.0F;
  const float controlSize = std::min(24.0F * m_contentScale, crossLength);
  const float requestedControlsWidth = static_cast<float>(m_controls.size()) * controlSize;
  // On narrow bars retain the metadata/artwork rather than squeezing controls into it.
  const bool showControls = !artOnly
      && m_showControls
      && !m_lastBusName.empty()
      && controlSize > 0.0F
      && maxLength >= leadingWidth + spacing + 32.0F * m_contentScale + gap + requestedControlsWidth;
  const float controlsWidth = showControls ? requestedControlsWidth : 0.0F;
  const float controlsGap = showControls ? gap : 0.0F;
  const float labelMaxWidth =
      artOnly ? 0.0F : std::max(0.0F, maxLength - leadingWidth - spacing - controlsGap - controlsWidth);
  const bool showLabel = !artOnly && !m_label->text().empty() && labelMaxWidth > 0.0F;
  m_label->setVisible(showLabel);
  m_label->setMaxWidth(labelMaxWidth);
  m_label->measure(renderer);
  if (m_secondaryLabel != nullptr) {
    m_secondaryLabel->setVisible(showLabel && showSecondary);
    m_secondaryLabel->setMaxWidth(labelMaxWidth);
    m_secondaryLabel->measure(renderer);
  }
  applyTitleScrollMode(showLabel);

  float textWidth = showLabel ? m_label->width() : 0.0F;
  float textHeight = showLabel ? m_label->height() : 0.0F;
  if (showLabel && showSecondary) {
    textWidth = std::max(textWidth, m_secondaryLabel->width());
    textHeight += lineGap + m_secondaryLabel->height();
  }
  const float contentHeight = std::min(
      crossLength,
      std::max({
          textHeight,
          artSize,
          showEmptyGlyph ? m_emptyGlyph->height() : 0.0F,
          showControls ? controlSize : 0.0F,
      })
  );
  const float contentWidth = leadingWidth + (showLabel ? spacing + textWidth : 0.0F) + controlsGap + controlsWidth;
  const float width = artOnly ? leadingWidth : std::clamp(contentWidth, minLength, maxLength);
  m_art->setPosition(0.0F, std::round((contentHeight - artSize) * 0.5F));
  m_emptyGlyph->setPosition(0.0F, std::round((contentHeight - m_emptyGlyph->height()) * 0.5F));
  const float textY = std::round((contentHeight - textHeight) * 0.5F);
  m_label->setPosition(leadingWidth + spacing, textY);
  if (m_secondaryLabel != nullptr) {
    m_secondaryLabel->setPosition(leadingWidth + spacing, textY + m_label->height() + lineGap);
  }
  float controlX = width - controlsWidth;
  for (std::size_t i = 0; i < m_controls.size(); ++i) {
    if (m_controls[i] == nullptr) {
      continue;
    }
    m_controls[i]->setVisible(showControls);
    m_controls[i]->setOpacity(m_controlAvailable[i] ? 1.0F : 0.32F);
    if (!showControls) {
      continue;
    }
    m_controls[i]->setSize(controlSize, controlSize);
    m_controls[i]->setPosition(controlX, std::round((contentHeight - controlSize) * 0.5F));
    m_controlGlyphs[i]->setGlyphSize(std::min(Style::baseGlyphSize * m_contentScale, controlSize));
    m_controlGlyphs[i]->measure(renderer);
    m_controlGlyphs[i]->setPosition(
        std::round((controlSize - m_controlGlyphs[i]->width()) * 0.5F),
        std::round((controlSize - m_controlGlyphs[i]->height()) * 0.5F)
    );
    controlX += controlSize;
  }
  rootNode->setSize(width, contentHeight);
  m_progressBar->setVisible(showProgressFill);
  if (showProgressFill) {
    const float fillWidth = rootNode->width();
    const float fillHeight = rootNode->height();
    m_progressBar->setPosition(0.0F, 0.0F);
    m_progressBar->setSize(fillWidth, fillHeight);
    m_progressBar->setRadius(resolvedBarCapsuleRadius(fillWidth, fillHeight));
  }
  // The update phase owns the fill value and the timer, and it runs before layout. Re-enter it when
  // a layout-only pass changes an input it cannot observe, so the fill never shows a stale value and
  // the timer cannot stay unarmed while the fill is on screen.
  if (m_isVertical != wasVertical || showProgressFill != m_progressFillVisible) {
    m_progressFillVisible = showProgressFill;
    requestUpdate();
  }
}

void MediaWidget::doUpdate(Renderer& renderer) {
  const auto active = activePlayer();
  syncState(renderer, active);
  syncProgress(active);
}

bool MediaWidget::needsFrameTick() const {
  // hasImage() already implies the art slot is shown: syncState() clears the art when hide_album_art is set. Do not
  // test visible() here, syncState() runs before doLayout() reveals the art slot for a newly loaded image.
  return m_rotateAlbumArt && m_lastPlaybackStatus == "Playing" && m_art != nullptr && m_art->hasImage();
}

void MediaWidget::onFrameTick(float deltaMs) {
  constexpr float kRotationsPerSecond = 0.1F;
  constexpr float kTwoPi = 2.0F * std::numbers::pi_v<float>;
  // The first tick after a pause carries the whole idle gap; clamp it so resuming does not jump the artwork.
  constexpr float kMaxStepMs = 50.0F;

  m_artRotation += (kTwoPi * kRotationsPerSecond) * (std::min(deltaMs, kMaxStepMs) / 1000.0F);
  if (m_artRotation >= kTwoPi) {
    m_artRotation -= kTwoPi;
  }
  if (m_art != nullptr) {
    m_art->setRotation(m_artRotation);
    requestRedraw();
  }
}

void MediaWidget::applyTitleScrollMode(bool titleVisible) {
  if (m_label == nullptr) {
    return;
  }

  const bool hovered = (m_area != nullptr && m_area->hovered())
      || std::any_of(m_controls.begin(), m_controls.end(), [](const InputArea* control) {
                         return control != nullptr && control->visible() && control->hovered();
                       });
  const bool shouldScroll = titleVisible
      && (m_titleScrollMode == MediaTitleScrollMode::Always
          || (m_titleScrollMode == MediaTitleScrollMode::OnHover && hovered));
  m_label->setAutoScroll(shouldScroll);
  m_label->setAutoScrollOnlyWhenHovered(false);
  if (m_secondaryLabel != nullptr) {
    m_secondaryLabel->setAutoScroll(shouldScroll && m_secondaryLabel->visible());
    m_secondaryLabel->setAutoScrollOnlyWhenHovered(false);
  }
}

void MediaWidget::syncWidgetVisibility(bool showWidget) {
  if (Node* rootNode = root(); rootNode != nullptr) {
    if (rootNode->visible() != showWidget || rootNode->participatesInLayout() != showWidget) {
      rootNode->setVisible(showWidget);
      rootNode->setParticipatesInLayout(showWidget);
      requestUpdate();
    }
  }
}

std::optional<MprisPlayerInfo> MediaWidget::activePlayer() const {
  return m_mpris != nullptr ? m_mpris->activePlayer() : std::nullopt;
}

bool MediaWidget::progressFillEligible(const std::optional<MprisPlayerInfo>& active) const noexcept {
  return m_showProgress && !m_isVertical && !m_albumArtOnly && active.has_value() && active->lengthUs > 0;
}

void MediaWidget::syncProgress(const std::optional<MprisPlayerInfo>& active) {
  if (m_progressBar == nullptr) {
    return;
  }
  // Reset while ineligible so a later reveal cannot flash the previous track's position.
  if (!progressFillEligible(active)) {
    m_progressTimer.stop();
    if (m_progressBar->progress() != 0.0F) {
      m_progressBar->setProgress(0.0F);
      requestRedraw();
    }
    return;
  }

  const float progress = static_cast<float>(active->positionUs) / static_cast<float>(active->lengthUs);
  if (std::abs(progress - m_progressBar->progress()) > 0.0005F) {
    m_progressBar->setProgress(progress);
    requestRedraw();
  }
  if (active->playbackStatus != "Playing") {
    m_progressTimer.stop();
    return;
  }
  // One wakeup per pixel of travel: a 3-minute track in a 200 px widget moves a pixel every ~900 ms.
  const float fillWidth = std::max(1.0F, m_progressBar->width());
  const auto intervalMs =
      static_cast<std::int64_t>(std::clamp(static_cast<float>(active->lengthUs / 1000) / fillWidth, 250.0F, 1000.0F));
  m_progressTimer.start(std::chrono::milliseconds(intervalMs), [this]() { requestUpdate(); });
}

void MediaWidget::syncState(Renderer& renderer, const std::optional<MprisPlayerInfo>& active) {
  if (m_art == nullptr || m_label == nullptr) {
    return;
  }

  const bool playing = active.has_value() && active->playbackStatus == "Playing";
  const bool showWidget = active.has_value() ? (!m_hideWhenIdle || playing) : !m_hideWhenNoMedia;
  syncWidgetVisibility(showWidget);
  if (!showWidget) {
    applyTitleScrollMode(false);
    m_lastPlaybackStatus.clear();
    return;
  }

  const std::string empty;
  const auto& playbackStatus = active.has_value() ? active->playbackStatus : empty;
  const bool playbackChanged = playbackStatus != m_lastPlaybackStatus;
  const bool metadataChanged = !m_stateInitialized
      || (active.has_value() ? active->title != m_lastTitle
                  || active->artists != m_lastArtists
                  || active->identity != m_lastIdentity
                  || active->busName != m_lastBusName
                             : !m_lastBusName.empty());
  bool textChanged = false;
  if (metadataChanged || playbackChanged) {
    std::string displayText;
    std::string secondaryText;
    if (!active.has_value()) {
      displayText = i18n::tr("bar.widgets.media.nothing-playing");
    } else if (!m_stacked) {
      displayText = buildDisplayText(*active, m_hideArtist, m_artistFirst);
    } else {
      displayText = active->title;
      const bool hasArtist = std::any_of(active->artists.begin(), active->artists.end(), [](const std::string& artist) {
        return !artist.empty();
      });
      const std::string artists = m_hideArtist ? std::string() : joinArtists(active->artists);
      if (!displayText.empty()) {
        secondaryText = artists;
        if (secondaryText.empty() && !hasArtist && active->identity != displayText) {
          secondaryText = active->identity;
        }
        if (m_artistFirst && !artists.empty()) {
          displayText.swap(secondaryText);
        }
      } else {
        displayText = buildDisplayText(*active, m_hideArtist, m_artistFirst);
      }
    }
    if (displayText != m_lastText) {
      m_lastText = std::move(displayText);
      m_label->setText(m_lastText);
      textChanged = true;
    }
    if (secondaryText != m_lastSecondaryText) {
      m_lastSecondaryText = std::move(secondaryText);
      if (m_secondaryLabel != nullptr) {
        m_secondaryLabel->setText(m_lastSecondaryText);
      }
      textChanged = true;
    }
    if (metadataChanged) {
      m_lastTitle = active.has_value() ? active->title : empty;
      if (active.has_value()) {
        m_lastArtists = active->artists;
      } else {
        m_lastArtists.clear();
      }
      m_lastIdentity = active.has_value() ? active->identity : empty;
      m_lastBusName = active.has_value() ? active->busName : empty;
    }
  }
  m_stateInitialized = true;

  std::array<bool, 3> controlAvailable{};
  if (m_showControls && active.has_value() && active->canControl) {
    controlAvailable = {
        active->canGoPrevious,
        playbackStatus == "Playing" ? active->canPause : active->canPlay,
        active->canGoNext,
    };
  }
  const bool controlsChanged = controlAvailable != m_controlAvailable;
  m_controlAvailable = controlAvailable;
  if (playbackChanged) {
    m_lastPlaybackStatus = playbackStatus;
    m_label->setColor(
        m_lastPlaybackStatus == "Playing" ? widgetForegroundOr(colorSpecFromRole(ColorRole::OnSurface))
                                          : colorSpecFromRole(ColorRole::OnSurfaceVariant)
    );
    if (m_controlGlyphs[1] != nullptr) {
      m_controlGlyphs[1]->setGlyph(m_lastPlaybackStatus == "Playing" ? "media-pause" : "media-play");
    }
  }

  const std::string artUrl = active.has_value() ? effectiveArtUrl(*active) : empty;
  const bool artChanged = artUrl != m_lastArtUrl;
  const bool hadArt = m_art->hasImage();
  const bool artAwaitingDecode = !m_hideAlbumArt && !artUrl.empty() && !hadArt;
  if (!textChanged && !artChanged && !playbackChanged && !controlsChanged && !artAwaitingDecode) {
    return;
  }
  if (artChanged) {
    m_lastArtUrl = artUrl;
  }

  const int artDecodePx = static_cast<int>(std::round(64.0F * m_contentScale));
  if (artChanged) {
    m_artRotation = 0.0F;
    m_art->setRotation(0.0F);
    if (m_hideAlbumArt) {
      m_art->clear(renderer);
    } else {
      const std::string artPath = resolveArtworkSource(
          m_httpClient, m_pendingArtDownloads, m_lastArtUrl, [this] { requestUpdate(); }, m_aliveGuard
      );
      if (!artPath.empty()) {
        if (!m_art->setSourceFile(renderer, artPath, artDecodePx, true, true)) {
          kLog.warn(R"(artwork load failed url="{}" path="{}")", m_lastArtUrl, artPath);
          m_art->clear(renderer);
        } else {
          kLog.debug(R"(artwork loaded url="{}" path="{}")", m_lastArtUrl, artPath);
        }
      } else {
        if (!m_lastArtUrl.empty()) {
          kLog.debug("artwork unresolved url=\"{}\"", m_lastArtUrl);
        }
        m_art->clear(renderer);
      }
    }
  } else if (!m_lastArtUrl.empty() && !m_art->hasImage() && !m_hideAlbumArt) {
    const std::string artPath = cachedArtworkPath(m_lastArtUrl);
    if (!artPath.empty()) {
      if (m_art->setSourceFile(renderer, artPath, artDecodePx, false, true)) {
        requestRedraw();
      }
    }
  }

  if (needsFrameTick()) {
    requestFrameTick();
  }

  if (textChanged || artChanged || controlsChanged || hadArt != m_art->hasImage()) {
    requestUpdate();
  } else if (playbackChanged) {
    requestRedraw();
  }
}

std::string MediaWidget::buildDisplayText(const MprisPlayerInfo& player, bool hideArtist, bool artistFirst) {
  const std::string artists = hideArtist ? std::string() : joinArtists(player.artists);
  if (!player.title.empty() && !artists.empty()) {
    if (artistFirst) {
      return artists + " - " + player.title;
    }
    return player.title + " - " + artists;
  }
  if (!player.title.empty()) {
    return player.title;
  }
  if (!artists.empty()) {
    return artists;
  }
  if (!player.identity.empty()) {
    return player.identity;
  }
  if (!player.busName.empty()) {
    return player.busName;
  }
  if (player.playbackStatus == "Playing") {
    return i18n::tr("bar.widgets.media.playing");
  }
  return i18n::tr("bar.widgets.media.nothing-playing");
}
