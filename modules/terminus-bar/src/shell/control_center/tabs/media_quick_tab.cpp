#include "shell/control_center/tabs/media_quick_tab.h"

#include "core/deferred_call.h"
#include "core/input/key_modifiers.h"
#include "dbus/mpris/mpris_art.h"
#include "dbus/mpris/mpris_service.h"
#include "pipewire/pipewire_service.h"
#include "pipewire/pipewire_spectrum.h"
#include "render/core/renderer.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/image.h"
#include "ui/visuals/audio_visualizer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include <xkbcommon/xkbcommon-keysyms.h>

using namespace mpris;

namespace {

  constexpr float kArtSize = 96.0F;
  constexpr float kStripHeight = 40.0F;
  constexpr float kSectionGap = Style::spaceSm;
  constexpr float kRow = term_frame::kRowHeight;
  // Box chrome: border + padding on both sides, the title row and the gap under it.
  constexpr float kBoxChrome = 2.0F * (Style::spaceSm + Style::borderWidth) + 20.0F + Style::spaceXs;
  constexpr float kSinkLineHeight = 19.0F;
  constexpr float kIdleHeight = 56.0F;
  constexpr int kBandCount = 32;
  constexpr std::int64_t kSeekStepUs = 5'000'000;
  constexpr std::int64_t kSeekArrivedToleranceUs = 1'500'000;
  constexpr auto kNoActivePlayerGrace = std::chrono::milliseconds(2000);
  constexpr auto kPendingSeekTimeout = std::chrono::milliseconds(5000);

  std::string formatTime(std::int64_t us) {
    const std::int64_t total = std::max<std::int64_t>(0, us) / 1'000'000;
    const std::int64_t hours = total / 3600;
    const std::int64_t minutes = (total / 60) % 60;
    const std::int64_t seconds = total % 60;
    if (hours > 0) {
      return std::format("{}:{:02}:{:02}", hours, minutes, seconds);
    }
    return std::format("{}:{:02}", minutes, seconds);
  }

  void setLabelText(Label* label, const std::string& text) {
    if (label != nullptr && label->text() != text) {
      label->setText(text);
    }
  }

  void setKeyword(
      term_frame::Keyword& keyword, const std::string& word, std::size_t hot, bool block, const std::string& blockText,
      bool enabled
  ) {
    if (keyword.button == nullptr) {
      return;
    }
    if (keyword.word != word || keyword.hot != hot || keyword.block != block || keyword.blockText != blockText) {
      keyword.word = word;
      keyword.hot = hot;
      keyword.block = block;
      keyword.blockText = blockText;
      term_frame::refreshKeyword(keyword);
    }
    keyword.button->setEnabled(enabled);
    keyword.button->setOpacity(enabled ? 1.0F : 0.4F);
  }

} // namespace

MediaQuickTab::MediaQuickTab(
    MprisService* mpris, HttpClient* httpClient, PipeWireSpectrum* spectrum, PipeWireService* audio
)
    : m_mpris(mpris), m_httpClient(httpClient), m_spectrum(spectrum), m_audio(audio) {}

MediaQuickTab::~MediaQuickTab() { m_aliveGuard.reset(); }

