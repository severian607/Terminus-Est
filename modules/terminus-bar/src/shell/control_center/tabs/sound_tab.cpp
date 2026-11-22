#include "shell/control_center/tabs/sound_tab.h"

#include "core/input/key_modifiers.h"
#include "core/process/process.h"
#include "render/scene/node.h"
#include "shell/control_center/tabs/async_command.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/button.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"
#include "ui/controls/scroll_view.h"
#include "ui/controls/slider.h"
#include "ui/style.h"

#include <nlohmann/json.hpp>

#include <xkbcommon/xkbcommon-keysyms.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <string_view>
#include <utility>

namespace {

  constexpr const char* kAudioFxCommand = "terminus-audiofx";
  constexpr std::array<const char*, SoundTab::kBandCount> kBandNames = {
      "32", "64", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"
  };
  // Marker + name column shared by the volume and band rows, in monospace cells.
  constexpr std::size_t kTagCols = 14;
  // Fixed two-cell marker column in front of every row: `▸ ` when selected, blank otherwise (also for rows that
  // are not selectable, so every row shares one indent).
  constexpr const char* kMarkerOn = "▸ ";
  constexpr const char* kMarkerOff = "  ";
  constexpr std::size_t kValueCols = 5;
  constexpr float kRow = term_frame::kRowHeight;
  constexpr float kSectionGap = Style::spaceSm;
  // Box chrome: border + padding on both sides, the title row and the gap under it.
  constexpr float kBoxChrome = 2.0F * (Style::spaceSm + Style::borderWidth) + 20.0F + Style::spaceXs;
  // Application rows counted into the panel height; more scroll with the whole body.
  constexpr std::size_t kMaxAppRows = 6;
  constexpr std::size_t kMaxMoveTargets = 9;
  constexpr std::size_t kNameChars = 34;
  constexpr float kVolumeStep = 0.05F;
  constexpr float kBandStep = 1.0F;
  constexpr float kBandSnap = 0.5F;
  constexpr float kGainEpsilon = 0.01F;
  constexpr auto kVolumeHold = std::chrono::milliseconds(450);
  constexpr auto kBandCommitDelay = std::chrono::milliseconds(350);
  constexpr auto kFailureTtl = std::chrono::milliseconds(10000);
  constexpr auto kPendingTimeout = std::chrono::milliseconds(3000);
  constexpr auto kPactlTimeout = std::chrono::milliseconds(5000);
  constexpr auto kAudioFxTimeout = std::chrono::milliseconds(8000);

  [[nodiscard]] std::string formatGain(float gain) { return std::format("{:+.1f}", gain); }

  [[nodiscard]] std::string formatPercent(float volume) {
    return std::format("{}%", static_cast<int>(std::lround(volume * 100.0F)));
  }

  [[nodiscard]] float boxHeight(std::size_t rows) {
    const auto n = static_cast<float>(std::max<std::size_t>(1, rows));
    return kBoxChrome + n * kRow + (n - 1.0F) * Style::spaceXs;
  }

  [[nodiscard]] std::string escapeMarkup(std::string_view text) {
    std::string out;
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

  // At most `maxChars` code points of `text`, with an ellipsis when cut.
  [[nodiscard]] std::string truncateText(const std::string& text, std::size_t maxChars) {
    std::size_t chars = 0;
    std::size_t i = 0;
    while (i < text.size()) {
      if (chars == maxChars) {
        return text.substr(0, i) + "…";
      }
      ++i;
      while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xC0U) == 0x80U) {
        ++i;
      }
      ++chars;
    }
    return text;
  }

  void setLabelText(Label* label, const std::string& text) {
    if (label != nullptr && label->text() != text) {
      label->setText(text);
    }
  }

  void showNode(Node* node, bool show) {
    if (node != nullptr && node->visible() != show) {
      node->setVisible(show);
      node->setParticipatesInLayout(show);
    }
  }

  void styleTag(Label* tag, const std::string& base, bool selected) {
    if (tag == nullptr) {
      return;
    }
    setLabelText(tag, std::string(selected ? kMarkerOn : kMarkerOff) + base);
    tag->setColor(selected ? colorSpecFromRole(ColorRole::Primary) : term_frame::dimColor());
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

  [[nodiscard]] std::string appDisplayName(const AudioNode& node) {
    for (const std::string* candidate :
         {&node.applicationName, &node.applicationBinary, &node.streamTitle, &node.name, &node.description}) {
      if (!candidate->empty()) {
        return *candidate;
      }
    }
    return "application";
  }

  // WirePlumber leaves `default.audio.source` unset when the configured microphone is unplugged, although
  // `pactl get-default-source` still names the source it routes to; the tab reads that name once on open.
  std::string g_fallbackSource;

  // The default node. Asks the node list first (its `isDefault` flag is what the service resolves from the
  // configured default name) and falls back to the named source, so a default that exists is never reported missing.
  [[nodiscard]] const AudioNode* defaultNode(const PipeWireService* pw, bool input) {
    if (pw == nullptr) {
      return nullptr;
    }
    if (const AudioNode* node = input ? pw->defaultSource() : pw->defaultSink(); node != nullptr) {
      return node;
    }
    if (input && !g_fallbackSource.empty()) {
      for (const AudioNode& node : pw->state().sources) {
        if (node.name == g_fallbackSource) {
          return &node;
        }
      }
    }
    return nullptr;
  }

  [[nodiscard]] std::uint32_t defaultId(const PipeWireService* pw, bool input) {
    if (pw == nullptr) {
      return 0;
    }
    if (const AudioNode* node = defaultNode(pw, input); node != nullptr) {
      return node->id;
    }
    return input ? pw->state().defaultSourceId : pw->state().defaultSinkId;
  }

  [[nodiscard]] bool isMonitorSource(const AudioNode& node) {
    return node.name.ends_with(".monitor") || node.description.starts_with("Monitor of ");
  }

  [[nodiscard]] std::vector<std::string> splitDeviceWords(std::string_view text) {
    std::vector<std::string> words;
    std::string current;
    for (const char c : text) {
      if (c == ' ' || c == '\t' || c == '/') {
        if (!current.empty()) {
          words.push_back(std::move(current));
          current.clear();
        }
      } else {
        current += c;
      }
    }
    if (!current.empty()) {
      words.push_back(std::move(current));
    }
    return words;
  }

  [[nodiscard]] std::string joinWords(const std::vector<std::string>& words, std::size_t begin, std::size_t end) {
    std::string out;
    for (std::size_t i = begin; i < end && i < words.size(); ++i) {
      if (!out.empty()) {
        out += ' ';
      }
      out += words[i];
    }
    return out;
  }

  // Short row name of a device. `Starship/Matisse HD Audio Controller Analog Stereo` -> `Analog`,
  // `Ellesmere HDMI Audio [Radeon RX 470/480 / 570/580/590]` -> `HDMI`, `soundcore Liberty 5` (bluetooth) ->
  // `Liberty 5`, `Easy Effects Source` -> `Easy Effects`.
  [[nodiscard]] std::string shortDeviceName(const std::string& description, const std::string& nodeName) {
    std::string clean;
    int depth = 0;
    for (const char c : description) {
      if (c == '[') {
        ++depth;
      } else if (c == ']') {
        depth = std::max(0, depth - 1);
      } else if (depth == 0) {
        clean += c;
      }
    }
    std::vector<std::string> words = splitDeviceWords(clean);
    if (words.empty()) {
      return !description.empty() ? description : nodeName;
    }

    if (nodeName.starts_with("bluez_")) {
      // The device alias; a leading vendor word is dropped when something remains.
      constexpr std::array<std::string_view, 12> vendors = {"soundcore", "anker",  "sony",    "bose",
                                                            "jbl",       "apple",  "samsung", "logitech",
                                                            "jabra",     "beats",  "sennheiser", "marshall"};
      const std::string first = term_frame::lowerAscii(words.front());
      if (words.size() >= 2 && std::find(vendors.begin(), vendors.end(), first) != vendors.end()) {
        return joinWords(words, 1, words.size());
      }
      return joinWords(words, 0, words.size());
    }

    constexpr std::array<std::string_view, 12> ports = {"hdmi",       "displayport", "analog", "digital",
                                                        "spdif",      "optical",     "headphones", "headset",
                                                        "speaker",    "speakers",    "microphone", "line"};
    for (std::size_t i = 0; i < words.size(); ++i) {
      const std::string lower = term_frame::lowerAscii(words[i]);
      if (std::find(ports.begin(), ports.end(), lower) == ports.end()) {
        continue;
      }
      const bool number = i + 1 < words.size() &&
          std::all_of(words[i + 1].begin(), words[i + 1].end(), [](char c) { return c >= '0' && c <= '9'; });
      return joinWords(words, i, number ? i + 2 : i + 1);
    }

    constexpr std::array<std::string_view, 10> generic = {"stereo", "mono",   "surround", "source",     "sink",
                                                          "output", "input",  "duplex",   "audio", "controller"};
    std::size_t end = words.size();
    while (end > 1 &&
           std::find(generic.begin(), generic.end(), term_frame::lowerAscii(words[end - 1])) != generic.end()) {
      --end;
    }
    return joinWords(words, 0, end);
  }

  // Short names of one device list; equal names get ` 2`, ` 3`, … appended.
  [[nodiscard]] std::vector<std::string> shortDeviceNames(const std::vector<AudioNode>& nodes) {
    std::vector<std::string> names;
    names.reserve(nodes.size());
    for (const AudioNode& node : nodes) {
      const std::string base = shortDeviceName(node.description, node.name);
      std::string candidate = base;
      int n = 1;
      while (std::find(names.begin(), names.end(), candidate) != names.end()) {
        candidate = base + " " + std::to_string(++n);
      }
      names.push_back(std::move(candidate));
    }
    return names;
  }

  // PipeWire card of a bluetooth node ("bluez_output.AA_BB_….1" -> "bluez_card.AA_BB_…"); empty for any other node.
  [[nodiscard]] std::string btCardName(const AudioNode& node) {
    constexpr std::string_view prefix = "bluez_";
    if (!node.name.starts_with(prefix)) {
      return {};
    }
    const std::size_t dot = node.name.find('.');
    if (dot == std::string::npos) {
      return {};
    }
    const std::size_t end = node.name.find('.', dot + 1);
    const std::string mac = node.name.substr(dot + 1, end == std::string::npos ? std::string::npos : end - dot - 1);
    return mac.empty() ? std::string() : "bluez_card." + mac;
  }

  // a2dp-sink = high-quality audio, headset-head-unit / handsfree = headset mode.
  [[nodiscard]] std::string shortProfile(const std::string& name) {
    if (name.find("a2dp") != std::string::npos) {
      return "a2dp";
    }
    if (name.find("head-unit") != std::string::npos || name.find("head_unit") != std::string::npos ||
        name.find("handsfree") != std::string::npos || name.find("headset") != std::string::npos) {
      return "headset";
    }
    return name;
  }

  // Selectable profiles of a card, one per kind (a2dp / headset), unavailable ones and `off` dropped.
  template <typename Card> [[nodiscard]] std::vector<std::pair<std::string, std::string>> profileCandidates(const Card& card) {
    std::vector<std::pair<std::string, std::string>> out; // short name, full name
    for (const auto& profile : card.profiles) {
      if (!profile.available || profile.name == "off") {
        continue;
      }
      const std::string shortName = shortProfile(profile.name);
      const bool seen = std::any_of(out.begin(), out.end(), [&](const auto& entry) { return entry.first == shortName; });
      if (!seen) {
        out.emplace_back(shortName, profile.name);
      }
    }
    return out;
  }

} // namespace

