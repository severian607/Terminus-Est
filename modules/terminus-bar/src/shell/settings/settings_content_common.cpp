#include "shell/settings/settings_content_common.h"

#include "config/config_service.h"
#include "config/config_types.h"
#include "i18n/i18n.h"
#include "shell/control_center/term_frame.h"
#include "shell/settings/settings_content.h"
#include "ui/builders.h"
#include "ui/palette.h"
#include "ui/style.h"
#include "util/string_utils.h"

#include <array>
#include <cmath>
#include <format>
#include <utility>

namespace settings {

  namespace {

    constexpr std::array<std::string_view, 10> kSuperscriptDigits{"⁰", "¹", "²", "³", "⁴", "⁵", "⁶", "⁷", "⁸", "⁹"};

    // Outline tile palette with the error role as the lit colour, for destructive confirmations.
    Button::ButtonPalette alertTilePalette() {
      using State = Button::ButtonStateColors;
      auto palette = term_frame::tilePalette(false);
      const ColorSpec error = colorSpecFromRole(ColorRole::Error);
      palette.normal =
          State{.bg = clearColorSpec(), .border = colorSpecFromRole(ColorRole::Error, 0.6F), .label = error};
      palette.hover = State{
          .bg = colorSpecFromRole(ColorRole::Error, 0.12F),
          .border = colorSpecFromRole(ColorRole::Error, 0.9F),
          .label = error,
      };
      palette.pressed = State{
          .bg = colorSpecFromRole(ColorRole::Error, 0.22F),
          .border = error,
          .label = error,
      };
      palette.selected = palette.hover;
      return palette;
    }

  } // namespace

  std::unique_ptr<Button> makeOutlineTile(OutlineTileProps props) {
    const float scale = props.scale;
    return ui::button({
        .text = std::move(props.text),
        .fontSize = Style::fontSizeCaption * scale,
        .enabled = props.enabled,
        .selected = props.selected,
        .contentAlign = ButtonContentAlign::Center,
        .variant = ButtonVariant::Outline,
        .customPalette = props.alert ? alertTilePalette() : term_frame::tilePalette(props.dim),
        .surfaceOpacity = 1.0F,
        .minWidth = props.minWidth > 0.0F ? std::optional<float>{props.minWidth} : std::nullopt,
        .minHeight = 0.0F,
        .paddingV = Style::spaceXs * scale,
        .paddingH = Style::spaceSm * scale,
        .gap = Style::spaceSm * scale,
        .radius = 0.0F,
        .onClick = std::move(props.onClick),
        .configure =
            [](Button& control) {
              control.setDirection(FlexDirection::Horizontal);
              control.setAlign(FlexAlign::Center);
              if (control.label() != nullptr) {
                control.label()->setFontFamily(term_frame::kFont);
                control.label()->setMaxLines(1);
              }
            },
    });
  }

  std::unique_ptr<Button> makeMarkerToggle(
      bool checked, bool enabled, float scale, std::function<void(bool)> onChange, std::string label
  ) {
    const auto labelFor = [label](bool on) {
      return std::string(on ? term_frame::kMarkerOn : term_frame::kMarkerOff) + " "
          + (label.empty() ? std::string(on ? "on" : "off") : label);
    };
    const std::size_t widthChars = label.empty() ? 5 : label.size() + 2;
    auto state = std::make_shared<bool>(checked);
    auto self = std::make_shared<Button*>(nullptr);
    auto button = makeOutlineTile({
        .text = labelFor(checked),
        .scale = scale,
        .selected = checked,
        .dim = !checked,
        .enabled = enabled,
        .minWidth = term_frame::monoWidth(widthChars, Style::fontSizeCaption * scale) + 2.0F * Style::spaceSm * scale,
        .onClick = enabled
            ? std::function<void()>{[state, self, labelFor, onChange = std::move(onChange)]() {
                *state = !*state;
                if (*self != nullptr) {
                  (*self)->setText(labelFor(*state));
                  (*self)->setSelected(*state);
                }
                if (onChange) {
                  onChange(*state);
                }
              }}
            : std::function<void()>{},
    });
    *self = button.get();
    return button;
  }