std::unique_ptr<Flex> MediaQuickTab::create() {
  const float scale = contentScale();
  const float font = term_frame::kBtopFontSize * scale;
  const float caption = Style::fontSizeCaption * scale;

  auto root = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .gap = kSectionGap * scale,
      .fillWidth = true,
  });

  // ---- hero ----
  auto art = ui::column(
      {
          .out = &m_artBox,
          .align = FlexAlign::Center,
          .justify = FlexJustify::Center,
          .radius = 0.0F,
          .border = term_frame::lineColor(),
          .borderWidth = Style::borderWidth,
          .clipChildren = true,
      },
      ui::image({
          .out = &m_art,
          .fit = ImageFit::Cover,
          .radius = 0.0F,
          .width = (kArtSize - 2.0F * Style::borderWidth) * scale,
          .height = (kArtSize - 2.0F * Style::borderWidth) * scale,
          .visible = false,
      }),
      ui::label({
          .out = &m_artPlaceholder,
          .text = "♪",
          .fontSize = 32.0F * scale,
          .fontFamily = std::string(term_frame::kFont),
          .color = term_frame::dimColor(),
          .maxLines = 1,
      })
  );
  term_frame::pinSize(*art, kArtSize * scale, kArtSize * scale);

  auto heroText = ui::column(
      {
          .align = FlexAlign::Start,
          .justify = FlexJustify::Center,
          .gap = 2.0F * scale,
          .flexGrow = 1.0F,
      },
      ui::label({
          .out = &m_title,
          .text = "nothing playing",
          .fontSize = Style::fontSizeTitle * scale,
          .fontFamily = std::string(term_frame::kFont),
          .color = colorSpecFromRole(ColorRole::Primary),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      }),
      ui::label({
          .out = &m_artist,
          .fontSize = font,
          .fontFamily = std::string(term_frame::kFont),
          .color = term_frame::dimColor(),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      }),
      ui::label({
          .out = &m_album,
          .fontSize = caption,
          .fontFamily = std::string(term_frame::kFont),
          .color = colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.7F),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      })
  );

  auto hero = ui::row({.align = FlexAlign::Center, .gap = Style::spaceMd * scale, .fillWidth = true});
  hero->addChild(std::move(art));
  hero->addChild(std::move(heroText));
  root->addChild(std::move(hero));

  // ---- ¹ playback ----
  auto playBox = term_frame::box("¹", "playback", scale, &m_playBox);

  auto progressRow = ui::row(
      {.align = FlexAlign::Center,
       .gap = Style::spaceSm * scale,
       .minHeight = kRow * scale,
       .fillWidth = true},
      ui::label({
          .out = &m_elapsed,
          .text = "0:00",
          .fontSize = font,
          .fontFamily = std::string(term_frame::kFont),
          .color = term_frame::dimColor(),
          .minWidth = term_frame::monoWidth(5, font),
          .maxLines = 1,
      }),
      ui::slider({
          .out = &m_progress,
          .minValue = 0.0F,
          .maxValue = 100.0F,
          .step = 1.0F,
          .enabled = false,
          .trackHeight = Style::sliderTrackHeight * scale,
          .controlHeight = kRow * scale,
          .width = 1.0F,
          .flexGrow = 1.0F,
          .onValueChanged = [this](double value) { beginSeek(value); },
          .onDragEnd =
              [this]() {
                if (m_progress != nullptr) {
                  commitSeek(m_progress->value());
                }
              },
      }),
      ui::label({
          .out = &m_length,
          .text = "0:00",
          .fontSize = font,
          .fontFamily = std::string(term_frame::kFont),
          .color = term_frame::dimColor(),
          .minWidth = term_frame::monoWidth(5, font),
          .maxLines = 1,
          .textAlign = TextAlign::End,
      })
  );
  playBox->addChild(std::move(progressRow));

  auto transport = ui::row({
      .align = FlexAlign::Center,
      .justify = FlexJustify::SpaceBetween,
      .minHeight = kRow * scale,
      .fillWidth = true,
  });
  transport->addChild(term_frame::makeKeyword(m_kwShuffle, "shuffle", 0, scale, [this]() { cycleShuffle(); }));
  transport->addChild(term_frame::makeKeyword(m_kwPrev, "p ‹‹", 0, scale, [this]() { previousTrack(); }));
  transport->addChild(term_frame::makeKeyword(m_kwPlay, "space ▶", 0, scale, [this]() { togglePlay(); }));
  transport->addChild(term_frame::makeKeyword(m_kwNext, "n ››", 0, scale, [this]() { nextTrack(); }));
  transport->addChild(term_frame::makeKeyword(m_kwRepeat, "repeat", 0, scale, [this]() { cycleRepeat(); }));
  playBox->addChild(std::move(transport));
  root->addChild(std::move(playBox));

  // ---- ² output ----
  auto outBox = term_frame::box("²", "output", scale, &m_outBox);
  term_frame::MeterRow volume;
  outBox->addChild(term_frame::meterRow("player vol", scale, volume));
  m_volume = volume.slider;
  m_volValue = volume.value;
  volume.tag->setColor(term_frame::dimColor());
  volume.tag->setFontSize(font);
  volume.tag->setMinWidth(term_frame::monoWidth(4, font));
  m_volValue->setFontSize(font);
  m_volValue->setMinWidth(term_frame::monoWidth(4, font));
  m_volume->setReadOnly(false);
  m_volume->setStep(0.01);
  m_volume->setWheelAdjustEnabled(true);
  m_volume->setOnValueChanged([this](double value) {
    if (m_syncing || m_mpris == nullptr || m_lastBusName.empty()) {
      return;
    }
    (void)m_mpris->setVolume(m_lastBusName, std::clamp(value, 0.0, 1.0));
    setLabelText(m_volValue, std::format("{}%", static_cast<int>(std::lround(value * 100.0))));
  });
  outBox->addChild(
      ui::label({
          .out = &m_sinkLine,
          .fontSize = caption,
          .fontFamily = std::string(term_frame::kFont),
          .color = term_frame::dimColor(),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      })
  );
  root->addChild(std::move(outBox));

  // ---- ³ players ----
  auto othersBox = term_frame::box("³", "players", scale, &m_othersBox);
  for (std::size_t i = 0; i < kMaxOthers; ++i) {
    othersBox->addChild(
        ui::button({
            .out = &m_otherButtons[i],
            .fontSize = font,
            .contentAlign = ButtonContentAlign::Start,
            .variant = ButtonVariant::Ghost,
            .customPalette = term_frame::keywordPalette(0.8F),
            .surfaceOpacity = 1.0F,
            .minHeight = kRow * scale,
            .paddingV = 0.0F,
            .paddingH = 3.0F * scale,
            .radius = 0.0F,
            .visible = false,
            .onClick =
                [this, i]() {
                  if (m_mpris == nullptr || m_otherBus[i].empty()) {
                    return;
                  }
                  const std::string bus = m_otherBus[i];
                  const std::weak_ptr<void> aliveGuard = m_aliveGuard;
                  DeferredCall::callLater([this, aliveGuard, bus]() {
                    if (aliveGuard.expired() || m_mpris == nullptr) {
                      return;
                    }
                    (void)m_mpris->setPinnedPlayerPreference(bus);
                    PanelManager::instance().refresh();
                  });
                },
            .configure =
                [](Button& button) {
                  if (button.label() != nullptr) {
                    button.label()->setFontFamily(term_frame::kFont);
                    button.label()->setMaxLines(1);
                  }
                },
        })
    );
  }
  root->addChild(std::move(othersBox));

  // ---- spectrum strip ----
  auto spectrum = std::make_unique<AudioVisualizer>();
  spectrum->setGradient(colorSpecFromRole(ColorRole::Primary), colorSpecFromRole(ColorRole::Primary, 0.4F));
  spectrum->setOrientation(AudioSpectrumOrientation::Horizontal);
  spectrum->setValues(std::vector<float>(kBandCount, 0.0F));
  spectrum->tick(0.0F);
  // Placed by hand right under the last box (no section gap above it); see doLayout.
  spectrum->setParticipatesInLayout(false);
  m_spectrumNode = spectrum.get();
  root->addChild(std::move(spectrum));

  applyShape(currentShape());
  return root;
}

