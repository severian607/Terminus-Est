#include "shell/settings/settings_sidebar.h"

#include "config/config_types.h"
#include "i18n/i18n.h"
#include "render/core/renderer.h"
#include "shell/control_center/term_frame.h"
#include "shell/settings/settings_content_common.h"
#include "shell/settings/settings_registry.h"
#include "ui/builders.h"
#include "ui/controls/roving_list_nav.h"
#include "ui/controls/scroll_view.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <functional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace settings {
  namespace {

    constexpr float kSidebarWidth = 236.0F;
    constexpr float kSidebarPadding = 6.0F;
    constexpr float kIndentLeft = 24.0F;
    constexpr float kEntryLeft = 10.0F;

    constexpr std::array<std::string_view, 3> kCentralPages{"look", "screensaver", "login"};

    // Hidden sections (wallpaper, notifications, desktop, dock, niri, umbriel) are dropped from the registry,
    // so they never reach `ctx.sections`; see kHiddenSections in settings_registry.cpp.
    constexpr std::array<SettingsSection, 5> kBarGroup{
        SettingsSection::Panels, SettingsSection::ControlCenter, SettingsSection::Launcher,
        SettingsSection::Osd,    SettingsSection::Screenshot,
    };
    constexpr std::array<SettingsSection, 5> kShellGroup{
        SettingsSection::Appearance, SettingsSection::Shell,   SettingsSection::Templates,
        SettingsSection::Keybinds,   SettingsSection::Plugins,
    };
    constexpr std::array<SettingsSection, 7> kSystemGroup{
        SettingsSection::System, SettingsSection::Services, SettingsSection::Location,
        SettingsSection::Calendar, SettingsSection::Power,  SettingsSection::Security,
        SettingsSection::Hooks,
    };

    std::string normalizedConfigId(std::string_view text) { return StringUtils::trim(text); }

    bool isValidConfigId(std::string_view text) {
      const auto trimmed = StringUtils::trim(text);
      if (trimmed.empty()) {
        return false;
      }
      return std::ranges::all_of(trimmed, [](unsigned char c) { return std::isalnum(c) != 0 || c == '_' || c == '-'; });
    }

    bool barNameExists(const std::vector<std::string>& barNames, std::string_view name) {
      return std::ranges::contains(barNames, name);
    }

    std::string nextAvailableBarName(const std::vector<std::string>& barNames) {
      for (std::size_t i = 1;; ++i) {
        const std::string candidate = i == 1 ? "bar" : std::format("bar_{}", i);
        if (!barNameExists(barNames, candidate)) {
          return candidate;
        }
      }
    }

    // Index row palette: transparent at rest, faint accent on hover, inverted (accent ground, surface text)
    // when selected.
    Button::ButtonPalette indexPalette(bool dim) {
      using State = Button::ButtonStateColors;
      const ColorSpec rest = dim ? colorSpecFromRole(ColorRole::OnSurfaceVariant, 0.65F)
                                 : colorSpecFromRole(ColorRole::OnSurface);
      Button::ButtonPalette palette;
      palette.borderWidth = 0.0F;
      palette.normal = State{.bg = clearColorSpec(), .border = clearColorSpec(), .label = rest};
      palette.hover = State{
          .bg = colorSpecFromRole(ColorRole::Primary, 0.12F),
          .border = clearColorSpec(),
          .label = colorSpecFromRole(ColorRole::Primary),
      };
      palette.pressed = State{
          .bg = colorSpecFromRole(ColorRole::Primary, 0.22F),
          .border = clearColorSpec(),
          .label = colorSpecFromRole(ColorRole::Primary),
      };
      palette.disabled = palette.normal;
      palette.selected = State{
          .bg = colorSpecFromRole(ColorRole::Primary),
          .border = colorSpecFromRole(ColorRole::Primary),
          .label = colorSpecFromRole(ColorRole::Surface),
      };
      return palette;
    }

    struct IndexItem {
      std::string label;
      int count = 0;
      bool selected = false;
      bool dim = false;
      bool indent = false;
      std::function<void()> onClick;
    };

    std::unique_ptr<Button> makeIndexButton(const IndexItem& item, float scale) {
      auto button = ui::button({
          .text = item.label,
          .fontSize = Style::fontSizeCaption * scale,
          .selected = item.selected,
          .contentAlign = ButtonContentAlign::Start,
          .variant = ButtonVariant::Outline,
          .customPalette = indexPalette(item.dim),
          .surfaceOpacity = 1.0F,
          .minHeight = 0.0F,
          .paddingTop = Style::spaceXs * scale,
          .paddingRight = Style::spaceSm * scale,
          .paddingBottom = Style::spaceXs * scale,
          .paddingLeft = (item.indent ? kIndentLeft : kEntryLeft) * scale,
          .gap = Style::spaceSm * scale,
          .radius = 0.0F,
          .onClick = item.onClick,
          .configure =
              [](Button& control) {
                control.setDirection(FlexDirection::Horizontal);
                control.setAlign(FlexAlign::Center);
                control.setTabStop(false);
                if (control.label() != nullptr) {
                  control.label()->setFontFamily(term_frame::kFont);
                  control.label()->setMaxLines(1);
                  control.label()->setEllipsize(TextEllipsize::End);
                  control.label()->setFlexGrow(1.0F);
                }
              },
      });
      const auto addTag = [&](const std::string& text) {
        button->addChild(
            ui::label({
                .text = text,
                .fontSize = Style::fontSizeCaption * scale,
                .fontFamily = std::string(term_frame::kFont),
                .maxLines = 1,
            })
        );
      };
      if (item.count > 0) {
        addTag(std::to_string(item.count));
      }
      return button;
    }

    void addNavButton(RovingListNavHost& nav, std::unique_ptr<Button> button, std::function<void()> onClick) {
      Button* raw = button.get();
      nav.registerItem(raw, std::move(onClick));
      nav.addChild(std::move(button));
    }

    std::unique_ptr<Flex> makeHeading(std::size_t number, std::string_view name, float scale) {
      auto row = term_frame::titleRow(superscriptNumber(number), name, scale);
      row->setPadding(Style::spaceSm * scale, 0.0F, 0.0F, 0.0F);
      return row;
    }

    // Action row (`+ new bar`): same geometry as an index row, never selected.
    std::unique_ptr<Button>
    makeActionButton(std::string text, bool indent, float scale, std::function<void()> onClick) {
      return makeIndexButton(
          IndexItem{
              .label = "+ " + lowerLabel(text),
              .dim = true,
              .indent = indent,
              .onClick = std::move(onClick),
          },
          scale
      );
    }

  } // namespace

  bool isCentralSettingsPage(std::string_view id) { return std::ranges::contains(kCentralPages, id); }

  std::string_view centralSettingsSection(std::string_view id) { return id == "look" ? "appearance" : id; }

  std::string settingsPageScopeKey(
      std::string_view selectedSection, std::string_view selectedBarName, std::string_view selectedMonitorOverride
  ) {
    if (selectedSection == "dock") {
      std::string key = "dock";
      if (!selectedMonitorOverride.empty()) {
        key += ":monitor:" + std::string(selectedMonitorOverride);
      }
      return key;
    }
    if (selectedSection != "bar") {
      return std::string(selectedSection);
    }
    std::string key = "bar:" + std::string(selectedBarName);
    if (!selectedMonitorOverride.empty()) {
      key += ":monitor:" + std::string(selectedMonitorOverride);
    }
    return key;
  }

  std::string_view settingsIndexGroupName(std::string_view sectionId) {
    if (isCentralSettingsPage(sectionId)) {
      return "terminus";
    }
    if (sectionId == "bar") {
      return "bar";
    }
    const auto section = settingsSectionFromId(sectionId);
    if (!section.has_value()) {
      return "shell";
    }
    if (std::ranges::contains(kBarGroup, *section)) {
      return "bar";
    }
    if (std::ranges::contains(kSystemGroup, *section)) {
      return "system";
    }
    return "shell";
  }

  std::string settingsIndexLabel(std::string_view sectionId) {
    if (isCentralSettingsPage(sectionId)) {
      return std::string(sectionId);
    }
    const auto section = settingsSectionFromId(sectionId);
    if (!section.has_value()) {
      return std::string(sectionId);
    }
    if (*section == SettingsSection::Appearance) {
      return "theme";
    }
    return lowerLabel(i18n::tr(settingsSectionLabelKey(*section)));
  }

  std::unique_ptr<Flex> buildSettingsSidebar(SettingsSidebarContext ctx) {
    const Config& cfg = ctx.config;
    std::vector<std::string> existingBarNames = ctx.availableBars;
    const std::string nextBarName = nextAvailableBarName(existingBarNames);

    auto* scroll = &ctx.contentScrollState;
    auto* selectedSection = &ctx.selectedSection;
    auto* selectedBarName = &ctx.selectedBarName;
    auto* selectedMonitorOverride = &ctx.selectedMonitorOverride;
    auto* creatingBarName = &ctx.creatingBarName;

    const auto clearTransientState = std::move(ctx.clearTransientState);
    const auto clearSearchQuery = std::move(ctx.clearSearchQuery);
    const auto requestRebuild = std::move(ctx.requestRebuild);
    const auto createBar = std::move(ctx.createBar);
    const auto openMonitorOverrideCreate = std::move(ctx.openMonitorOverrideCreate);
    const float scale = ctx.scale;
    const bool searchActive = ctx.globalSearchActive;
    const bool showActiveTab = !searchActive;
    const auto& counts = ctx.overrideCounts;

    const auto countFor = [&counts](const std::string& key) {
      const auto it = counts.find(key);
      return it == counts.end() ? 0 : it->second;
    };

    // Single navigation entry point: every index row selects (section, bar, monitor override).
    const auto navigate = [selectedSection, selectedBarName, selectedMonitorOverride, scroll, searchActive,
                           clearTransientState, clearSearchQuery,
                           requestRebuild](const std::string& section, const std::string& bar,
                                           const std::string& monitor) {
      if (searchActive
          || *selectedSection != section
          || (section == "bar" && *selectedBarName != bar)
          || *selectedMonitorOverride != monitor) {
        scroll->offset = 0.0F;
      }
      *selectedSection = section;
      if (section == "bar") {
        *selectedBarName = bar;
      }
      *selectedMonitorOverride = monitor;
      clearSearchQuery();
      clearTransientState();
      requestRebuild();
    };

    auto sidebarScroll = ui::scrollView({
        .out = ctx.outScroll,
        .state = &ctx.sidebarScrollState,
        .contentScale = scale,
        .scrollbarVisible = true,
        .viewportPaddingH = 0.0F,
        .viewportPaddingV = 0.0F,
        .flexGrow = 1.0F,
        .configure = [](ScrollView& scrollView) {
          scrollView.clearFill();
          scrollView.clearBorder();
        },
    });

    auto sidebarNav = std::make_unique<RovingListNavHost>(RovingListNavController::Options{
        .axis = RovingListNavAxis::Vertical,
        .mode = RovingListNavMode::FollowFocus,
        .keepItemsInTabOrder = false,
        .scrollIntoView = std::move(ctx.scrollSidebarNodeIntoView),
        .syncIndexFromSelection = {},
    });
    sidebarNav->setTabFocusKey("settings.sidebar");
    sidebarNav->setGap(1.0F * scale);
    sidebarNav->setPadding(kSidebarPadding * scale);
    RovingListNavHost* nav = sidebarNav.get();

    std::size_t groupNumber = 0;
    const auto addGroupHeading = [&](std::string_view name) {
      nav->addChild(makeHeading(++groupNumber, name, scale));
    };

    const auto sectionPresent = [&ctx](SettingsSection section) {
      return std::ranges::contains(ctx.sections, section);
    };

    // Adds a plain section row for `section`.
    const auto addSectionRow = [&](SettingsSection section) {
      const std::string sectionId(settingsSectionId(section));
      const bool selected = showActiveTab && sectionId == *selectedSection;
      const auto onClick = [navigate, sectionId]() { navigate(sectionId, {}, {}); };
      addNavButton(
          *nav,
          makeIndexButton(
              IndexItem{
                  .label = settingsIndexLabel(sectionId),
                  .count = countFor(settingsPageScopeKey(sectionId, {}, {})),
                  .selected = selected,
                  .onClick = onClick,
              },
              scale
          ),
          onClick
      );
    };

    // ── terminus: the desktop's central pages ─────────────────────────────
    addGroupHeading("terminus");
    for (const std::string_view page : kCentralPages) {
      const std::string id(page);
      const auto onClick = [navigate, id]() { navigate(id, {}, {}); };
      addNavButton(
          *nav,
          makeIndexButton(
              IndexItem{
                  .label = id,
                  .count = countFor(id),
                  .selected = showActiveTab && *selectedSection == id,
                  .onClick = onClick,
              },
              scale
          ),
          onClick
      );
    }

    // ── bar ───────────────────────────────────────────────────────────────
    addGroupHeading("bar");
    for (const auto& barName : ctx.availableBars) {
      const bool barSelected =
          showActiveTab && *selectedSection == "bar" && *selectedBarName == barName && selectedMonitorOverride->empty();
      const auto onBarClick = [navigate, barName]() { navigate("bar", barName, {}); };
      addNavButton(
          *nav,
          makeIndexButton(
              IndexItem{
                  .label = lowerLabel(i18n::tr("settings.entities.bar.label", "name", barName)),
                  .count = countFor(settingsPageScopeKey("bar", barName, {})),
                  .selected = barSelected,
                  .onClick = onBarClick,
              },
              scale
          ),
          onBarClick
      );

      const auto* bar = settings::findBar(cfg, barName);
      if (bar == nullptr) {
        continue;
      }

      for (const auto& ovr : bar->monitorOverrides) {
        const bool ovrSelected = showActiveTab
            && *selectedSection == "bar"
            && *selectedBarName == barName
            && *selectedMonitorOverride == ovr.match;
        const std::string match = ovr.match;
        const auto onMonitorClick = [navigate, barName, match]() { navigate("bar", barName, match); };
        addNavButton(
            *nav,
            makeIndexButton(
                IndexItem{
                    .label = lowerLabel(i18n::tr("settings.entities.monitor-override.label", "name", ovr.match)),
                    .count = countFor(settingsPageScopeKey("bar", barName, match)),
                    .selected = ovrSelected,
                    .indent = true,
                    .onClick = onMonitorClick,
                },
                scale
            ),
            onMonitorClick
        );
      }

      if (*selectedSection != "bar" || *selectedBarName != barName) {
        continue;
      }

      // Opens the create flow in a modal (wide enough for the output picker) rather than expanding inline.
      const auto onNewMonitorClick = [openMonitorOverrideCreate, barName, clearTransientState]() {
        clearTransientState();
        if (openMonitorOverrideCreate) {
          openMonitorOverrideCreate(barName);
        }
      };
      addNavButton(
          *nav, makeActionButton(i18n::tr("settings.entities.monitor-override.new"), true, scale, onNewMonitorClick),
          onNewMonitorClick
      );
    }

    const auto onNewBarClick = [creatingBarName, nextBarName, clearTransientState, requestRebuild]() {
      clearTransientState();
      *creatingBarName = nextBarName;
      requestRebuild();
    };
    addNavButton(
        *nav, makeActionButton(i18n::tr("settings.entities.bar.new"), false, scale, onNewBarClick), onNewBarClick
    );

    if (!creatingBarName->empty()) {
      auto createPanel = ui::column({
          .align = FlexAlign::Stretch,
          .gap = Style::spaceXs * scale,
          .configure = [scale](Flex& panel) { panel.setPadding(0.0F, Style::spaceXs * scale); },
      });

      Input* inputPtr = nullptr;
      auto input = ui::input({
          .out = &inputPtr,
          .value = *creatingBarName,
          .placeholder = i18n::tr("settings.entities.bar.id-placeholder"),
          .fontSize = Style::fontSizeCaption * scale,
          .controlHeight = Style::controlHeightSm * scale,
          .horizontalPadding = Style::spaceXs * scale,
          .width = 120.0F * scale,
          .height = Style::controlHeightSm * scale,
      });

      auto doCreate = [existingBarNames, createBar, inputPtr](std::string rawName) {
        const std::string name = normalizedConfigId(rawName);
        if (!isValidConfigId(name) || barNameExists(existingBarNames, name)) {
          inputPtr->setInvalid(true);
          return;
        }
        createBar(name);
      };
      input->setOnSubmit([doCreate](const std::string& text) mutable { doCreate(text); });
      input->setOnChange([inputPtr](const std::string& /*text*/) { inputPtr->setInvalid(false); });

      createPanel->addChild(std::move(input));
      createPanel->addChild(
          ui::row(
              {
                  .align = FlexAlign::Center,
                  .gap = Style::spaceXs * scale,
              },
              makeOutlineTile({
                  .text = lowerLabel(i18n::tr("settings.entities.bar.create")),
                  .scale = scale,
                  .onClick = [doCreate, inputPtr]() mutable { doCreate(inputPtr->value()); },
              }),
              makeOutlineTile({
                  .text = "x",
                  .scale = scale,
                  .dim = true,
                  .onClick = [creatingBarName, requestRebuild]() {
                    creatingBarName->clear();
                    requestRebuild();
                  },
              })
          )
      );
      nav->addChild(std::move(createPanel));
    }

    const auto addSectionGroup = [&](std::string_view name, std::span<const SettingsSection> order) {
      std::vector<SettingsSection> rows;
      for (const auto section : order) {
        if (sectionPresent(section)) {
          rows.push_back(section);
        }
      }
      if (rows.empty()) {
        return;
      }
      addGroupHeading(name);
      for (const auto section : rows) {
        addSectionRow(section);
      }
    };

    // The bar group continues with the shell's own panels; they share the "bar" heading above.
    for (const auto section : kBarGroup) {
      if (sectionPresent(section)) {
        addSectionRow(section);
      }
    }
    addSectionGroup("shell", kShellGroup);
    addSectionGroup("system", kSystemGroup);

    auto* sidebar = sidebarScroll->content();
    sidebar->setDirection(FlexDirection::Vertical);
    sidebar->setAlign(FlexAlign::Stretch);
    sidebar->addChild(std::move(sidebarNav));

    if (ctx.outNav != nullptr) {
      *ctx.outNav = nav;
    }

    auto column = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
        .minWidth = kSidebarWidth * scale,
        .fillHeight = true,
        .width = kSidebarWidth * scale,
    });
    if (ctx.makeSearchField) {
      column->addChild(ctx.makeSearchField());
    }
    column->addChild(std::move(sidebarScroll));
    return column;
  }

} // namespace settings