  std::unique_ptr<Flex> makeTileGroup(
      const std::vector<std::string>& labels, std::optional<std::size_t> selectedIndex, float scale,
      std::function<void(std::size_t)> onChange, bool enabled
  ) {
    auto group = ui::row({
        .align = FlexAlign::Center,
        .wrap = labels.size() > 4 ? std::optional<bool>{true} : std::nullopt,
        .gap = Style::spaceXs * scale,
    });
    auto tiles = std::make_shared<std::vector<Button*>>();
    tiles->reserve(labels.size());
    for (std::size_t i = 0; i < labels.size(); ++i) {
      auto tile = makeOutlineTile({
          .text = labels[i],
          .scale = scale,
          .selected = selectedIndex.has_value() && *selectedIndex == i,
          .dim = !(selectedIndex.has_value() && *selectedIndex == i),
          .enabled = enabled,
          .onClick = enabled ? std::function<void()>{[tiles, i, onChange]() {
            for (std::size_t j = 0; j < tiles->size(); ++j) {
              (*tiles)[j]->setSelected(j == i);
            }
            if (onChange) {
              onChange(i);
            }
          }}
                             : std::function<void()>{},
      });
      tiles->push_back(tile.get());
      group->addChild(std::move(tile));
    }
    return group;
  }

  std::string superscriptNumber(std::size_t number) {
    std::string digits = std::to_string(number);
    std::string out;
    for (const char c : digits) {
      out += kSuperscriptDigits[static_cast<std::size_t>(c - '0')];
    }
    return out;
  }

  std::string lowerLabel(std::string_view text) { return StringUtils::toLower(text); }

  bool isMonitorOverrideSettingPath(const std::vector<std::string>& path) {
    return path.size() >= 5 && path[0] == "bar" && path[2] == "monitor";
  }