std::optional<MprisPlayerInfo> MediaQuickTab::effectiveActive() const {
  if (m_mpris == nullptr) {
    return std::nullopt;
  }
  auto active = m_mpris->activePlayer();
  if (!active.has_value() && m_lastActive.has_value()
      && std::chrono::steady_clock::now() - m_lastActiveSeenAt <= kNoActivePlayerGrace) {
    // Hides transient MPRIS discovery gaps so the panel does not flip size.
    active = m_lastActive;
  }
  return active;
}

std::string MediaQuickTab::defaultSinkLabel() const {
  if (m_audio == nullptr) {
    return {};
  }
  const AudioNode* sink = m_audio->defaultSink();
  return sink != nullptr ? audioDeviceLabel(*sink) : std::string{};
}

MediaQuickTab::Shape MediaQuickTab::currentShape() const {
  Shape shape;
  const auto active = effectiveActive();
  shape.player = active.has_value();
  if (!shape.player) {
    return shape;
  }
  shape.sink = !defaultSinkLabel().empty();
  if (m_mpris != nullptr) {
    std::size_t others = 0;
    for (const auto& player : m_mpris->listPlayers()) {
      if (player.busName != active->busName) {
        ++others;
      }
    }
    shape.others = std::min(others, kMaxOthers);
  }
  return shape;
}

float MediaQuickTab::heightFor(const Shape& shape) {
  if (!shape.player) {
    return kIdleHeight;
  }
  const float playbox = kBoxChrome + kRow + Style::spaceXs + kRow;
  const float outbox = kBoxChrome + kRow + (shape.sink ? Style::spaceXs + kSinkLineHeight : 0.0F);
  float total = kArtSize + kSectionGap + playbox + kSectionGap + outbox;
  if (shape.others > 0) {
    const auto n = static_cast<float>(shape.others);
    total += kSectionGap + kBoxChrome + n * kRow + (n - 1.0F) * Style::spaceXs;
  }
  return total + kStripHeight;
}