SoundTab::SoundTab(
    PipeWireService* audio, EasyEffectsService* /*easyEffects*/, MprisService* /*mpris*/, ConfigService* /*config*/,
    WaylandConnection* /*wayland*/, RenderContext* /*renderContext*/
)
    : m_pw(audio) {
  m_applied.fill(0.0F);
  m_work.fill(0.0F);
  m_eq.gains.fill(0.0F);
  m_bands.fill(nullptr);
}

SoundTab::~SoundTab() { *m_alive = false; }

// ---- Data helpers -----------------------------------------------------------------------------------

std::vector<AudioNode> SoundTab::devices(bool input) const {
  std::vector<AudioNode> list;
  if (m_pw == nullptr) {
    return list;
  }
  for (const AudioNode& node : input ? m_pw->state().sources : m_pw->state().sinks) {
    // Monitors are hidden unless one is the default: a virtual default (Easy Effects, a loopback) is the input.
    if (input && isMonitorSource(node) && node.id != defaultId(m_pw, true)) {
      continue;
    }
    list.push_back(node);
  }
  std::stable_sort(list.begin(), list.end(), [](const AudioNode& a, const AudioNode& b) {
    const std::string la = term_frame::lowerAscii(audioDeviceLabel(a));
    const std::string lb = term_frame::lowerAscii(audioDeviceLabel(b));
    return la != lb ? la < lb : a.id < b.id;
  });
  if (input) {
    // The default input first (virtual or not), the physical microphones below it.
    const std::uint32_t def = defaultId(m_pw, true);
    std::stable_partition(list.begin(), list.end(), [def](const AudioNode& n) { return n.id == def; });
  }
  return list;
}

std::vector<AudioNode> SoundTab::sortedApps() const {
  std::vector<AudioNode> apps;
  if (m_pw != nullptr) {
    apps = m_pw->state().programOutputs;
  }
  std::stable_sort(apps.begin(), apps.end(), [](const AudioNode& a, const AudioNode& b) {
    const std::string na = term_frame::lowerAscii(appDisplayName(a));
    const std::string nb = term_frame::lowerAscii(appDisplayName(b));
    return na != nb ? na < nb : a.id < b.id;
  });
  return apps;
}

const AudioNode* SoundTab::findNode(const Item& item) const {
  if (m_pw == nullptr) {
    return nullptr;
  }
  const auto find = [&item](const std::vector<AudioNode>& nodes) -> const AudioNode* {
    for (const AudioNode& node : nodes) {
      if (node.id == item.ident.id) {
        return &node;
      }
    }
    return nullptr;
  };
  switch (item.ident.kind) {
  case Kind::OutVolume:
    return m_pw->defaultSink();
  case Kind::InVolume:
    return defaultNode(m_pw, true);
  case Kind::OutDevice:
    return find(m_pw->state().sinks);
  case Kind::InDevice:
    return find(m_pw->state().sources);
  case Kind::App:
    return find(m_pw->state().programOutputs);
  default:
    return nullptr;
  }
}

std::string SoundTab::statusKey(const Item& item) const {
  const AudioNode* node = findNode(item);
  switch (item.ident.kind) {
  case Kind::OutDevice:
  case Kind::InDevice:
    if (node != nullptr) {
      const std::string card = btCardName(*node);
      if (!card.empty()) {
        return "card:" + card;
      }
    }
    return "node:" + std::to_string(item.ident.id);
  case Kind::App:
    return "app:" + std::to_string(item.ident.id);
  default:
    return "row:" + std::to_string(static_cast<int>(item.ident.kind));
  }
}

const SoundTab::Card* SoundTab::cardFor(const AudioNode& node) const {
  const std::string name = btCardName(node);
  if (name.empty()) {
    return nullptr;
  }
  const auto it = m_cards.find(name);
  return it != m_cards.end() ? &it->second : nullptr;
}

SoundTab::Shape SoundTab::currentShape() const {
  Shape shape;
  shape.outs = devices(false).size();
  shape.ins = devices(true).size();
  shape.apps = m_pw != nullptr ? m_pw->state().programOutputs.size() : 0;
  const bool bands = m_eqLoaded && m_error.empty() && m_eq.enabled;
  shape.eqRows = 1 + (bands ? kBandCount : 0) + (m_error.empty() ? 0 : 1);
  shape.menuRows = m_menuRows;
  return shape;
}

float SoundTab::heightFor(const Shape& shape) {
  const std::size_t appRows = std::min(std::max<std::size_t>(1, shape.apps), kMaxAppRows);
  const float menu = static_cast<float>(shape.menuRows) * (kRow + Style::spaceXs);
  return boxHeight(1 + std::max<std::size_t>(1, shape.outs)) + boxHeight(1 + std::max<std::size_t>(1, shape.ins)) +
         boxHeight(appRows) + boxHeight(shape.eqRows) + 3.0F * kSectionGap + menu;
}

float SoundTab::quickPanelHeight(float width, float fallback) const {
  (void)width;
  return std::min(heightFor(currentShape()), std::max(1.0F, fallback));
}

std::string SoundTab::quickPanelHints() const { return "↑↓ select  ⏎ default  ←→ adjust  a actions"; }

std::string SoundTab::quickPanelStatus() const {
  if (m_pw == nullptr) {
    return {};
  }
  const AudioNode* sink = m_pw->defaultSink();
  if (sink == nullptr) {
    return {};
  }
  return sink->muted ? "muted" : formatPercent(sink->volume);
}

// ---- Body construction ------------------------------------------------------------------------------

SoundTab::Item& SoundTab::addItem(Kind kind, std::uint32_t id, std::size_t index) {
  auto item = std::make_unique<Item>();
  item->ident = Ident{.kind = kind, .id = id, .index = index};
  if (item->ident == m_selIdent) {
    m_selected = m_items.size();
  }
  m_items.push_back(std::move(item));
  return *m_items.back();
}

void SoundTab::selectItem(const Item& item) {
  for (std::size_t i = 0; i < m_items.size(); ++i) {
    if (m_items[i].get() == &item) {
      select(i);
      return;
    }
  }
}