  bool monitorOverrideHasExplicitValue(const Config& cfg, const std::vector<std::string>& path) {
    if (!isMonitorOverrideSettingPath(path)) {
      return false;
    }

    const auto* bar = findBar(cfg, path[1]);
    if (bar == nullptr) {
      return false;
    }

    const auto* override = findMonitorOverride(*bar, path[3]);
    if (override == nullptr) {
      return false;
    }

    const std::string_view key = path.back();
    if (key == "enabled") {
      return override->enabled.has_value();
    }
    if (key == "auto_hide") {
      return override->autoHide.has_value();
    }
    if (key == "auto_hide_delay_ms") {
      return override->autoHideDelayMs.has_value();
    }
    if (key == "smart_auto_hide") {
      return override->smartAutoHide.has_value();
    }
    if (key == "show_on_workspace_switch") {
      return override->showOnWorkspaceSwitch.has_value();
    }
    if (key == "reserve_space") {
      return override->reserveSpace.has_value();
    }
    if (key == "thickness") {
      return override->thickness.has_value();
    }
    if (key == "scale") {
      return override->scale.has_value();
    }
    if (key == "margin_ends") {
      return override->marginEnds.has_value();
    }
    if (key == "margin_edge") {
      return override->marginEdge.has_value();
    }
    if (key == "margin_opposite_edge") {
      return override->marginOppositeEdge.has_value();
    }
    if (key == "padding") {
      return override->padding.has_value();
    }
    if (key == "radius") {
      return override->radius.has_value();
    }
    if (key == "radius_top_left") {
      return override->radiusTopLeft.has_value();
    }
    if (key == "radius_top_right") {
      return override->radiusTopRight.has_value();
    }
    if (key == "radius_bottom_left") {
      return override->radiusBottomLeft.has_value();
    }
    if (key == "radius_bottom_right") {
      return override->radiusBottomRight.has_value();
    }
    if (key == "concave_edge_corners") {
      return override->concaveEdgeCorners.has_value();
    }
    if (key == "background_opacity") {
      return override->backgroundOpacity.has_value();
    }
    if (key == "compositor_blur") {
      return override->compositorBlur.has_value();
    }
    if (key == "border") {
      return override->border.has_value();
    }
    if (key == "border_width") {
      return override->borderWidth.has_value();
    }
    if (key == "shadow") {
      return override->shadow.has_value();
    }
    if (key == "panel_overlap") {
      return override->panelOverlap.has_value();
    }
    if (key == "widget_spacing") {
      return override->widgetSpacing.has_value();
    }
    if (key == "capsule") {
      return override->widgetCapsuleDefault.has_value();
    }
    if (key == "capsule_fill") {
      return override->widgetCapsuleFill.has_value();
    }
    if (key == "capsule_border") {
      return override->widgetCapsuleBorderSpecified;
    }
    if (key == "capsule_border_width") {
      return override->widgetCapsuleBorderWidth.has_value();
    }
    if (key == "capsule_foreground") {
      return override->widgetCapsuleForeground.has_value();
    }
    if (key == "color") {
      return override->widgetColor.has_value();
    }
    if (key == "icon_color") {
      return override->widgetIconColor.has_value();
    }
    if (key == "capsule_padding") {
      return override->widgetCapsulePadding.has_value();
    }
    if (key == "capsule_radius") {
      return override->widgetCapsuleRadius.has_value();
    }
    if (key == "capsule_opacity") {
      return override->widgetCapsuleOpacity.has_value();
    }
    if (key == "start") {
      return override->startWidgets.has_value();
    }
    if (key == "center") {
      return override->centerWidgets.has_value();
    }
    if (key == "end") {
      return override->endWidgets.has_value();
    }
    if (path.size() >= 6 && path[4] == "dead_zone") {
      return override->deadZone.actions.has_value();
    }
    return false;
  }

  bool settingEntryHasEffectiveOverride(const SettingEntry& entry, const ConfigService& configService) {
    if (configService.hasEffectiveOverride(entry.path)) {
      return true;
    }
    if (const auto* range = std::get_if<RangeSliderSetting>(&entry.control)) {
      return configService.hasEffectiveOverride(range->highPath);
    }
    if (const auto* select = std::get_if<SelectSetting>(&entry.control)) {
      return !select->linkedPath.empty() && configService.hasEffectiveOverride(select->linkedPath);
    }
    return false;
  }

  std::unique_ptr<Label>
  makeLabel(std::string_view text, float fontSize, const ColorSpec& color, FontWeight fontWeight) {
    return ui::label({
        .text = std::string(text),
        .fontSize = fontSize,
        .fontWeight = fontWeight,
        .color = color,
    });
  }

  void updateSettingsStatusBanner(Flex& banner, Label& message, std::string_view text, bool error) {
    message.setText(std::string(text));
    message.setColor(colorSpecFromRole(error ? ColorRole::Error : ColorRole::Primary));
    banner.setBorder(colorSpecFromRole(error ? ColorRole::Error : ColorRole::Primary, 0.6F), Style::borderWidth);
    banner.setVisible(!text.empty());
  }

  std::unique_ptr<Flex> makeSettingsStatusBanner(SettingsStatusBannerProps props) {
    const float scale = props.scale;
    const bool error = props.error;
    auto banner = ui::row({
        .out = props.out,
        .align = FlexAlign::Center,
        .gap = Style::spaceSm * scale,
        .visible = !props.message.empty(),
        .configure = [scale, error](Flex& row) {
          row.setPadding(Style::spaceXs * scale, Style::spaceSm * scale);
          row.setRadius(0.0F);
          row.setBorder(colorSpecFromRole(error ? ColorRole::Error : ColorRole::Primary, 0.6F), Style::borderWidth);
        },
    });
    banner->addChild(
        ui::label({
            .out = props.messageOut,
            .text = std::move(props.message),
            .fontSize = Style::fontSizeCaption * scale,
            .fontFamily = std::string(term_frame::kFont),
            .color = colorSpecFromRole(error ? ColorRole::Error : ColorRole::Primary),
            .maxLines = 3,
            .flexGrow = 1.0F,
        })
    );
    if (props.onDismiss) {
      banner->addChild(makeOutlineTile({.text = "x", .scale = scale, .onClick = std::move(props.onDismiss)}));
    }
    return banner;
  }