float MediaQuickTab::quickPanelHeight(float width, float fallback) const {
  (void)width;
  (void)fallback;
  return heightFor(currentShape());
}

std::string MediaQuickTab::quickPanelHints() const {
  if (!currentShape().player) {
    return {};
  }
  // The keyword row already shows the transport letters; the footer names the rest.
  return "←→ seek  o player";
}

std::string MediaQuickTab::quickPanelStatus() const {
  const auto active = effectiveActive();
  if (!active.has_value()) {
    return {};
  }
  std::string name = term_frame::lowerAscii(active->identity.empty() ? "player" : active->identity);
  if (m_mpris != nullptr && m_mpris->players().size() > 1) {
    name += " ▾";
  }
  return name;
}

void MediaQuickTab::applyShape(const Shape& shape) {
  const bool changed = !m_shapeApplied || !(shape == m_shape);
  m_shape = shape;
  m_shapeApplied = true;
  if (m_root == nullptr) {
    return;
  }
  if (m_artBox != nullptr) {
    m_artBox->setVisible(shape.player);
  }
  if (m_playBox != nullptr) {
    m_playBox->setVisible(shape.player);
  }
  if (m_outBox != nullptr) {
    m_outBox->setVisible(shape.player);
  }
  if (m_sinkLine != nullptr) {
    m_sinkLine->setVisible(shape.sink);
  }
  if (m_othersBox != nullptr) {
    m_othersBox->setVisible(shape.others > 0);
  }
  if (m_spectrumNode != nullptr) {
    m_spectrumNode->setVisible(shape.player);
  }
  if (changed) {
    requestQuickPanelResize();
    PanelManager::instance().requestLayout();
  }
}

void MediaQuickTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_root == nullptr) {
    return;
  }
  const float scale = contentScale();
  const float width = std::max(1.0F, contentWidth);

  const float textWidth = std::max(
      1.0F, width - (m_shape.player ? kArtSize * scale + Style::spaceMd * scale : 0.0F)
  );
  for (auto* label : {m_title, m_artist, m_album}) {
    if (label != nullptr) {
      label->setMaxWidth(textWidth);
    }
  }
  m_root->setSize(width, bodyHeight);
  m_root->layout(renderer);
  if (m_spectrumNode != nullptr) {
    // The strip sits flush under the last visible box instead of one section gap below it.
    const Flex* last = m_othersBox != nullptr && m_othersBox->visible() ? m_othersBox : m_outBox;
    const float top = last != nullptr ? last->y() + last->height() : 0.0F;
    m_spectrumNode->setPosition(0.0F, top);
    m_spectrumNode->setSize(width, kStripHeight * scale);
  }
}

void MediaQuickTab::syncSpectrum(bool wanted) {
  if (m_spectrum == nullptr) {
    return;
  }
  if (wanted && m_spectrumListenerId == 0) {
    m_spectrumListenerId = m_spectrum->addChangeListener(kBandCount, [this]() {
      if (!m_active || m_spectrum->idle()) {
        return;
      }
      PanelManager::instance().requestFrameTick();
    });
  } else if (!wanted && m_spectrumListenerId != 0) {
    m_spectrum->removeChangeListener(m_spectrumListenerId);
    m_spectrumListenerId = 0;
    if (m_spectrumNode != nullptr) {
      m_spectrumNode->setValues(std::vector<float>(kBandCount, 0.0F));
    }
  }
}

void MediaQuickTab::onFrameTick(float deltaMs) {
  if (!m_active || m_spectrumNode == nullptr) {
    return;
  }
  if (m_spectrum != nullptr && m_spectrumListenerId != 0 && (!m_spectrum->idle() || !m_spectrumNode->converged())) {
    m_spectrumNode->setValues(m_spectrum->values(m_spectrumListenerId));
  }
  m_spectrumNode->tick(deltaMs);
}

void MediaQuickTab::setActive(bool active) {
  m_active = active;
  if (!active) {
    m_progressTimer.stop();
    syncSpectrum(false);
  }
}