std::string SoundTab::structureKey() const {
  std::string key;
  for (const bool input : {false, true}) {
    const std::uint32_t def = defaultId(m_pw, input);
    key += input ? "I|" : "O|";
    key += std::to_string(def) + '|';
    for (const AudioNode& node : devices(input)) {
      key += std::to_string(node.id) + ':' + audioDeviceLabel(node) + (node.id == def ? ":d" : ":-");
      if (const Card* card = cardFor(node)) {
        key += ":c" + std::to_string(profileCandidates(*card).size());
      }
      key += '\n';
    }
  }
  key += "A|";
  for (const AudioNode& app : sortedApps()) {
    key += std::to_string(app.id) + ':' + appDisplayName(app) + '\n';
  }
  key += std::format(
      "E|{}{}{}|M|{}{}{}:{}:{}:{}", m_eqLoaded ? 1 : 0, m_eq.enabled ? 1 : 0, m_error.empty() ? 0 : 1,
      m_menuOpen ? 1 : 0, m_moveMode ? 1 : 0, m_moveSel, static_cast<int>(m_selIdent.kind), m_selIdent.id,
      m_selIdent.index
  );
  return key;
}

void SoundTab::markDirty() {
  m_bodyDirty = true;
  PanelManager::instance().requestUpdateOnly();
}

std::unique_ptr<Flex>
SoundTab::buildMeterRow(Item& item, std::string tag, std::function<void(double)> onChange, float scale) {
  const float font = Style::fontSizeCaption * scale;
  term_frame::MeterRow meter;
  auto row = term_frame::meterRow(tag, scale, meter);
  item.row = meter.row;
  item.tag = meter.tag;
  item.slider = meter.slider;
  item.value = meter.value;
  item.base = std::move(tag);

  item.row->setMinHeight(kRow * scale);
  const float tagWidth = term_frame::monoWidth(kTagCols, font);
  item.tag->setFontSize(font);
  item.tag->setColor(term_frame::dimColor());
  item.tag->setMinWidth(tagWidth);
  item.tag->setMaxWidth(tagWidth);
  item.tag->setEllipsize(TextEllipsize::End);
  item.tag->setMaxLines(1);
  item.tag->setText(std::string(kMarkerOff) + item.base);
  item.value->setFontSize(font);
  item.value->setMinWidth(term_frame::monoWidth(kValueCols, font));

  Label* value = item.value;
  item.slider->setReadOnly(false);
  item.slider->setStep(0.01);
  item.slider->setWheelAdjustEnabled(true);
  item.slider->setOnValueChanged([this, onChange = std::move(onChange), value](double volume) {
    if (m_syncing) {
      return;
    }
    const auto clamped = static_cast<float>(std::clamp(volume, 0.0, 1.0));
    onChange(clamped);
    noteVolumeWrite();
    setLabelText(value, formatPercent(clamped));
  });

  // In-row failure / progress text (hidden until used) and the [muted] badge.
  item.row->addChild(
      ui::label({
          .out = &item.status,
          .fontSize = font,
          .fontFamily = std::string(term_frame::kFont),
          .color = term_frame::dimColor(),
          .maxWidth = term_frame::monoWidth(20, font),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .visible = false,
      })
  );
  item.status->setParticipatesInLayout(false);

  Item* self = &item;
  item.badge = std::make_unique<term_frame::Keyword>();
  item.row->addChild(term_frame::makeKeyword(*item.badge, "muted", 99, scale, [this, self]() {
    selectItem(*self);
    toggleMute(*self);
  }));
  setKeyword(*item.badge, "muted", 99, true, "muted", true);
  showNode(item.badge->button, false);
  return row;
}

std::unique_ptr<Flex> SoundTab::buildDeviceRow(Item& item, const AudioNode& node, std::string shortName, float scale) {
  const float font = Style::fontSizeCaption * scale;
  item.base = std::move(shortName);
  Item* self = &item;

  auto row = ui::row({
      .out = &item.row,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .minHeight = kRow * scale,
      .fillWidth = true,
  });
  row->addChild(
      ui::button({
          .out = &item.nameButton,
          .fontSize = font,
          .contentAlign = ButtonContentAlign::Start,
          .variant = ButtonVariant::Ghost,
          .customPalette = term_frame::toggleRowPalette(),
          .surfaceOpacity = 1.0F,
          .minHeight = kRow * scale,
          .paddingV = 0.0F,
          .paddingH = 0.0F,
          .radius = 0.0F,
          .flexGrow = 1.0F,
          .onClick =
              [this, self]() {
                selectItem(*self);
                setDefaultDevice(*self);
              },
          .configure =
              [](Button& button) {
                if (button.label() != nullptr) {
                  button.label()->setFontFamily(term_frame::kFont);
                  button.label()->setUseMarkup(true);
                  button.label()->setMaxLines(1);
                  button.label()->setEllipsize(TextEllipsize::End);
                }
              },
      })
  );
  if (item.nameButton != nullptr) {
    item.nameButton->setTooltip(audioDeviceLabel(node));
  }
  row->addChild(
      ui::label({
          .out = &item.detail,
          .fontSize = font,
          .fontFamily = std::string(term_frame::kFont),
          .color = term_frame::dimColor(),
          .maxWidth = term_frame::monoWidth(22, font),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .visible = false,
      })
  );
  item.detail->setParticipatesInLayout(false);

  item.badge = std::make_unique<term_frame::Keyword>();
  row->addChild(term_frame::makeKeyword(*item.badge, "muted", 99, scale, [this, self]() {
    selectItem(*self);
    toggleMute(*self);
  }));
  setKeyword(*item.badge, "muted", 99, true, "muted", true);
  showNode(item.badge->button, false);

  item.defaultBadge = std::make_unique<term_frame::Keyword>();
  row->addChild(term_frame::makeKeyword(*item.defaultBadge, "default", 99, scale, [this, self]() {
    selectItem(*self);
  }));
  setKeyword(*item.defaultBadge, "default", 99, true, "default", true);
  showNode(item.defaultBadge->button, false);
  return row;
}

std::unique_ptr<Flex> SoundTab::buildEqRow(Item& item, float scale) {
  const float font = Style::fontSizeCaption * scale;
  item.base = "eq";
  auto row = ui::row({
      .out = &item.row,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .minHeight = kRow * scale,
      .fillWidth = true,
  });
  const float tagWidth = term_frame::monoWidth(kTagCols, font);
  row->addChild(
      ui::label({
          .out = &item.tag,
          .text = std::string(kMarkerOff) + item.base,
          .fontSize = font,
          .fontFamily = std::string(term_frame::kFont),
          .color = term_frame::dimColor(),
          .minWidth = tagWidth,
          .maxWidth = tagWidth,
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      })
  );
  Item* self = &item;
  item.badge = std::make_unique<term_frame::Keyword>();
  row->addChild(term_frame::makeKeyword(*item.badge, "off", 99, scale, [this, self]() {
    selectItem(*self);
    toggleEq();
  }));
  row->addChild(
      ui::label({
          .out = &item.detail,
          .fontSize = font,
          .fontFamily = std::string(term_frame::kFont),
          .color = term_frame::dimColor(),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .flexGrow = 1.0F,
      })
  );
  return row;
}

std::unique_ptr<Flex> SoundTab::buildBandRow(Item& item, std::size_t band, float scale) {
  const float font = Style::fontSizeCaption * scale;
  const float tagWidth = term_frame::monoWidth(kTagCols, font);
  Item* self = &item;
  item.base = kBandNames[band];
  auto row = ui::row({
      .out = &item.row,
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .minHeight = kRow * scale,
      .fillWidth = true,
  });
  row->addChild(
      ui::label({
          .out = &item.tag,
          .text = std::string(kMarkerOff) + item.base,
          .fontSize = font,
          .fontFamily = std::string(term_frame::kFont),
          .color = term_frame::dimColor(),
          .minWidth = tagWidth,
          .maxWidth = tagWidth,
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
      })
  );
  row->addChild(
      ui::button({
          .out = &item.meter,
          .fontSize = font,
          .contentAlign = ButtonContentAlign::Start,
          .variant = ButtonVariant::Ghost,
          .customPalette = term_frame::toggleRowPalette(),
          .surfaceOpacity = 1.0F,
          .minHeight = kRow * scale,
          .paddingV = 0.0F,
          .paddingH = 0.0F,
          .radius = 0.0F,
          .flexGrow = 1.0F,
          .onPress =
              [this, self, band](float x, float, bool pressed) {
                if (!m_eqLoaded || !m_error.empty()) {
                  return;
                }
                if (pressed) {
                  selectItem(*self);
                  m_dragBand = band;
                  setBandFromX(band, x);
                } else if (m_dragBand == band) {
                  m_dragBand = kBandCount;
                  commitBand(band);
                }
              },
          .onPointerMotion =
              [this, band](float x, float) {
                if (m_dragBand == band) {
                  setBandFromX(band, x);
                }
              },
          .configure =
              [](Button& button) {
                if (button.label() != nullptr) {
                  button.label()->setFontFamily(term_frame::kFont);
                  button.label()->setUseMarkup(true);
                  button.label()->setMaxLines(1);
                }
              },
      })
  );
  row->addChild(
      ui::label({
          .out = &item.value,
          .text = formatGain(0.0F),
          .fontSize = font,
          .fontFamily = std::string(term_frame::kFont),
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .minWidth = term_frame::monoWidth(kValueCols, font),
          .maxLines = 1,
          .textAlign = TextAlign::End,
      })
  );
  return row;
}