  bool settingsSectionNeedsOfflineModeNotice(SettingsSection section) {
    switch (section) {
    case SettingsSection::Appearance:
    case SettingsSection::Templates:
    case SettingsSection::Launcher:
    case SettingsSection::Security:
    case SettingsSection::Location:
    case SettingsSection::Calendar:
      return true;
    case SettingsSection::Services:
    case SettingsSection::Wallpaper:
    case SettingsSection::Desktop:
    case SettingsSection::Dock:
    case SettingsSection::Panels:
    case SettingsSection::ControlCenter:
    case SettingsSection::Notifications:
    case SettingsSection::Osd:
    case SettingsSection::Screenshot:
    case SettingsSection::Shell:
    case SettingsSection::Keybinds:
    case SettingsSection::System:
    case SettingsSection::Power:
    case SettingsSection::Hooks:
    case SettingsSection::Niri:
    case SettingsSection::Umbriel:
    case SettingsSection::Bar:
    case SettingsSection::Plugins:
      return false;
    }
    return false;
  }

  std::string offlineModeNoticeMessage(SettingsSection section) {
    switch (section) {
    case SettingsSection::Appearance:
      return i18n::tr("settings.window.offline-mode-notice.appearance");
    case SettingsSection::Templates:
      return i18n::tr("settings.window.offline-mode-notice.templates");
    case SettingsSection::Launcher:
      return i18n::tr("settings.window.offline-mode-notice.launcher");
    case SettingsSection::Security:
      return i18n::tr("settings.window.offline-mode-notice.security");
    case SettingsSection::Calendar:
      return i18n::tr("settings.window.offline-mode-notice.calendar");
    case SettingsSection::Location:
      return i18n::tr("settings.window.offline-mode-notice.location");
    case SettingsSection::Services:
    case SettingsSection::Wallpaper:
    case SettingsSection::Desktop:
    case SettingsSection::Dock:
    case SettingsSection::Panels:
    case SettingsSection::ControlCenter:
    case SettingsSection::Notifications:
    case SettingsSection::Osd:
    case SettingsSection::Screenshot:
    case SettingsSection::Shell:
    case SettingsSection::Keybinds:
    case SettingsSection::System:
    case SettingsSection::Power:
    case SettingsSection::Hooks:
    case SettingsSection::Niri:
    case SettingsSection::Umbriel:
    case SettingsSection::Bar:
    case SettingsSection::Plugins:
      return {};
    }
    return {};
  }

  std::unique_ptr<Flex> makeOfflineModeNotice(float scale, std::string message, bool showDisableHint) {
    if (showDisableHint) {
      message += ' ';
      message += i18n::tr("settings.window.offline-mode-notice.disable-hint");
    }
    auto panel = ui::column({
        .align = FlexAlign::Stretch,
        .gap = Style::spaceXs * scale,
        .padding = Style::spaceSm * scale,
        .configure = [scale](Flex& column) {
          column.setRadius(0.0F);
          column.setBorder(colorSpecFromRole(ColorRole::Error, 0.5F), Style::borderWidth);
        },
    });
    panel->addChild(
        ui::label({
            .text = lowerLabel(i18n::tr("settings.window.offline-mode-notice.title")),
            .fontSize = Style::fontSizeCaption * scale,
            .fontFamily = std::string(term_frame::kFont),
            .color = colorSpecFromRole(ColorRole::Error),
        })
    );
    panel->addChild(
        ui::label({
            .text = std::move(message),
            .fontSize = Style::fontSizeCaption * scale,
            .fontFamily = std::string(term_frame::kFont),
            .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
            .maxLines = 4,
        })
    );
    return panel;
  }