void MediaQuickTab::onClose() {
  m_progressTimer.stop();
  syncSpectrum(false);
  m_active = false;
  m_root = nullptr;
  m_artBox = nullptr;
  m_art = nullptr;
  m_artPlaceholder = nullptr;
  m_title = nullptr;
  m_artist = nullptr;
  m_album = nullptr;
  m_playBox = nullptr;
  m_elapsed = nullptr;
  m_length = nullptr;
  m_progress = nullptr;
  for (auto* keyword : {&m_kwShuffle, &m_kwPrev, &m_kwPlay, &m_kwNext, &m_kwRepeat}) {
    keyword->button = nullptr;
  }
  m_outBox = nullptr;
  m_volValue = nullptr;
  m_volume = nullptr;
  m_sinkLine = nullptr;
  m_othersBox = nullptr;
  m_otherButtons.fill(nullptr);
  m_spectrumNode = nullptr;
  m_shapeApplied = false;
  m_lastArtUrl.clear();
  m_lastBusName.clear();
  m_pendingSeekUs = -1;
  m_pendingSeekBus.clear();
  m_lastActive.reset();
}

void MediaQuickTab::doUpdate(Renderer& renderer) {
  if (!m_active) {
    m_progressTimer.stop();
    return;
  }
  const auto active = effectiveActive();
  const bool playing = active.has_value() && active->playbackStatus == "Playing";
  const bool pendingSeek = m_pendingSeekUs >= 0 && std::chrono::steady_clock::now() < m_pendingSeekUntil;
  if (playing || pendingSeek) {
    if (!m_progressTimer.active()) {
      m_progressTimer.startRepeating(std::chrono::milliseconds(1000), [this]() {
        if (!m_active) {
          return;
        }
        PanelManager::instance().requestUpdateOnly();
        PanelManager::instance().requestRedraw();
      });
    }
  } else {
    m_progressTimer.stop();
  }
  syncSpectrum(playing);
  refresh(renderer);
}