std::vector<SoundTab::MenuAction> SoundTab::menuActions(const Item& item) const {
  std::vector<MenuAction> actions;
  switch (item.ident.kind) {
  case Kind::OutVolume:
  case Kind::InVolume:
    actions.push_back({'m', "mute", 0});
    break;
  case Kind::OutDevice:
  case Kind::InDevice: {
    actions.push_back({'m', "mute", 0});
    const AudioNode* node = findNode(item);
    const Card* card = node != nullptr ? cardFor(*node) : nullptr;
    if (card != nullptr && profileCandidates(*card).size() >= 2) {
      actions.push_back({'p', "profile", 0});
    }
    break;
  }
  case Kind::App:
    actions.push_back({'m', "mute", 0});
    actions.push_back({'o', "move to device", 1});
    break;
  case Kind::EqMain:
    if (m_eqLoaded && m_error.empty() && m_eq.enabled) {
      actions.push_back({'p', "preset", 0});
      actions.push_back({'r', "reset", 0});
    }
    break;
  case Kind::Band:
    break;
  }
  return actions;
}

// The inline action line (or the output list of "move to device") directly under the selected row.
void SoundTab::buildMenu(Flex& box, Item& item, float scale) {
  if (!m_menuOpen || !(item.ident == m_selIdent)) {
    return;
  }
  const float font = Style::fontSizeCaption * scale;
  const auto gutter = [&](const char* text) {
    return ui::label({
        .text = text,
        .fontSize = font,
        .fontFamily = std::string(term_frame::kFont),
        .color = colorSpecFromRole(ColorRole::Primary),
        .maxLines = 1,
    });
  };

  if (m_moveMode && item.ident.kind == Kind::App) {
    const std::vector<AudioNode> outs = devices(false);
    const std::vector<std::string> moveNames = shortDeviceNames(outs);
    const std::size_t count = std::min(outs.size(), kMaxMoveTargets);
    if (count == 0) {
      m_moveMode = false;
    } else {
      m_moveSel = std::min(m_moveSel, count - 1);
      for (std::size_t i = 0; i < count; ++i) {
        auto row = ui::row({
            .align = FlexAlign::Center,
            .gap = 0.0F,
            .minHeight = kRow * scale,
            .fillWidth = true,
        });
        row->addChild(gutter(i == m_moveSel ? kMarkerOn : kMarkerOff));
        auto keyword = std::make_unique<term_frame::Keyword>();
        row->addChild(term_frame::makeKeyword(
            *keyword, std::to_string(i + 1) + " " + truncateText(moveNames[i], kNameChars), 0, scale,
            [this, i]() { executeMove(i); }
        ));
        m_menuKw.push_back(std::move(keyword));
        box.addChild(std::move(row));
      }
      m_menuRows = count;
      return;
    }
  }

  const std::vector<MenuAction> actions = menuActions(item);
  if (actions.empty()) {
    m_menuOpen = false;
    return;
  }
  auto row = ui::row({
      .align = FlexAlign::Center,
      .gap = Style::spaceSm * scale,
      .minHeight = kRow * scale,
      .fillWidth = true,
  });
  row->addChild(gutter(kMarkerOff));
  Item* self = &item;
  for (const MenuAction& action : actions) {
    auto keyword = std::make_unique<term_frame::Keyword>();
    const char key = action.key;
    row->addChild(term_frame::makeKeyword(*keyword, action.word, action.hot, scale, [this, self, key]() {
      selectItem(*self);
      runAction(*self, key);
    }));
    m_menuKw.push_back(std::move(keyword));
  }
  box.addChild(std::move(row));
  m_menuRows = 1;
  if (item.ident.kind == Kind::OutDevice || item.ident.kind == Kind::InDevice) {
    if (const AudioNode* node = findNode(item)) {
      auto nameRow = ui::row({
          .align = FlexAlign::Center,
          .gap = 0.0F,
          .minHeight = kRow * scale,
          .fillWidth = true,
      });
      nameRow->addChild(gutter(kMarkerOff));
      nameRow->addChild(
          ui::label({
              .text = "name: " + audioDeviceLabel(*node),
              .fontSize = font,
              .fontFamily = std::string(term_frame::kFont),
              .color = term_frame::dimColor(),
              .maxLines = 1,
              .ellipsize = TextEllipsize::End,
              .flexGrow = 1.0F,
          })
      );
      box.addChild(std::move(nameRow));
      m_menuRows = 2;
    }
  }
}

void SoundTab::buildDeviceSection(Flex& content, bool input, float scale) {
  const float font = Style::fontSizeCaption * scale;
  Flex* box = nullptr;
  auto boxPtr = term_frame::box(input ? "²" : "¹", input ? "input" : "output", scale, &box);

  const std::vector<AudioNode> list = devices(input);
  const std::vector<std::string> names = shortDeviceNames(list);
  const std::uint32_t defId =
      defaultId(m_pw, input);
  const bool defInList =
      defId != 0 && std::any_of(list.begin(), list.end(), [defId](const AudioNode& n) { return n.id == defId; });

  // The volume row of the default device; sits directly under that device's row (first when the default is not
  // listed, e.g. a monitor, or missing).
  const auto addVolume = [&]() {
    Item& vol = addItem(input ? Kind::InVolume : Kind::OutVolume, 0, 0);
    boxPtr->addChild(buildMeterRow(
        vol, "vol",
        [this, input](double volume) {
          if (m_pw == nullptr) {
            return;
          }
          const AudioNode* node = defaultNode(m_pw, input);
          if (node == nullptr) {
            return;
          }
          if (input) {
            m_pw->setSourceVolume(node->id, static_cast<float>(volume));
          } else {
            m_pw->setSinkVolume(node->id, static_cast<float>(volume));
          }
        },
        scale
    ));
    buildMenu(*box, vol, scale);
  };

  if (!defInList) {
    addVolume();
  }
  if (list.empty()) {
    boxPtr->addChild(
        ui::label({
            .text = std::string(kMarkerOff) + (input ? "no input devices" : "no output devices"),
            .fontSize = font,
            .fontFamily = std::string(term_frame::kFont),
            .color = term_frame::dimColor(),
            .height = kRow * scale,
            .maxLines = 1,
        })
    );
  }
  for (std::size_t i = 0; i < list.size(); ++i) {
    const AudioNode& node = list[i];
    Item& dev = addItem(input ? Kind::InDevice : Kind::OutDevice, node.id, 0);
    boxPtr->addChild(buildDeviceRow(dev, node, names[i], scale));
    buildMenu(*box, dev, scale);
    if (defInList && node.id == defId) {
      addVolume();
    }
  }
  m_sections.push_back(boxPtr.get());
  content.addChild(std::move(boxPtr));
}

void SoundTab::buildAppsSection(Flex& content, float scale) {
  const float font = Style::fontSizeCaption * scale;
  Flex* box = nullptr;
  auto boxPtr = term_frame::box("³", "apps", scale, &box);
  const std::vector<AudioNode> apps = sortedApps();
  if (apps.empty()) {
    boxPtr->addChild(
        ui::label({
            .text = std::string(kMarkerOff) + "nothing playing",
            .fontSize = font,
            .fontFamily = std::string(term_frame::kFont),
            .color = term_frame::dimColor(),
            .height = kRow * scale,
            .maxLines = 1,
        })
    );
  }
  for (const AudioNode& node : apps) {
    Item& app = addItem(Kind::App, node.id, 0);
    const std::uint32_t id = node.id;
    boxPtr->addChild(buildMeterRow(
        app, appDisplayName(node),
        [this, id](double volume) {
          if (m_pw != nullptr) {
            m_pw->setProgramOutputVolume(id, static_cast<float>(volume));
          }
        },
        scale
    ));
    buildMenu(*box, app, scale);
  }
  m_sections.push_back(boxPtr.get());
  content.addChild(std::move(boxPtr));
}

void SoundTab::buildEqSection(Flex& content, float scale) {
  const float font = Style::fontSizeCaption * scale;
  Flex* box = nullptr;
  auto boxPtr = term_frame::box("⁴", "equaliser", scale, &box);
  boxPtr->addChild(
      ui::label({
          .out = &m_eqNotice,
          .fontSize = font,
          .fontFamily = std::string(term_frame::kFont),
          .color = colorSpecFromRole(ColorRole::Error),
          .height = kRow * scale,
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .visible = false,
      })
  );
  m_eqNotice->setParticipatesInLayout(false);

  Item& main = addItem(Kind::EqMain, 0, 0);
  boxPtr->addChild(buildEqRow(main, scale));
  buildMenu(*box, main, scale);

  if (m_eqLoaded && m_error.empty() && m_eq.enabled) {
    for (std::size_t i = 0; i < kBandCount; ++i) {
      Item& band = addItem(Kind::Band, 0, i);
      m_bands[i] = &band;
      boxPtr->addChild(buildBandRow(band, i, scale));
    }
  }
  m_sections.push_back(boxPtr.get());
  content.addChild(std::move(boxPtr));
}