  std::unique_ptr<Label> makeSettingSubtitleLabel(std::string_view text, float scale) {
    return ui::label({
        .text = std::string(text),
        .fontSize = Style::fontSizeCaption * scale,
        .fontFamily = std::string(term_frame::kFont),
        .color = colorSpecFromRole(ColorRole::OnSurfaceVariant),
        .maxLines = kSettingDescriptionMaxLines,
    });
  }

  std::optional<std::size_t> optionIndex(const std::vector<SelectOption>& options, std::string_view value) {
    for (std::size_t i = 0; i < options.size(); ++i) {
      if (options[i].value == value) {
        return i;
      }
    }
    return std::nullopt;
  }

  std::string optionLabel(const std::vector<SelectOption>& options, std::string_view value) {
    for (const auto& opt : options) {
      if (opt.value == value) {
        return opt.label;
      }
    }
    return std::string(value);
  }

  std::vector<std::string> optionLabels(const std::vector<SelectOption>& options) {
    std::vector<std::string> labels;
    labels.reserve(options.size());
    for (const auto& opt : options) {
      labels.push_back(opt.label);
    }
    return labels;
  }

  std::vector<ColorSwatchPreview> optionSwatchPreviews(const std::vector<SelectOption>& options) {
    std::vector<ColorSwatchPreview> previews;
    previews.reserve(options.size());
    for (const auto& opt : options) {
      previews.push_back(opt.preview);
    }
    return previews;
  }

  bool isBlankInput(std::string_view text) { return StringUtils::trim(text).empty(); }

  std::string formatSliderValue(double value, bool integerValue) {
    if (integerValue) {
      return std::format("{}", static_cast<int>(std::llround(value)));
    }
    return StringUtils::formatFixedDotDecimal(value, 2);
  }

  std::optional<double> parseDoubleInput(std::string_view text) { return StringUtils::parseDotDecimal<double>(text); }

  std::vector<SelectOption> sessionActionKindOptions() {
    return {
        {"lock", i18n::tr("session.actions.lock"), {}},
        {"logout", i18n::tr("session.actions.logout"), {}},
        {"suspend", i18n::tr("session.actions.suspend"), {}},
        {"lock_and_suspend", i18n::tr("session.actions.lock-and-suspend"), {}},
        {"reboot", i18n::tr("session.actions.reboot"), {}},
        {"shutdown", i18n::tr("session.actions.shutdown"), {}},
        {"command", i18n::tr("session.actions.custom"), {}},
    };
  }

  std::string
  sessionActionRowSummary(const std::vector<SelectOption>& kindOptions, const SessionPanelActionConfig& row) {
    if (row.label.has_value() && !row.label->empty()) {
      return *row.label;
    }
    return optionLabel(kindOptions, row.action);
  }

  std::string sessionActionDisplayTitle(const SessionPanelActionConfig& row) {
    return sessionActionRowSummary(sessionActionKindOptions(), row);
  }

  std::string sanitizedIdleBehaviorName(std::string_view text) {
    std::string out = StringUtils::trim(text);
    for (char& ch : out) {
      if (ch == '.' || ch == '[' || ch == ']') {
        ch = '-';
      }
    }
    return out;
  }

  std::string uniqueIdleBehaviorName(
      std::string base, const std::vector<IdleBehaviorConfig>& rows, std::optional<std::size_t> ignoreIndex
  ) {
    base = sanitizedIdleBehaviorName(base);
    if (base.empty()) {
      base = "idle-behavior";
    }

    std::unordered_set<std::string> names;
    for (std::size_t i = 0; i < rows.size(); ++i) {
      if (ignoreIndex.has_value() && i == *ignoreIndex) {
        continue;
      }
      if (!rows[i].name.empty()) {
        names.insert(rows[i].name);
      }
    }

    if (!names.contains(base)) {
      return base;
    }
    for (int suffix = 2; suffix < 10000; ++suffix) {
      std::string candidate = std::format("{}-{}", base, suffix);
      if (!names.contains(candidate)) {
        return candidate;
      }
    }
    return base;
  }