void MediaQuickTab::refresh(Renderer& renderer) {
  if (m_root == nullptr || m_title == nullptr || m_progress == nullptr) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (m_mpris != nullptr) {
    if (auto live = m_mpris->activePlayer(); live.has_value()) {
      m_lastActive = live;
      m_lastActiveSeenAt = now;
    }
  }
  const auto active = effectiveActive();

  const Shape shape = currentShape();
  if (!m_shapeApplied || !(shape == m_shape)) {
    applyShape(shape);
  }

  if (!active.has_value()) {
    m_pendingSeekUs = -1;
    m_pendingSeekBus.clear();
    m_lastBusName.clear();
    m_lastArtUrl.clear();
    setLabelText(m_title, "nothing playing");
    setLabelText(m_artist, "start playback in any MPRIS app");
    setLabelText(m_album, "");
    m_album->setVisible(false);
    return;
  }

  const auto& player = *active;
  m_lastBusName = player.busName;

  // ---- hero ----
  setLabelText(m_title, player.title.empty() ? player.identity : player.title);
  const std::string artists = joinArtists(player.artists);
  setLabelText(m_artist, artists.empty() ? player.identity : artists);
  setLabelText(m_album, player.album);
  m_album->setVisible(!player.album.empty());

  const std::string artUrl = effectiveArtUrl(player);
  if (m_art != nullptr) {
    if (artUrl.empty()) {
      if (m_art->hasImage()) {
        m_art->clear(renderer);
      }
      m_lastArtUrl.clear();
    } else if (artUrl != m_lastArtUrl || !m_art->hasImage()) {
      const std::string path = resolveArtworkSource(
          m_httpClient, m_pendingArtDownloads, artUrl,
          [this] {
            m_lastArtUrl.clear();
            PanelManager::instance().refresh();
          },
          m_aliveGuard
      );
      bool loaded = false;
      if (!path.empty()) {
        loaded = m_art->setSourceFile(renderer, path, static_cast<int>(std::round(kArtSize * contentScale())), true, true);
      }
      if (!loaded && m_art->hasImage()) {
        m_art->clear(renderer);
      }
      m_lastArtUrl = loaded ? artUrl : std::string{};
    }
    const bool hasArt = m_art->hasImage();
    m_art->setVisible(hasArt);
    if (m_artPlaceholder != nullptr) {
      m_artPlaceholder->setVisible(!hasArt);
    }
  }

  // ---- progress ----
  std::int64_t position = player.lengthUs > 0 ? std::clamp<std::int64_t>(player.positionUs, 0, player.lengthUs)
                                              : std::max<std::int64_t>(0, player.positionUs);
  const bool pendingForPlayer = m_pendingSeekUs >= 0 && (m_pendingSeekBus.empty() || m_pendingSeekBus == player.busName);
  if (pendingForPlayer && std::llabs(position - m_pendingSeekUs) <= kSeekArrivedToleranceUs) {
    m_pendingSeekUs = -1;
    m_pendingSeekBus.clear();
  } else if (pendingForPlayer && now < m_pendingSeekUntil) {
    position = m_pendingSeekUs;
  } else if (pendingForPlayer) {
    m_pendingSeekUs = -1;
    m_pendingSeekBus.clear();
  }

  m_syncing = true;
  m_progress->setEnabled(player.canSeek && player.lengthUs > 0);
  if (player.lengthUs > 0) {
    m_progress->setRange(0.0, static_cast<double>(player.lengthUs) / 1'000'000.0);
  }
  if (!m_progress->dragging()) {
    const double max = m_progress->maxValue();
    m_progress->setValue(max > 0.0 ? std::clamp(static_cast<double>(position) / 1'000'000.0, 0.0, max) : 0.0);
  }
  m_syncing = false;
  const std::int64_t shown =
      m_progress->dragging() ? static_cast<std::int64_t>(m_progress->value() * 1'000'000.0) : position;
  setLabelText(m_elapsed, formatTime(shown));
  setLabelText(m_length, player.lengthUs > 0 ? formatTime(player.lengthUs) : "--:--");

  // ---- transport ----
  const bool playing = player.playbackStatus == "Playing";
  const bool control = player.canControl;
  setKeyword(m_kwShuffle, "shuffle", 0, player.shuffle, "shuffle", control);
  setKeyword(m_kwPrev, "p ‹‹", 0, false, {}, control && player.canGoPrevious);
  setKeyword(
      m_kwPlay, playing ? "space ❚❚" : "space ▶", 0, false, {}, control && (playing ? player.canPause : player.canPlay)
  );
  setKeyword(m_kwNext, "n ››", 0, false, {}, control && player.canGoNext);
  const bool repeating = player.loopStatus != "None";
  setKeyword(m_kwRepeat, "repeat", 0, repeating, player.loopStatus == "Track" ? "repeat 1" : "repeat", control);

  // ---- output ----
  if (m_volume != nullptr) {
    m_syncing = true;
    m_volume->setEnabled(control);
    if (!m_volume->dragging()) {
      m_volume->setValue(std::clamp(player.volume, 0.0, 1.0));
    }
    m_syncing = false;
    setLabelText(m_volValue, std::format("{}%", static_cast<int>(std::lround(std::clamp(player.volume, 0.0, 1.0) * 100.0))));
  }
  if (m_sinkLine != nullptr && shape.sink) {
    setLabelText(m_sinkLine, "sink  " + defaultSinkLabel());
  }

  // ---- other players ----
  std::size_t shownOthers = 0;
  if (m_mpris != nullptr) {
    for (const auto& other : m_mpris->listPlayers()) {
      if (other.busName == player.busName || shownOthers >= kMaxOthers) {
        continue;
      }
      Button* button = m_otherButtons[shownOthers];
      if (button != nullptr) {
        const std::string name = other.identity.empty() ? other.busName : other.identity;
        button->setText(other.title.empty() ? name : name + " · " + other.title);
        button->setVisible(true);
      }
      m_otherBus[shownOthers] = other.busName;
      ++shownOthers;
    }
  }
  for (std::size_t i = shownOthers; i < kMaxOthers; ++i) {
    m_otherBus[i].clear();
    if (m_otherButtons[i] != nullptr) {
      m_otherButtons[i]->setVisible(false);
    }
  }
}

void MediaQuickTab::beginSeek(double valueSeconds) {
  if (m_syncing || m_mpris == nullptr) {
    return;
  }
  m_pendingSeekUs = static_cast<std::int64_t>(std::llround(valueSeconds * 1'000'000.0));
  m_pendingSeekBus = m_lastBusName;
  m_pendingSeekUntil = std::chrono::steady_clock::now() + kPendingSeekTimeout;
}