void SoundTab::rebuildBody() {
  if (m_scroll == nullptr || m_dragBand < kBandCount) {
    return;
  }
  flushBand();
  const Ident wanted = m_selIdent;
  Flex* content = m_scroll->content();
  for (Flex* section : m_sections) {
    (void)content->removeChild(section);
  }
  m_sections.clear();
  m_items.clear();
  m_menuKw.clear();
  m_bands.fill(nullptr);
  m_eqNotice = nullptr;
  m_menuRows = 0;
  m_selected = 0;
  m_selIdent = wanted;

  const float scale = contentScale();
  buildDeviceSection(*content, false, scale);
  buildDeviceSection(*content, true, scale);
  buildAppsSection(*content, scale);
  buildEqSection(*content, scale);

  if (m_selected >= m_items.size() || !(m_items[m_selected]->ident == wanted)) {
    // The selected row is gone: fall back to its neighbour in the list and drop any open menu.
    m_selected = std::min(m_selected, m_items.size() - 1);
    m_menuOpen = false;
    m_moveMode = false;
  }
  m_selIdent = m_items[m_selected]->ident;
  m_bodyDirty = false;
  m_eqDirty = true;

  // Bluetooth cards seen for the first time: read their profiles once.
  for (const bool input : {false, true}) {
    for (const AudioNode& node : devices(input)) {
      const std::string card = btCardName(node);
      if (!card.empty() && !m_cards.contains(card) && m_cardsTried.insert(card).second) {
        loadCards();
      }
    }
  }

  updateSelection();
  if (const Node* node = m_items[m_selected]->row) {
    m_scroll->requestScrollIntoView(node);
  }
  PanelManager::instance().requestLayout();
}

std::unique_ptr<Flex> SoundTab::create() {
  const float scale = contentScale();

  auto scroll = ui::scrollView({
      .out = &m_scroll,
      .contentScale = scale,
      .scrollbarVisible = true,
      .viewportPaddingH = 0.0F,
      .viewportPaddingV = 0.0F,
      .configure =
          [](ScrollView& view) {
            view.clearFill();
            view.clearBorder();
          },
  });
  Flex* content = scroll->content();
  content->setDirection(FlexDirection::Vertical);
  content->setAlign(FlexAlign::Stretch);
  content->setGap(kSectionGap * scale);

  m_sections.clear();
  m_items.clear();
  m_menuKw.clear();
  m_bands.fill(nullptr);
  m_menuOpen = false;
  m_moveMode = false;
  m_selIdent = Ident{};
  m_cardsTried.clear();
  rebuildBody();
  m_bodyKey = structureKey();
  m_shape = currentShape();

  m_tickTimer.startRepeating(std::chrono::milliseconds(500), [this]() {
    if (m_scroll != nullptr && (!m_statuses.empty() || !m_pending.empty())) {
      PanelManager::instance().requestUpdateOnly();
    }
  });
  loadCards();
  return scroll;
}

void SoundTab::setActive(bool active) {
  m_active = active;
  if (active && m_scroll != nullptr) {
    m_statusRequested = true;
    enqueueStatus();
    m_cardsTried.clear();
    loadCards();
    asynccmd::run({"pactl", "get-default-source"}, kPactlTimeout, m_alive, [](process::RunResult result) {
      if (result) {
        g_fallbackSource = asynccmd::firstLine(result.out);
      }
    });
  }
}

void SoundTab::onClose() {
  flushBand();
  m_commitTimer.stop();
  m_tickTimer.stop();
  m_scroll = nullptr;
  m_eqNotice = nullptr;
  m_sections.clear();
  m_items.clear();
  m_menuKw.clear();
  m_bands.fill(nullptr);
  m_bodyKey.clear();
  m_bodyDirty = true;
  m_menuOpen = false;
  m_moveMode = false;
  m_menuRows = 0;
  m_statuses.clear();
  m_pending.clear();
  m_statusRequested = false;
  m_syncing = false;
  m_eqPending = false;
  m_dragBand = kBandCount;
}

void SoundTab::noteVolumeWrite() { m_volumeHoldUntil = std::chrono::steady_clock::now() + kVolumeHold; }

void SoundTab::setVolumeLocal(Item& item, float volume, bool muted) {
  if (item.slider == nullptr) {
    return;
  }
  m_syncing = true;
  if (std::abs(item.slider->value() - static_cast<double>(volume)) > 0.0005) {
    item.slider->setValue(static_cast<double>(volume));
  }
  m_syncing = false;
  item.slider->setOpacity(muted ? 0.5F : 1.0F);
  setLabelText(item.value, formatPercent(volume));
}

// ---- Status and pending ------------------------------------------------------------------------------

void SoundTab::setStatus(const std::string& key, std::string text, bool error, std::chrono::milliseconds ttl) {
  m_statuses[key] = RowStatus{.text = std::move(text), .error = error, .until = std::chrono::steady_clock::now() + ttl};
  PanelManager::instance().requestUpdateOnly();
}

void SoundTab::clearStatus(const std::string& key) {
  if (m_statuses.erase(key) > 0) {
    PanelManager::instance().requestUpdateOnly();
  }
}

const SoundTab::RowStatus* SoundTab::statusFor(const std::string& key) const {
  const auto it = m_statuses.find(key);
  return it != m_statuses.end() ? &it->second : nullptr;
}

void SoundTab::addPending(std::string key, std::function<bool()> done, std::chrono::milliseconds timeout) {
  std::erase_if(m_pending, [&key](const Pending& pending) { return pending.key == key; });
  m_pending.push_back(Pending{
      .key = std::move(key), .done = std::move(done), .deadline = std::chrono::steady_clock::now() + timeout
  });
  PanelManager::instance().requestUpdateOnly();
}

