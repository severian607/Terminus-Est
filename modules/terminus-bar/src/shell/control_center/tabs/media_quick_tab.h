#pragma once

#include "core/timer_manager.h"
#include "dbus/mpris/mpris_service.h"
#include "shell/control_center/tab.h"
#include "shell/control_center/term_frame.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

class AudioVisualizer;
class Button;
class Flex;
class HttpClient;
class Image;
class Label;
class MprisService;
class PipeWireService;
class PipeWireSpectrum;
class Slider;

// Media quick panel: btop-styled hero (art, title, artist, album), playback box with a seekable progress line and
// keyword transport, output box (player volume + default sink), other players and a thin spectrum strip.
class MediaQuickTab : public Tab {
public:
  MediaQuickTab(MprisService* mpris, HttpClient* httpClient, PipeWireSpectrum* spectrum, PipeWireService* audio);
  ~MediaQuickTab() override;

  std::unique_ptr<Flex> create() override;
  void onFrameTick(float deltaMs) override;
  void setActive(bool active) override;
  void onClose() override;

  [[nodiscard]] float quickPanelHeight(float width, float fallback) const override;
  [[nodiscard]] std::string quickPanelHints() const override;
  [[nodiscard]] std::string quickPanelStatus() const override;
  [[nodiscard]] bool
  handleQuickPanelKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) override;

private:
  static constexpr std::size_t kMaxOthers = 4;

  // What the layout depends on; a change reopens the panel at the new height.
  struct Shape {
    bool player = false;
    bool sink = false;
    std::size_t others = 0;
    [[nodiscard]] bool operator==(const Shape&) const = default;
  };

  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;

  [[nodiscard]] std::unique_ptr<Flex> buildHero(float scale);
  [[nodiscard]] std::unique_ptr<Flex> buildPlaybackBox(float scale);
  [[nodiscard]] std::unique_ptr<Flex> buildOutputBox(float scale);
  [[nodiscard]] std::unique_ptr<Flex> buildPlayersBox(float scale);
  [[nodiscard]] std::unique_ptr<AudioVisualizer> buildSpectrum();

  [[nodiscard]] std::optional<MprisPlayerInfo> withinGrace(std::optional<MprisPlayerInfo> live) const;
  [[nodiscard]] std::optional<MprisPlayerInfo> effectiveActive() const;
  [[nodiscard]] std::optional<MprisPlayerInfo> trackActive();
  [[nodiscard]] std::string defaultSinkLabel() const;
  [[nodiscard]] Shape currentShape(const std::optional<MprisPlayerInfo>& active) const;
  [[nodiscard]] static float heightFor(const Shape& shape);
  void applyShape(const Shape& shape);
  void refresh(Renderer& renderer, const std::optional<MprisPlayerInfo>& active);
  void showIdle();
  void refreshHero(Renderer& renderer, const MprisPlayerInfo& player);
  void refreshArt(Renderer& renderer, const std::string& artUrl);
  void refreshProgress(const MprisPlayerInfo& player);
  void refreshTransport(const MprisPlayerInfo& player);
  void refreshOutput(const MprisPlayerInfo& player, const Shape& shape);
  void refreshOthers(const MprisPlayerInfo& player);
  void syncSpectrum(bool wanted);
  std::int64_t setPendingSeek(double valueSeconds);
  void beginSeek(double valueSeconds);
  void commitSeek(double valueSeconds);

  // Actions, deferred to the next main-loop tick like the control-center tab.
  void withActive(std::function<void(MprisService&, const MprisPlayerInfo&)> action);
  void togglePlay();
  void previousTrack();
  void nextTrack();
  void seekBy(std::int64_t offsetUs);
  void cycleShuffle();
  void cycleRepeat();
  void cyclePlayer();
  void pinPlayer(const std::string& busName);

  std::shared_ptr<void> m_aliveGuard = std::make_shared<int>(0);

  MprisService* m_mpris = nullptr;
  HttpClient* m_httpClient = nullptr;
  PipeWireSpectrum* m_spectrum = nullptr;
  PipeWireService* m_audio = nullptr;
  std::uint64_t m_spectrumListenerId = 0;
  bool m_active = false;

  Flex* m_root = nullptr;
  Flex* m_artBox = nullptr;
  Image* m_art = nullptr;
  Label* m_artPlaceholder = nullptr;
  Label* m_title = nullptr;
  Label* m_artist = nullptr;
  Label* m_album = nullptr;

  Flex* m_playBox = nullptr;
  Label* m_elapsed = nullptr;
  Label* m_length = nullptr;
  Slider* m_progress = nullptr;
  term_frame::Keyword m_kwShuffle;
  term_frame::Keyword m_kwPrev;
  term_frame::Keyword m_kwPlay;
  term_frame::Keyword m_kwNext;
  term_frame::Keyword m_kwRepeat;

  Flex* m_outBox = nullptr;
  Label* m_volValue = nullptr;
  Slider* m_volume = nullptr;
  Label* m_sinkLine = nullptr;

  Flex* m_othersBox = nullptr;
  std::array<Button*, kMaxOthers> m_otherButtons{};
  std::array<std::string, kMaxOthers> m_otherBus;

  AudioVisualizer* m_spectrumNode = nullptr;

  Shape m_shape;
  bool m_shapeApplied = false;

  std::string m_lastArtUrl;
  std::string m_lastBusName;
  bool m_syncing = false;
  std::int64_t m_pendingSeekUs = -1;
  std::string m_pendingSeekBus;
  std::chrono::steady_clock::time_point m_pendingSeekUntil;
  std::unordered_set<std::string> m_pendingArtDownloads;
  std::optional<MprisPlayerInfo> m_lastActive;
  std::chrono::steady_clock::time_point m_lastActiveSeenAt;
  Timer m_progressTimer;
};
