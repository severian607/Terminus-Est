#pragma once

#include "shell/settings/settings_registry.h"
#include "ui/controls/label.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

class ConfigService;
class Button;
class Flex;
class Node;

namespace settings {

  struct SettingsStatusBannerProps {
    std::string message;
    bool error = false;
    float scale = 1.0F;
    std::function<void()> onDismiss;
    Flex** out = nullptr;
    Label** messageOut = nullptr;
  };

  [[nodiscard]] std::unique_ptr<Label>
  makeLabel(std::string_view text, float fontSize, const ColorSpec& color, FontWeight fontWeight = FontWeight::Normal);
  [[nodiscard]] std::unique_ptr<Flex> makeSettingsStatusBanner(SettingsStatusBannerProps props);
  void updateSettingsStatusBanner(Flex& banner, Label& message, std::string_view text, bool error);

  // Non-dismissible callout for settings pages/sheets that need HTTP while offline_mode is on.
  // showDisableHint is false on the Security page (toggle lives there already).
  [[nodiscard]] std::unique_ptr<Flex>
  makeOfflineModeNotice(float scale, std::string message, bool showDisableHint = true);
  [[nodiscard]] bool settingsSectionNeedsOfflineModeNotice(SettingsSection section);
  [[nodiscard]] std::string offlineModeNoticeMessage(SettingsSection section);

  // Terminal-look building blocks shared by every settings surface of this window.
  // Square 1px outline tile (term_frame::tilePalette); `alert` swaps the accent for the error role.
  struct OutlineTileProps {
    std::string text;
    float scale = 1.0F;
    bool selected = false;
    bool dim = false;
    bool alert = false;
    bool enabled = true;
    float minWidth = 0.0F;
    std::function<void()> onClick;
  };
  [[nodiscard]] std::unique_ptr<Button> makeOutlineTile(OutlineTileProps props);

  // `● on` / `○ off` marker tile replacing the round-thumb toggle. A click flips the tile immediately
  // and then reports the new value; `enabled == false` renders it inert. A non-empty `label` replaces
  // the on/off wording (`● advanced` / `○ advanced`).
  [[nodiscard]] std::unique_ptr<Button> makeMarkerToggle(
      bool checked, bool enabled, float scale, std::function<void(bool)> onChange, std::string label = {}
  );

  // Row of outline tiles with one selected, replacing the rounded segmented control.
  [[nodiscard]] std::unique_ptr<Flex> makeTileGroup(
      const std::vector<std::string>& labels, std::optional<std::size_t> selectedIndex, float scale,
      std::function<void(std::size_t)> onChange, bool enabled = true
  );

  // Superscript decimal numeral ("¹", "¹²") for numbered boxes.
  [[nodiscard]] std::string superscriptNumber(std::size_t number);
  // ASCII lower-casing for index and box titles.
  [[nodiscard]] std::string lowerLabel(std::string_view text);

  [[nodiscard]] std::optional<std::size_t>
  optionIndex(const std::vector<SelectOption>& options, std::string_view value);
  [[nodiscard]] std::string optionLabel(const std::vector<SelectOption>& options, std::string_view value);
  [[nodiscard]] std::vector<std::string> optionLabels(const std::vector<SelectOption>& options);
  [[nodiscard]] std::vector<ColorSwatchPreview> optionSwatchPreviews(const std::vector<SelectOption>& options);

  [[nodiscard]] bool isMonitorOverrideSettingPath(const std::vector<std::string>& path);
  [[nodiscard]] bool monitorOverrideHasExplicitValue(const Config& cfg, const std::vector<std::string>& path);
  [[nodiscard]] bool settingEntryHasEffectiveOverride(const SettingEntry& entry, const ConfigService& configService);

  [[nodiscard]] bool isBlankInput(std::string_view text);
  [[nodiscard]] std::string formatSliderValue(double value, bool integerValue);
  [[nodiscard]] std::optional<double> parseDoubleInput(std::string_view text);

  [[nodiscard]] std::vector<SelectOption> sessionActionKindOptions();
  [[nodiscard]] std::string
  sessionActionRowSummary(const std::vector<SelectOption>& kindOptions, const SessionPanelActionConfig& row);
  [[nodiscard]] std::string sessionActionDisplayTitle(const SessionPanelActionConfig& row);

  [[nodiscard]] std::string sanitizedIdleBehaviorName(std::string_view text);
  [[nodiscard]] std::string uniqueIdleBehaviorName(
      std::string base, const std::vector<IdleBehaviorConfig>& rows,
      std::optional<std::size_t> ignoreIndex = std::nullopt
  );
  void normalizeIdleBehaviorNames(std::vector<IdleBehaviorConfig>& rows);
  [[nodiscard]] std::string idleBehaviorRowSummary(const IdleBehaviorConfig& row);
  [[nodiscard]] std::string notificationFilterRowSummary(const NotificationFilterConfig& filter);

} // namespace settings