void SoundTab::syncPending() {
  const auto now = std::chrono::steady_clock::now();
  for (auto it = m_pending.begin(); it != m_pending.end();) {
    if (it->done()) {
      clearStatus(it->key);
      it = m_pending.erase(it);
    } else if (now > it->deadline) {
      setStatus(it->key, "failed: timeout", true, kFailureTtl);
      it = m_pending.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = m_statuses.begin(); it != m_statuses.end();) {
    if (now > it->second.until) {
      it = m_statuses.erase(it);
    } else {
      ++it;
    }
  }
}

// ---- Sync -------------------------------------------------------------------------------------------

void SoundTab::doUpdate(Renderer& renderer) {
  (void)renderer;
  if (m_scroll == nullptr) {
    return;
  }
  if (!m_statusRequested) {
    m_statusRequested = true;
    enqueueStatus();
  }
  refresh();
}

void SoundTab::refresh() {
  syncPending();
  if (m_bodyDirty || structureKey() != m_bodyKey) {
    rebuildBody();
    m_bodyKey = structureKey();
    m_volumeHoldUntil = {};
  }
  syncItems();

  const Shape shape = currentShape();
  if (!(shape == m_shape)) {
    m_shape = shape;
    requestQuickPanelResize();
    PanelManager::instance().requestLayout();
  }
}

void SoundTab::syncItems() {
  const bool hold = std::chrono::steady_clock::now() < m_volumeHoldUntil;
  for (auto& owned : m_items) {
    Item& item = *owned;
    switch (item.ident.kind) {
    case Kind::OutVolume:
    case Kind::InVolume:
    case Kind::App:
      syncMeter(item, findNode(item), hold);
      break;
    case Kind::OutDevice:
    case Kind::InDevice:
      syncDeviceRow(item, findNode(item));
      break;
    default:
      break;
    }
  }
  if (m_eqDirty) {
    m_eqDirty = false;
    syncEqControls();
  }
}

void SoundTab::syncMeter(Item& item, const AudioNode* node, bool hold) {
  if (item.slider == nullptr) {
    return;
  }
  item.slider->setEnabled(node != nullptr);
  if (node == nullptr) {
    setVolumeLocal(item, 0.0F, false);
    setLabelText(item.value, "–");
    if (item.badge != nullptr) {
      showNode(item.badge->button, false);
    }
    if (item.status != nullptr) {
      const bool volume = item.ident.kind == Kind::InVolume || item.ident.kind == Kind::OutVolume;
      if (volume) {
        setLabelText(item.status, item.ident.kind == Kind::InVolume ? "no input" : "no output");
        item.status->setColor(term_frame::dimColor());
      }
      showNode(item.status, volume);
    }
    return;
  }
  if (item.badge != nullptr) {
    showNode(item.badge->button, node->muted);
  }
  if (!hold && !item.slider->dragging()) {
    setVolumeLocal(item, node->volume, node->muted);
  }
  if (item.status != nullptr) {
    const RowStatus* status = item.ident.kind == Kind::App ? statusFor(statusKey(item)) : nullptr;
    if (status != nullptr) {
      setLabelText(item.status, status->text);
      item.status->setColor(status->error ? colorSpecFromRole(ColorRole::Error) : term_frame::dimColor());
    }
    showNode(item.status, status != nullptr);
  }
}

void SoundTab::syncDeviceRow(Item& item, const AudioNode* node) {
  if (node == nullptr || item.nameButton == nullptr) {
    return;
  }
  const bool selected = m_selected < m_items.size() && m_items[m_selected].get() == &item;
  styleItem(item, selected);
  const std::uint32_t def = m_pw != nullptr
      ? defaultId(m_pw, item.ident.kind == Kind::InDevice)
      : 0;
  showNode(item.defaultBadge->button, node->id == def);
  showNode(item.badge->button, node->muted);

  std::string detail;
  bool error = false;
  if (const RowStatus* status = statusFor(statusKey(item))) {
    detail = status->text;
    error = status->error;
  } else if (const Card* card = cardFor(*node)) {
    if (!card->active.empty() && card->active != "off") {
      detail = "profile: " + shortProfile(card->active);
    }
  }
  setLabelText(item.detail, detail);
  item.detail->setColor(error ? colorSpecFromRole(ColorRole::Error) : term_frame::dimColor());
  showNode(item.detail, !detail.empty());
}

void SoundTab::syncEqControls() {
  const bool usable = m_error.empty() && m_eqLoaded;

  if (m_eqNotice != nullptr) {
    const bool show = !m_error.empty();
    showNode(m_eqNotice, show);
    if (show) {
      setLabelText(m_eqNotice, std::string(kMarkerOff) + "equaliser unavailable: " + m_error);
    }
  }

  for (auto& owned : m_items) {
    Item& item = *owned;
    if (item.ident.kind != Kind::EqMain) {
      continue;
    }
    if (usable && m_eq.enabled) {
      setKeyword(*item.badge, "on", 99, true, "on", true);
    } else {
      setKeyword(*item.badge, "off", 99, false, {}, usable);
    }
    std::string detail;
    if (m_eqPending) {
      detail = "switching…";
    } else if (!m_eqLoaded && m_error.empty()) {
      detail = "loading…";
    } else {
      detail = m_eq.preset.empty() ? std::string("custom") : m_eq.preset;
    }
    setLabelText(item.detail, detail);
  }

  for (std::size_t i = 0; i < kBandCount; ++i) {
    Item* band = m_bands[i];
    if (band == nullptr) {
      continue;
    }
    renderBand(i);
    if (band->meter != nullptr) {
      band->meter->setEnabled(usable);
      band->meter->setOpacity(usable ? 1.0F : 0.5F);
    }
  }
}

// ---- Equaliser meters -------------------------------------------------------------------------------

std::string SoundTab::bandMarkup(float gain) const {
  const std::size_t cells = m_eqCells;
  const std::size_t centre = cells / 2;
  const float span = std::max({std::abs(m_eq.minGain), std::abs(m_eq.maxGain), 1.0F});
  const auto steps = static_cast<std::size_t>(
      std::lround(std::clamp(std::abs(gain) / span, 0.0F, 1.0F) * static_cast<float>(centre))
  );

  const Color primary = colorForRole(ColorRole::Primary);
  const Color outline = colorForRole(ColorRole::Outline);
  const std::string up = term_frame::colorHex(primary);
  const std::string down = term_frame::colorHex(
      rgba((primary.r + outline.r) * 0.5F, (primary.g + outline.g) * 0.5F, (primary.b + outline.b) * 0.5F)
  );
  const std::string rest = term_frame::paletteHex(ColorRole::Outline);
  const std::string axis = term_frame::paletteHex(ColorRole::OnSurfaceVariant);

  std::string out;
  const auto cell = [&out](const std::string& color, std::string_view glyph) {
    out += "<span foreground=\"" + color + "\">";
    out += glyph;
    out += "</span>";
  };
  for (std::size_t i = 0; i < cells; ++i) {
    if (i == centre) {
      cell(axis, "│");
    } else if (gain > 0.0F && i > centre && i <= centre + steps) {
      cell(up, "■");
    } else if (gain < 0.0F && i < centre && i + steps >= centre) {
      cell(down, "■");
    } else {
      cell(rest, "·");
    }
  }
  return out;
}

void SoundTab::renderBand(std::size_t index) {
  if (index >= kBandCount || m_bands[index] == nullptr) {
    return;
  }
  Item& band = *m_bands[index];
  if (band.meter != nullptr) {
    band.meter->setText(bandMarkup(m_work[index]));
  }
  setLabelText(band.value, formatGain(m_work[index]));
}

void SoundTab::setBandFromX(std::size_t index, float x) {
  if (index >= kBandCount || m_bands[index] == nullptr || m_bands[index]->meter == nullptr) {
    return;
  }
  const float width = m_bands[index]->meter->width();
  if (width <= 0.0F || m_eqCells < 3) {
    return;
  }
  const std::size_t centre = m_eqCells / 2;
  const float fraction = std::clamp(x / width, 0.0F, 0.9999F);
  const auto cell = static_cast<std::size_t>(fraction * static_cast<float>(m_eqCells));
  const float span = std::max({std::abs(m_eq.minGain), std::abs(m_eq.maxGain), 1.0F});
  float gain = (static_cast<float>(cell) - static_cast<float>(centre)) / static_cast<float>(centre) * span;
  gain = std::round(gain / kBandSnap) * kBandSnap;
  m_work[index] = std::clamp(gain, m_eq.minGain, m_eq.maxGain);
  renderBand(index);
}

void SoundTab::scheduleBandCommit(std::size_t index) {
  if (m_pendingBand != index) {
    flushBand();
  }
  m_pendingBand = index;
  m_commitTimer.start(kBandCommitDelay, [this]() { flushBand(); });
}

void SoundTab::flushBand() {
  if (m_pendingBand >= kBandCount) {
    return;
  }
  const std::size_t index = m_pendingBand;
  m_pendingBand = kBandCount;
  m_commitTimer.stop();
  commitBand(index);
}

void SoundTab::commitBand(std::size_t index) {
  if (index >= kBandCount) {
    return;
  }
  const float gain = m_work[index];
  if (std::abs(gain - m_applied[index]) < kGainEpsilon) {
    return;
  }
  m_applied[index] = gain;
  enqueue({kAudioFxCommand, "eq", "band", std::to_string(index + 1), std::format("{:.1f}", gain)});
  enqueueStatus();
}

void SoundTab::cyclePreset(int direction) {
  const std::size_t count = m_eq.presets.size();
  if (count == 0 || !m_eqLoaded || !m_error.empty()) {
    return;
  }
  const auto it = std::find(m_eq.presets.begin(), m_eq.presets.end(), m_eq.preset);
  std::size_t next = 0;
  if (it == m_eq.presets.end()) {
    next = direction > 0 ? 0 : count - 1;
  } else {
    const auto current = static_cast<std::size_t>(it - m_eq.presets.begin());
    next = direction > 0 ? (current + 1) % count : (current + count - 1) % count;
  }
  flushBand();
  m_eq.preset = m_eq.presets[next];
  m_eqDirty = true;
  PanelManager::instance().requestUpdateOnly();
  enqueue({kAudioFxCommand, "eq", "preset", m_eq.preset});
  enqueueStatus();
}

void SoundTab::toggleEq() {
  if (!m_eqLoaded || !m_error.empty()) {
    return;
  }
  flushBand();
  m_eqPending = true;
  m_eqDirty = true;
  PanelManager::instance().requestUpdateOnly();
  enqueue({kAudioFxCommand, "eq", m_eq.enabled ? "off" : "on"});
  enqueueStatus();
}

// ---- Selection and actions --------------------------------------------------------------------------

void SoundTab::select(std::size_t index) {
  if (m_items.empty()) {
    return;
  }
  index = std::min(index, m_items.size() - 1);
  if (index == m_selected) {
    return;
  }
  if (m_selected < m_items.size() && m_items[m_selected]->ident.kind == Kind::Band) {
    flushBand();
  }
  m_selected = index;
  m_selIdent = m_items[index]->ident;
  if (m_menuOpen) {
    closeMenu();
  }
  updateSelection();
  if (m_scroll != nullptr) {
    if (const Node* node = m_items[m_selected]->row) {
      m_scroll->requestScrollIntoView(node);
    }
  }
}

void SoundTab::styleItem(Item& item, bool selected) {
  switch (item.ident.kind) {
  case Kind::OutDevice:
  case Kind::InDevice: {
    if (item.nameButton == nullptr) {
      return;
    }
    const std::uint32_t def = m_pw != nullptr
        ? defaultId(m_pw, item.ident.kind == Kind::InDevice)
        : 0;
    std::string text;
    if (selected) {
      text = "<span foreground=\"" + term_frame::paletteHex(ColorRole::Primary) + "\">" + kMarkerOn +
             escapeMarkup(item.base) + "</span>";
    } else if (item.ident.id == def) {
      text = std::string(kMarkerOff) + escapeMarkup(item.base);
    } else {
      text = "<span foreground=\"" + term_frame::paletteHex(ColorRole::OnSurfaceVariant) + "\">" + kMarkerOff +
             escapeMarkup(item.base) + "</span>";
    }
    if (text != item.lastMarkup) {
      item.lastMarkup = text;
      item.nameButton->setText(text);
    }
    break;
  }
  default:
    styleTag(item.tag, item.base, selected);
    break;
  }
}

void SoundTab::updateSelection() {
  for (std::size_t i = 0; i < m_items.size(); ++i) {
    styleItem(*m_items[i], i == m_selected);
  }
}

void SoundTab::adjustSelected(int direction) {
  if (m_pw == nullptr || m_selected >= m_items.size()) {
    return;
  }
  Item& item = *m_items[m_selected];
  const auto stepped = [&item, direction]() {
    const double current = item.slider != nullptr ? item.slider->value() : 0.0;
    return std::clamp(static_cast<float>(current) + static_cast<float>(direction) * kVolumeStep, 0.0F, 1.0F);
  };

  switch (item.ident.kind) {
  case Kind::OutVolume:
  case Kind::InVolume: {
    const AudioNode* node = findNode(item);
    if (node == nullptr) {
      return;
    }
    const float volume = stepped();
    if (item.ident.kind == Kind::InVolume) {
      m_pw->setSourceVolume(node->id, volume);
    } else {
      m_pw->setSinkVolume(node->id, volume);
    }
    noteVolumeWrite();
    setVolumeLocal(item, volume, node->muted);
    return;
  }
  case Kind::App: {
    const AudioNode* node = findNode(item);
    if (node == nullptr) {
      return;
    }
    const float volume = stepped();
    m_pw->setProgramOutputVolume(node->id, volume);
    noteVolumeWrite();
    setVolumeLocal(item, volume, node->muted);
    return;
  }
  case Kind::EqMain:
    if (m_eq.enabled) {
      cyclePreset(direction);
    }
    return;
  case Kind::Band: {
    const std::size_t band = item.ident.index;
    if (band >= kBandCount || !m_eqLoaded || !m_error.empty()) {
      return;
    }
    m_work[band] = std::clamp(m_work[band] + static_cast<float>(direction) * kBandStep, m_eq.minGain, m_eq.maxGain);
    renderBand(band);
    scheduleBandCommit(band);
    return;
  }
  default:
    return;
  }
}

void SoundTab::activateSelected() {
  if (m_selected >= m_items.size()) {
    return;
  }
  const Item& item = *m_items[m_selected];
  switch (item.ident.kind) {
  case Kind::OutDevice:
  case Kind::InDevice:
    setDefaultDevice(item);
    break;
  case Kind::EqMain:
    toggleEq();
    break;
  default:
    break;
  }
}

void SoundTab::toggleMenu() {
  if (m_selected >= m_items.size()) {
    return;
  }
  if (m_menuOpen) {
    closeMenu();
    return;
  }
  if (menuActions(*m_items[m_selected]).empty()) {
    return;
  }
  m_menuOpen = true;
  m_moveMode = false;
  markDirty();
}

void SoundTab::closeMenu() {
  m_menuOpen = false;
  m_moveMode = false;
  markDirty();
}

void SoundTab::runAction(Item& item, char key) {
  const Kind kind = item.ident.kind;
  switch (key) {
  case 'm':
    toggleMute(item);
    closeMenu();
    break;
  case 'o':
    if (kind == Kind::App) {
      startMove(item);
    }
    break;
  case 'p':
    if (kind == Kind::EqMain) {
      cyclePreset(1);
    } else {
      cycleProfile(item);
    }
    closeMenu();
    break;
  case 'r':
    if (kind == Kind::EqMain && m_eqLoaded && m_error.empty()) {
      flushBand();
      m_eqPending = true;
      m_eqDirty = true;
      enqueue({kAudioFxCommand, "eq", "reset"});
      enqueueStatus();
    }
    closeMenu();
    break;
  default:
    break;
  }
}

void SoundTab::toggleMute(const Item& item) {
  const AudioNode* node = findNode(item);
  if (m_pw == nullptr || node == nullptr) {
    return;
  }
  switch (item.ident.kind) {
  case Kind::OutVolume:
  case Kind::OutDevice:
    m_pw->setSinkMuted(node->id, !node->muted);
    break;
  case Kind::InVolume:
  case Kind::InDevice:
    m_pw->setSourceMuted(node->id, !node->muted);
    break;
  case Kind::App:
    m_pw->setProgramOutputMuted(node->id, !node->muted);
    noteVolumeWrite();
    break;
  default:
    break;
  }
}

void SoundTab::setDefaultDevice(const Item& item) {
  const AudioNode* node = findNode(item);
  if (m_pw == nullptr || node == nullptr) {
    return;
  }
  const bool input = item.ident.kind == Kind::InDevice;
  const std::uint32_t id = node->id;
  PipeWireService* pw = m_pw;
  if (id == defaultId(pw, input)) {
    return;
  }
  const std::string key = statusKey(item);
  const bool issued = input ? pw->setDefaultSource(id) : pw->setDefaultSink(id);
  if (!issued) {
    setStatus(key, "failed: could not set default", true, kFailureTtl);
    return;
  }
  setStatus(key, "setting default…", false, kPendingTimeout);
  addPending(
      key,
      [pw, id, input]() { return id == defaultId(pw, input); },
      kPendingTimeout
  );
}

void SoundTab::startMove(const Item& item) {
  const AudioNode* node = findNode(item);
  if (node == nullptr) {
    return;
  }
  const std::vector<AudioNode> outs = devices(false);
  if (outs.empty()) {
    setStatus(statusKey(item), "failed: no output device", true, kFailureTtl);
    closeMenu();
    return;
  }
  const std::uint32_t current = node->routePinned ? node->routeSinkId : (m_pw != nullptr ? m_pw->state().defaultSinkId : 0);
  m_moveSel = 0;
  for (std::size_t i = 0; i < outs.size() && i < kMaxMoveTargets; ++i) {
    if (outs[i].id == current) {
      m_moveSel = i;
    }
  }
  m_moveMode = true;
  markDirty();
}

void SoundTab::executeMove(std::size_t targetIndex) {
  if (m_selected >= m_items.size() || m_pw == nullptr) {
    return;
  }
  const Item& item = *m_items[m_selected];
  const AudioNode* app = findNode(item);
  const std::vector<AudioNode> outs = devices(false);
  if (app == nullptr || item.ident.kind != Kind::App || targetIndex >= std::min(outs.size(), kMaxMoveTargets)) {
    return;
  }
  const std::uint32_t appId = app->id;
  const std::uint32_t sinkId = outs[targetIndex].id;
  const std::string key = statusKey(item);
  PipeWireService* pw = m_pw;
  closeMenu();
  if (!pw->moveProgramOutput(appId, sinkId)) {
    setStatus(key, "failed: cannot move", true, kFailureTtl);
    return;
  }
  setStatus(key, "moving…", false, kPendingTimeout);
  addPending(
      key,
      [pw, appId, sinkId]() {
        for (const AudioNode& node : pw->state().programOutputs) {
          if (node.id == appId) {
            return node.routePinned && node.routeSinkId == sinkId;
          }
        }
        return true; // the stream is gone; nothing left to confirm
      },
      kPendingTimeout
  );
}

void SoundTab::cycleProfile(const Item& item) {
  const AudioNode* node = findNode(item);
  if (node == nullptr) {
    return;
  }
  const std::string key = statusKey(item);
  const std::string cardName = btCardName(*node);
  const Card* card = cardFor(*node);
  if (card == nullptr) {
    setStatus(key, "failed: no profile info", true, kFailureTtl);
    return;
  }
  if (!process::commandExists("pactl")) {
    setStatus(key, "failed: pactl not found", true, kFailureTtl);
    return;
  }
  const auto candidates = profileCandidates(*card);
  if (candidates.size() < 2) {
    setStatus(key, "failed: no other profile", true, kFailureTtl);
    return;
  }
  const std::string current = shortProfile(card->active);
  std::size_t next = 0;
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    if (candidates[i].first == current) {
      next = (i + 1) % candidates.size();
      break;
    }
  }
  const auto [shortName, profile] = candidates[next];
  setStatus(key, "profile → " + shortName + "…", false, std::chrono::milliseconds(15000));
  asynccmd::run(
      {"pactl", "set-card-profile", cardName, profile}, kPactlTimeout, m_alive, [this, key](process::RunResult result) {
        if (!result) {
          std::string message = asynccmd::firstLine(result.err);
          if (message.empty()) {
            message = result.timedOut ? "timeout" : std::format("exit code {}", result.exitCode);
          }
          setStatus(key, "failed: " + message, true, kFailureTtl);
          return;
        }
        clearStatus(key);
        loadCards();
      }
  );
}

