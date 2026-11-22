#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct Config;
struct ScrollViewState;
class Flex;
class Node;
class RovingListNavHost;
class ScrollView;

namespace settings {
  enum class SettingsSection : std::uint8_t;

  // Pages of the central desktop settings (config/settings.toml) hosted above the bar's own sections.
  // The index id differs from TerminusCentralSettings' section name: "look" is its "appearance".
  [[nodiscard]] bool isCentralSettingsPage(std::string_view id);
  [[nodiscard]] std::string_view centralSettingsSection(std::string_view id);

  // Key under which per-page override counts are reported to the index (and the reset-page scope).
  [[nodiscard]] std::string settingsPageScopeKey(
      std::string_view selectedSection, std::string_view selectedBarName, std::string_view selectedMonitorOverride
  );

  // Index group heading a page lives under: terminus, bar, shell or system.
  [[nodiscard]] std::string_view settingsIndexGroupName(std::string_view sectionId);
  // Lower-case index label of a (non-bar) page.
  [[nodiscard]] std::string settingsIndexLabel(std::string_view sectionId);

  struct SettingsSidebarContext {
    const Config& config;
    const std::vector<SettingsSection>& sections;
    const std::vector<std::string>& availableBars;
    float scale = 1.0F;
    bool globalSearchActive = false;
    // Overridden-setting count per settingsPageScopeKey() (central pages: "look", "screensaver", "login").
    std::unordered_map<std::string, int> overrideCounts;

    ScrollViewState& sidebarScrollState;
    ScrollViewState& contentScrollState;
    std::string& selectedSection;
    std::string& selectedBarName;
    std::string& selectedMonitorOverride;
    std::string& creatingBarName;

    std::function<std::unique_ptr<Node>()> makeSearchField;
    std::function<void()> clearTransientState;
    std::function<void()> clearSearchQuery;
    std::function<void()> requestRebuild;
    std::function<void(std::string)> createBar;
    // Opens the "new monitor override" dialog for the given bar. The create flow lives in a modal
    // (wide enough for the output picker) instead of the narrow sidebar.
    std::function<void(std::string)> openMonitorOverrideCreate;
    std::function<void(const Node*)> scrollSidebarNodeIntoView;
    ScrollView** outScroll = nullptr;
    RovingListNavHost** outNav = nullptr;
  };

  // Search field on top, then the grouped settings index inside a scroll view.
  [[nodiscard]] std::unique_ptr<Flex> buildSettingsSidebar(SettingsSidebarContext ctx);

} // namespace settings