  void normalizeIdleBehaviorNames(std::vector<IdleBehaviorConfig>& rows) {
    std::vector<IdleBehaviorConfig> normalized;
    normalized.reserve(rows.size());
    for (auto& row : rows) {
      row.name = uniqueIdleBehaviorName(row.name, normalized);
      normalized.push_back(row);
    }
    rows = std::move(normalized);
  }

  std::string idleBehaviorRowSummary(const IdleBehaviorConfig& row) {
    IdleBehaviorConfig norm = row;
    normalizeIdleBehaviorAction(norm);

    const auto displayName = [&]() -> std::string {
      if (norm.action == "lock") {
        return i18n::tr("settings.idle.behavior.kind.lock");
      }
      if (norm.action == "screen_off") {
        return i18n::tr("settings.idle.behavior.kind.screen-off");
      }
      if (norm.action == "suspend") {
        return i18n::tr("settings.idle.behavior.kind.suspend");
      }
      if (norm.action == "lock_and_suspend") {
        return i18n::tr("settings.idle.behavior.kind.lock-and-suspend");
      }
      if (row.name.empty()) {
        return i18n::tr("settings.idle.behavior.unnamed");
      }
      return row.name;
    };

    const std::string name = displayName();
    if (name.empty()) {
      return i18n::tr("settings.idle.behavior.unnamed");
    }
    if (row.timeoutSeconds <= 0) {
      return i18n::tr("settings.idle.behavior.summary-disabled-timeout", "name", name);
    }
    return i18n::tr(
        "settings.idle.behavior.summary", "name", name, "seconds", StringUtils::formatDotDecimal(row.timeoutSeconds)
    );
  }

  std::string notificationFilterRowSummary(const NotificationFilterConfig& filter) {
    if (!filter.enabled) {
      if (!filter.match.empty()) {
        return i18n::tr("settings.notifications.filter.summary-disabled", "match", filter.match);
      }
      return i18n::tr("settings.notifications.filter.unnamed");
    }

    const std::string matchLabel =
        filter.match.empty() ? i18n::tr("settings.notifications.filter.unnamed") : filter.match;
    std::vector<std::string> parts;
    if (filter.showToast) {
      parts.emplace_back(i18n::tr("settings.notifications.filter.flag.toast"));
    }
    if (filter.saveHistory) {
      parts.emplace_back(i18n::tr("settings.notifications.filter.flag.history"));
    }
    if (filter.playSound) {
      parts.emplace_back(i18n::tr("settings.notifications.filter.flag.sound"));
    }
    if (filter.bypassDnd) {
      parts.emplace_back(i18n::tr("settings.notifications.filter.flag.dnd-bypass"));
    }
    if (filter.allowPermanent) {
      parts.emplace_back(i18n::tr("settings.notifications.filter.flag.permanent"));
    }
    if (!filter.allowedUrgencies.empty()) {
      std::vector<std::string> urgencyLabels;
      urgencyLabels.reserve(filter.allowedUrgencies.size());
      for (const auto& urgency : filter.allowedUrgencies) {
        urgencyLabels.emplace_back(i18n::tr("settings.options.notification-urgency." + urgency));
      }
      parts.emplace_back(StringUtils::join(urgencyLabels, ", "));
    }
    if (parts.empty()) {
      return i18n::tr("settings.notifications.filter.summary-blocked", "match", matchLabel);
    }
    return i18n::tr(
        "settings.notifications.filter.summary", "match", matchLabel, "flags", StringUtils::join(parts, ", ")
    );
  }

} // namespace settings