// ---- pactl ------------------------------------------------------------------------------------------

void SoundTab::loadCards() {
  if (m_cardsLoading || !process::commandExists("pactl")) {
    return;
  }
  m_cardsLoading = true;
  asynccmd::run({"pactl", "-f", "json", "list", "cards"}, kPactlTimeout, m_alive, [this](process::RunResult result) {
    m_cardsLoading = false;
    if (result) {
      applyCards(result.out);
    }
  });
}

void SoundTab::applyCards(const std::string& json) {
  const auto doc = nlohmann::json::parse(json, nullptr, false);
  if (doc.is_discarded() || !doc.is_array()) {
    return;
  }
  std::unordered_map<std::string, Card> cards;
  for (const auto& entry : doc) {
    if (!entry.is_object()) {
      continue;
    }
    const std::string name = entry.value("name", std::string());
    if (!name.starts_with("bluez_card.")) {
      continue;
    }
    Card card;
    if (entry.contains("active_profile") && entry["active_profile"].is_string()) {
      card.active = entry["active_profile"].get<std::string>();
    }
    if (entry.contains("profiles") && entry["profiles"].is_object()) {
      for (const auto& [profileName, info] : entry["profiles"].items()) {
        CardProfile profile;
        profile.name = profileName;
        if (info.is_object() && info.contains("available") && info["available"].is_boolean()) {
          profile.available = info["available"].get<bool>();
        }
        card.profiles.push_back(std::move(profile));
      }
    }
    cards[name] = std::move(card);
  }
  m_cards = std::move(cards);
  PanelManager::instance().requestUpdateOnly();
}