void MediaQuickTab::commitSeek(double valueSeconds) {
  if (m_syncing || m_mpris == nullptr || m_lastBusName.empty()) {
    return;
  }
  const auto targetUs = static_cast<std::int64_t>(std::llround(valueSeconds * 1'000'000.0));
  m_pendingSeekUs = targetUs;
  m_pendingSeekBus = m_lastBusName;
  m_pendingSeekUntil = std::chrono::steady_clock::now() + kPendingSeekTimeout;
  const std::string bus = m_lastBusName;
  const std::weak_ptr<void> aliveGuard = m_aliveGuard;
  DeferredCall::callLater([this, aliveGuard, bus, targetUs]() {
    if (aliveGuard.expired() || m_mpris == nullptr) {
      return;
    }
    (void)m_mpris->setPosition(bus, targetUs);
    PanelManager::instance().refresh();
  });
}

void MediaQuickTab::withActive(std::function<void(MprisService&, const MprisPlayerInfo&)> action) {
  const auto active = effectiveActive();
  if (!active.has_value() || m_mpris == nullptr) {
    return;
  }
  const std::weak_ptr<void> aliveGuard = m_aliveGuard;
  DeferredCall::callLater([this, aliveGuard, action = std::move(action), player = *active]() {
    if (aliveGuard.expired() || m_mpris == nullptr) {
      return;
    }
    action(*m_mpris, player);
    PanelManager::instance().refresh();
  });
}

void MediaQuickTab::togglePlay() {
  withActive([](MprisService& mpris, const MprisPlayerInfo& player) { (void)mpris.playPause(player.busName); });
}

void MediaQuickTab::previousTrack() {
  withActive([](MprisService& mpris, const MprisPlayerInfo& player) { (void)mpris.previous(player.busName); });
}

void MediaQuickTab::nextTrack() {
  withActive([](MprisService& mpris, const MprisPlayerInfo& player) { (void)mpris.next(player.busName); });
}

void MediaQuickTab::seekBy(std::int64_t offsetUs) {
  withActive([offsetUs](MprisService& mpris, const MprisPlayerInfo& player) {
    if (player.canSeek) {
      (void)mpris.seek(player.busName, offsetUs);
    }
  });
}

void MediaQuickTab::cycleShuffle() {
  withActive([](MprisService& mpris, const MprisPlayerInfo& player) {
    if (player.canControl) {
      (void)mpris.setShuffle(player.busName, !player.shuffle);
    }
  });
}

void MediaQuickTab::cycleRepeat() {
  withActive([](MprisService& mpris, const MprisPlayerInfo& player) {
    if (!player.canControl) {
      return;
    }
    const char* next = player.loopStatus == "None" ? "Playlist" : (player.loopStatus == "Playlist" ? "Track" : "None");
    (void)mpris.setLoopStatus(player.busName, next);
  });
}

void MediaQuickTab::cyclePlayer() {
  withActive([](MprisService& mpris, const MprisPlayerInfo& player) {
    const auto players = mpris.listPlayers();
    if (players.size() < 2) {
      return;
    }
    std::size_t index = 0;
    for (std::size_t i = 0; i < players.size(); ++i) {
      if (players[i].busName == player.busName) {
        index = i;
        break;
      }
    }
    (void)mpris.setPinnedPlayerPreference(players[(index + 1) % players.size()].busName);
  });
}

bool MediaQuickTab::handleQuickPanelKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (preedit || (modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) != 0U) {
    return false;
  }
  if (!currentShape().player) {
    return false;
  }

  switch (sym) {
  case XKB_KEY_space:
    if (pressed) {
      togglePlay();
    }
    return true;
  case XKB_KEY_Left:
    if (pressed) {
      seekBy(-kSeekStepUs);
    }
    return true;
  case XKB_KEY_Right:
    if (pressed) {
      seekBy(kSeekStepUs);
    }
    return true;
  case XKB_KEY_n:
  case XKB_KEY_N:
    if (pressed) {
      nextTrack();
    }
    return true;
  case XKB_KEY_p:
  case XKB_KEY_P:
    if (pressed) {
      previousTrack();
    }
    return true;
  case XKB_KEY_s:
  case XKB_KEY_S:
    if (pressed) {
      cycleShuffle();
    }
    return true;
  case XKB_KEY_r:
  case XKB_KEY_R:
    if (pressed) {
      cycleRepeat();
    }
    return true;
  case XKB_KEY_o:
  case XKB_KEY_O:
    if (pressed) {
      cyclePlayer();
    }
    return true;
  default:
    return false;
  }
}