// ---- Keys -------------------------------------------------------------------------------------------

bool SoundTab::handleQuickPanelKey(std::uint32_t sym, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (preedit || (modifiers & (KeyMod::Ctrl | KeyMod::Alt | KeyMod::Super)) != 0U) {
    return false;
  }
  if (m_scroll == nullptr || m_items.empty()) {
    return false;
  }
  if (sym >= XKB_KEY_A && sym <= XKB_KEY_Z) {
    sym = sym - XKB_KEY_A + XKB_KEY_a;
  }

  const bool up = sym == XKB_KEY_Up || sym == XKB_KEY_k;
  const bool down = sym == XKB_KEY_Down || sym == XKB_KEY_j;
  const bool enter = sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter;

  // "move to device" sub-line: Up/Down pick an output, Enter or its number moves the stream.
  if (m_menuOpen && m_moveMode) {
    if (!pressed) {
      return up || down || enter || sym == XKB_KEY_a || (sym >= XKB_KEY_1 && sym <= XKB_KEY_9);
    }
    const std::size_t count = std::min(devices(false).size(), kMaxMoveTargets);
    if (up) {
      m_moveSel = m_moveSel > 0 ? m_moveSel - 1 : 0;
      markDirty();
      return true;
    }
    if (down) {
      m_moveSel = std::min(m_moveSel + 1, count > 0 ? count - 1 : 0);
      markDirty();
      return true;
    }
    if (enter) {
      executeMove(m_moveSel);
      return true;
    }
    if (sym >= XKB_KEY_1 && sym <= XKB_KEY_9) {
      executeMove(sym - XKB_KEY_1);
      return true;
    }
    if (sym == XKB_KEY_a) {
      closeMenu();
      return true;
    }
    return false;
  }

  if (up) {
    if (pressed) {
      select(m_selected > 0 ? m_selected - 1 : 0);
    }
    return true;
  }
  if (down) {
    if (pressed) {
      select(m_selected + 1);
    }
    return true;
  }
  if (sym == XKB_KEY_Left || sym == XKB_KEY_Right) {
    if (pressed) {
      adjustSelected(sym == XKB_KEY_Left ? -1 : 1);
    }
    return true;
  }
  if (enter) {
    if (pressed) {
      activateSelected();
    }
    return true;
  }
  if (sym == XKB_KEY_a) {
    if (pressed) {
      toggleMenu();
    }
    return true;
  }
  if (m_menuOpen && m_selected < m_items.size()) {
    Item& item = *m_items[m_selected];
    for (const MenuAction& action : menuActions(item)) {
      if (static_cast<std::uint32_t>(action.key) == sym) {
        if (pressed) {
          runAction(item, action.key);
        }
        return true;
      }
    }
  }
  return false;
}

// ---- Layout -----------------------------------------------------------------------------------------

void SoundTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_scroll == nullptr) {
    return;
  }
  const float scale = contentScale();
  const float width = std::max(1.0F, contentWidth);
  const float font = Style::fontSizeCaption * scale;
  const float gap = Style::spaceSm * scale;
  const float inner = std::max(1.0F, width - 2.0F * term_frame::boxInset(scale));

  // Meter cells that fit between the tag and value columns; odd so the 0 dB axis owns a cell, one cell of slack.
  const float meterWidth = inner - term_frame::monoWidth(kTagCols + kValueCols, font) - 2.0F * gap;
  const float cellWidth = term_frame::monoWidth(1, font);
  auto cells = static_cast<std::size_t>(std::max(0.0F, std::floor(meterWidth / cellWidth)));
  cells = cells > 1 ? cells - 1 : 0;
  if (cells % 2 == 0 && cells > 0) {
    --cells;
  }
  cells = std::max<std::size_t>(cells, 5);
  if (cells != m_eqCells) {
    m_eqCells = cells;
    for (std::size_t i = 0; i < kBandCount; ++i) {
      renderBand(i);
    }
  }

  m_scroll->setSize(width, bodyHeight);
  m_scroll->layout(renderer);
}

// ---- terminus-audiofx jobs ------------------------------------------------------------------------

void SoundTab::enqueue(std::vector<std::string> args) {
  m_queue.push_back(Job{.args = std::move(args), .status = false});
  pump();
}

void SoundTab::enqueueStatus() {
  const bool pending = std::any_of(m_queue.begin(), m_queue.end(), [](const Job& job) { return job.status; });
  if (!pending) {
    m_queue.push_back(Job{.args = {kAudioFxCommand, "eq", "status", "--json"}, .status = true});
  }
  pump();
}

void SoundTab::pump() {
  if (m_busy || m_queue.empty()) {
    return;
  }
  Job job = std::move(m_queue.front());
  m_queue.pop_front();

  if (!process::commandExists(kAudioFxCommand)) {
    m_queue.clear();
    setError(std::string(kAudioFxCommand) + " not found");
    return;
  }

  m_busy = true;
  asynccmd::run(job.args, kAudioFxTimeout, m_alive, [this, job](process::RunResult result) {
    m_busy = false;
    onJobDone(job, result);
    pump();
  });
}

void SoundTab::onJobDone(const Job& job, const process::RunResult& result) {
  if (!result) {
    std::string message = asynccmd::firstLine(result.err);
    if (message.empty()) {
      message = result.timedOut ? "command timed out" : std::format("exit code {}", result.exitCode);
    }
    // A queued status read would clear the message; the next action or reopen re-reads.
    m_queue.clear();
    setError(std::move(message));
    return;
  }
  if (job.status) {
    applyStatus(result.out);
  }
}

void SoundTab::applyStatus(const std::string& json) {
  const auto doc = nlohmann::json::parse(json, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) {
    setError("unreadable status output");
    return;
  }

  EqState next;
  next.enabled = doc.value("enabled", false);
  next.minGain = doc.value("min", -12.0F);
  next.maxGain = doc.value("max", 12.0F);
  if (next.maxGain <= next.minGain) {
    next.minGain = -12.0F;
    next.maxGain = 12.0F;
  }
  if (doc.contains("preset") && doc["preset"].is_string()) {
    next.preset = doc["preset"].get<std::string>();
  }
  if (doc.contains("presets") && doc["presets"].is_array()) {
    for (const auto& item : doc["presets"]) {
      if (item.is_string()) {
        next.presets.push_back(item.get<std::string>());
      }
    }
  }
  next.gains.fill(0.0F);
  if (doc.contains("bands") && doc["bands"].is_array()) {
    std::size_t i = 0;
    for (const auto& band : doc["bands"]) {
      if (i >= kBandCount) {
        break;
      }
      if (band.is_object()) {
        next.gains[i] = band.value("gain", 0.0F);
      }
      ++i;
    }
  }

  m_eq = std::move(next);
  m_applied = m_eq.gains;
  for (std::size_t i = 0; i < kBandCount; ++i) {
    if (i != m_dragBand && i != m_pendingBand) {
      m_work[i] = m_eq.gains[i];
    }
  }
  m_eqLoaded = true;
  m_eqPending = false;
  m_error.clear();
  m_eqDirty = true;
  PanelManager::instance().requestUpdateOnly();
}

void SoundTab::setError(std::string message) {
  m_error = std::move(message);
  m_eqPending = false;
  m_eqDirty = true;
  PanelManager::instance().requestUpdateOnly();
}
