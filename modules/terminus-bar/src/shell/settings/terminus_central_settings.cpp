// Settings-window pages backed by the `terminus-settings` CLI: schema + values reads, queued writes,
// and the node builders for each row kind. Every CLI call is asynchronous (see run()).

#include "shell/settings/terminus_central_settings.h"

#include "core/deferred_call.h"
#include "core/process/process.h"
#include "shell/control_center/term_frame.h"
#include "ui/builders.h"
#include "ui/controls/box.h"
#include "ui/controls/button.h"
#include "ui/controls/input.h"
#include "ui/controls/label.h"
#include "ui/controls/select.h"
#include "ui/controls/slider.h"
#include "ui/palette.h"
#include "ui/style.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>

namespace {

  using json = nlohmann::json;

  constexpr std::string_view kCli = "terminus-settings";
  constexpr long kReadTimeoutMs = 15000;
  constexpr long kWriteTimeoutMs = 30000;
  constexpr long kLoginApplyTimeoutMs = 600000;
  constexpr std::size_t kControlChars = 38;
  constexpr std::size_t kResetChars = 7;
  constexpr std::size_t kValueChars = 6;
  constexpr std::size_t kMaxTileChoices = 4;
  constexpr float kRowHeight = 22.0F;
  constexpr float kCellInsetPx = 2.0F; // marker/reset cells: room for the tile border
  constexpr float kSwatchRatio = 0.7F; // swatch edge relative to the row height
  constexpr double kFloatEpsilon = 1e-6;
  constexpr double kNumberSnap = 1000.0; // numbers sent to the CLI keep three decimals

  struct Group {
    std::string_view numeral;
    std::string_view title;
    std::vector<std::string_view> keys;
  };

  const std::vector<Group>& appearanceGroups() {
    static const std::vector<Group> groups = {
        {"¹",
         "palette",
         {"appearance.background", "appearance.foreground", "appearance.muted", "appearance.alert", "appearance.accent",
          "appearance.active_border", "appearance.inactive_border"}},
        {"²",
         "glass",
         {"appearance.background_opacity", "appearance.glass_opacity", "appearance.chrome_opacity",
          "appearance.popover_opacity", "appearance.bar_opacity", "appearance.blur", "appearance.blur_strength",
          "appearance.brightness", "appearance.contrast"}},
        {"³",
         "windows",
         {"appearance.active_opacity", "appearance.inactive_opacity", "appearance.default_active_opacity",
          "appearance.default_inactive_opacity", "appearance.global_active_opacity",
          "appearance.global_inactive_opacity", "appearance.dim_inactive", "appearance.dim_strength"}},
        {"⁴", "type", {"appearance.ui_font"}},
    };
    return groups;
  }

  std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
  }

  std::string firstLine(const std::string& text) {
    std::size_t pos = 0;
    while (pos < text.size()) {
      std::size_t end = text.find('\n', pos);
      if (end == std::string::npos) {
        end = text.size();
      }
      std::string line = text.substr(pos, end - pos);
      const auto first = line.find_first_not_of(" \t\r");
      if (first != std::string::npos) {
        const auto last = line.find_last_not_of(" \t\r");
        return line.substr(first, last - first + 1);
      }
      pos = end + 1;
    }
    return {};
  }

  std::string failureMessage(const process::RunResult& result, std::string_view what) {
    if (result.timedOut) {
      return std::string(what) + " timed out";
    }
    std::string line = firstLine(result.err);
    if (line.empty()) {
      line = firstLine(result.out);
    }
    if (line.empty()) {
      line = std::format("{} failed (exit {})", what, result.exitCode);
    }
    return line;
  }

  bool validHex(const std::string& text) {
    if (text.size() != 7 || text[0] != '#') {
      return false;
    }
    return std::ranges::all_of(text.begin() + 1, text.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
  }

  Color hexColor(const std::string& text) {
    return rgbHex(static_cast<std::uint32_t>(std::stoul(text.substr(1), nullptr, 16)));
  }

  bool sameValue(const json& a, const json& b) {
    if (a.is_number() && b.is_number()) {
      return std::fabs(a.get<double>() - b.get<double>()) < kFloatEpsilon;
    }
    if (a.is_string() && b.is_string()) {
      return lower(a.get<std::string>()) == lower(b.get<std::string>());
    }
    return a == b;
  }

  double defaultStep(const std::string& kind) { return kind == "integer" ? 1.0 : 0.01; }

  std::string formatNumber(const std::string& kind, double value) {
    if (kind == "integer") {
      return std::format("{}", static_cast<long long>(std::llround(value)));
    }
    return std::format("{:.2f}", value);
  }

  // Snaps to the control's own precision so the CLI sees `0.42`, not `0.4200000001`.
  json numberValue(const std::string& kind, double value) {
    if (kind == "integer") {
      return static_cast<long long>(std::llround(value));
    }
    return std::round(value * kNumberSnap) / kNumberSnap;
  }

  // A dim, single-line terminal-font label.
  std::unique_ptr<Label> mutedLabel(std::string text, float fontSize, float flexGrow = 0.0F) {
    return ui::label({
        .text = std::move(text),
        .fontSize = fontSize,
        .fontFamily = std::string(term_frame::kFont),
        .color = term_frame::dimColor(),
        .maxLines = 1,
        .ellipsize = TextEllipsize::End,
        .flexGrow = flexGrow,
    });
  }

} // namespace

TerminusCentralSettings::TerminusCentralSettings()
    : m_alive(std::make_shared<int>(0)), m_cancel(std::make_shared<std::atomic<bool>>(false)) {}

TerminusCentralSettings::~TerminusCentralSettings() {
  // Expiring the guard makes every queued deferred callback a no-op; the flag stops running children.
  m_alive.reset();
  m_cancel->store(true);
}

void TerminusCentralSettings::setChangedCallback(std::function<void()> cb) { m_changed = std::move(cb); }

bool TerminusCentralSettings::ready() const { return m_schemaLoaded && m_valuesLoaded; }

// ---------------------------------------------------------------------------------------------
// Process plumbing
// ---------------------------------------------------------------------------------------------

void TerminusCentralSettings::run(
    std::vector<std::string> args, long timeoutMs, std::function<void(process::RunResult)> done
) {
  std::weak_ptr<void> alive = m_alive;
  process::RunCallbacks callbacks;
  // onExit runs on a worker thread: only hand the result to the main loop from there.
  callbacks.onExit = [alive, done](process::RunResult result) {
    DeferredCall::callLater([alive, done, result = std::move(result)]() mutable {
      if (!alive.expired()) {
        done(std::move(result));
      }
    });
  };
  process::RunOptions options;
  options.timeout = std::chrono::milliseconds(timeoutMs);
  options.cancel = m_cancel;
  const std::string program = args.empty() ? std::string() : args.front();
  if (!process::runAsync(args, std::move(callbacks), std::move(options))) {
    process::RunResult failed;
    failed.err = "cannot start " + program;
    DeferredCall::callLater([alive, done, failed = std::move(failed)]() mutable {
      if (!alive.expired()) {
        done(std::move(failed));
      }
    });
  }
}

void TerminusCentralSettings::notify() {
  std::weak_ptr<void> alive = m_alive;
  DeferredCall::callLater([this, alive]() {
    if (!alive.expired() && m_changed) {
      m_changed();
    }
  });
}

// ---------------------------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------------------------

void TerminusCentralSettings::refresh() {
  if (m_readBusy) {
    m_readQueued = true;
    return;
  }
  m_readBusy = true;
  m_readQueued = false;
  if (m_schemaLoaded) {
    startGetRead();
  } else {
    startSchemaRead();
  }
}

void TerminusCentralSettings::finishRead() {
  m_readBusy = false;
  notify();
  if (m_readQueued) {
    refresh();
  }
}

std::optional<TerminusCentralSettings::Entry> TerminusCentralSettings::parseEntry(const json& item) {
  if (!item.is_object() || !item.contains("key") || !item["key"].is_string()) {
    return std::nullopt;
  }
  Entry entry;
  entry.key = item["key"].get<std::string>();
  const auto dot = entry.key.find('.');
  if (dot == std::string::npos) {
    return std::nullopt;
  }
  entry.section = entry.key.substr(0, dot);
  entry.name = entry.key.substr(dot + 1);
  entry.kind = item.value("kind", std::string("string"));
  entry.label = item.value("label", entry.name);
  entry.def = item.contains("default") ? item["default"] : json();
  if (entry.kind == "number" || entry.kind == "integer") {
    entry.min = item.value("min", 0.0);
    entry.max = item.value("max", 1.0);
    entry.step = item.value("step", defaultStep(entry.kind));
  }
  if (item.contains("choices") && item["choices"].is_array()) {
    for (const auto& choice : item["choices"]) {
      if (choice.is_string()) {
        entry.choices.push_back(choice.get<std::string>());
      }
    }
  }
  return entry;
}

void TerminusCentralSettings::startSchemaRead() {
  run({std::string(kCli), "schema"}, kReadTimeoutMs, [this](process::RunResult result) {
    if (!result) {
      m_error = failureMessage(result, "terminus-settings schema");
      finishRead();
      return;
    }
    const json schema = json::parse(result.out, nullptr, false);
    if (schema.is_discarded() || !schema.is_array()) {
      m_error = "terminus-settings schema: unreadable output";
      finishRead();
      return;
    }
    std::vector<Entry> entries;
    for (const auto& item : schema) {
      if (auto entry = parseEntry(item)) {
        entries.push_back(std::move(*entry));
      }
    }
    m_entries = std::move(entries);
    m_schemaLoaded = true;
    m_error.clear();
    startGetRead();
  });
}

// Once values exist a failed re-read only shows in the status line; the page keeps its data.
void TerminusCentralSettings::failRead(const std::string& message) {
  (m_valuesLoaded ? m_status : m_error) = message;
  finishRead();
}

void TerminusCentralSettings::startGetRead() {
  run({std::string(kCli), "get"}, kReadTimeoutMs, [this](process::RunResult result) {
    // A write is pending or running: its optimistic values would be overwritten by this stale
    // snapshot. The write path re-reads once it drains.
    if (m_inFlight || !m_queue.empty()) {
      m_rereadNeeded = true;
      finishRead();
      return;
    }
    if (!result) {
      failRead(failureMessage(result, "terminus-settings get"));
      return;
    }
    json values = json::parse(result.out, nullptr, false);
    if (values.is_discarded() || !values.is_object()) {
      failRead("terminus-settings get: unreadable output");
      return;
    }
    m_values = std::move(values);
    m_valuesLoaded = true;
    m_error.clear();
    finishRead();
  });
}

// ---------------------------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------------------------

const TerminusCentralSettings::Entry* TerminusCentralSettings::findEntry(std::string_view key) const {
  const auto it = std::ranges::find_if(m_entries, [key](const Entry& entry) { return entry.key == key; });
  return it == m_entries.end() ? nullptr : &*it;
}

const json* TerminusCentralSettings::currentValue(const Entry& entry) const {
  if (!m_values.is_object()) {
    return nullptr;
  }
  const auto section = m_values.find(entry.section);
  if (section == m_values.end() || !section->is_object()) {
    return nullptr;
  }
  const auto value = section->find(entry.name);
  return value == section->end() ? nullptr : &*value;
}

void TerminusCentralSettings::storeValue(const Entry& entry, const json& value) {
  if (!m_values.is_object()) {
    m_values = json::object();
  }
  m_values[entry.section][entry.name] = value;
}

bool TerminusCentralSettings::differsFromDefault(const Entry& entry) const {
  const json* current = currentValue(entry);
  return current != nullptr && !sameValue(*current, entry.def);
}

int TerminusCentralSettings::changedCount(std::string_view section) const {
  if (!ready()) {
    return 0;
  }
  int count = 0;
  for (const Entry& entry : m_entries) {
    if (entry.section == section && differsFromDefault(entry)) {
      ++count;
    }
  }
  return count;
}

// ---------------------------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------------------------

void TerminusCentralSettings::commit(const std::string& key, json value, bool reset) {
  const Entry* entry = findEntry(key);
  if (entry == nullptr) {
    return;
  }
  const json* current = currentValue(*entry);
  const json target = reset ? entry->def : std::move(value);
  if (current != nullptr && sameValue(*current, target)) {
    return;
  }
  if (current != nullptr && !m_confirmed.contains(key)) {
    m_confirmed[key] = *current;
  }
  storeValue(*entry, target);
  // Coalesce: a queued (not yet running) write to the same key is superseded.
  std::erase_if(m_queue, [&key](const Write& pending) { return pending.key == key; });
  m_queue.push_back(Write{.key = key, .reset = reset, .value = target});
  m_status.clear();
  pumpWrites();
  notify();
}

void TerminusCentralSettings::pumpWrites() {
  if (m_inFlight) {
    return;
  }
  if (m_queue.empty()) {
    if (m_rereadNeeded) {
      m_rereadNeeded = false;
      refresh();
    }
    return;
  }
  m_inFlight = std::move(m_queue.front());
  m_queue.pop_front();
  std::vector<std::string> args = {std::string(kCli)};
  if (m_inFlight->reset) {
    args.push_back("reset");
    args.push_back(m_inFlight->key);
  } else {
    args.push_back("set");
    args.push_back(m_inFlight->key);
    args.push_back(m_inFlight->value.dump());
  }
  run(std::move(args), kWriteTimeoutMs, [this](process::RunResult result) {
    onWriteDone(static_cast<bool>(result), result ? std::string() : failureMessage(result, "terminus-settings"));
  });
}

void TerminusCentralSettings::onWriteDone(bool ok, const std::string& message) {
  const Write done = std::move(*m_inFlight);
  m_inFlight.reset();
  const Entry* entry = findEntry(done.key);
  const bool stillQueued =
      std::ranges::any_of(m_queue, [&done](const Write& pending) { return pending.key == done.key; });
  if (ok) {
    if (stillQueued) {
      m_confirmed[done.key] = done.value;
    } else {
      m_confirmed.erase(done.key);
    }
  } else {
    const auto confirmed = m_confirmed.find(done.key);
    if (confirmed != m_confirmed.end()) {
      if (entry != nullptr) {
        storeValue(*entry, confirmed->second);
      }
      m_confirmed.erase(confirmed);
    }
    std::erase_if(m_queue, [&done](const Write& pending) { return pending.key == done.key; });
    m_status = message;
  }
  m_rereadNeeded = true;
  pumpWrites();
  notify();
}

void TerminusCentralSettings::runLoginApply() {
  if (m_loginBusy) {
    return;
  }
  m_loginBusy = true;
  m_status = "login-apply: waiting for authentication...";
  notify();
  run({std::string(kCli), "login-apply"}, kLoginApplyTimeoutMs, [this](process::RunResult result) {
    m_loginBusy = false;
    if (result) {
      m_status = "login screen updated";
    } else {
      m_status = failureMessage(result, "login-apply"); // names the command itself when it timed out
      if (m_status.find("login-apply") == std::string::npos) {
        m_status = std::format("login-apply (exit {}): {}", result.exitCode, m_status);
      }
    }
    notify();
  });
}

// ---------------------------------------------------------------------------------------------
// Page building (the only place nodes are created)
// ---------------------------------------------------------------------------------------------

std::unique_ptr<Flex> TerminusCentralSettings::buildPage(std::string_view section, float scale) {
  const float fontSize = Style::fontSizeCaption * scale;
  auto page = ui::column({
      .align = FlexAlign::Stretch,
      .gap = Style::spaceSm * scale,
      .fillWidth = true,
  });

  const auto addMuted = [&](std::string text) { page->addChild(mutedLabel(std::move(text), fontSize)); };

  if (!m_schemaLoaded) {
    addMuted(m_error.empty() ? "loading central settings..." : m_error);
    return page;
  }
  if (!m_error.empty() && !m_valuesLoaded) {
    addMuted(m_error);
    return page;
  }
  if (!m_valuesLoaded) {
    addMuted("loading central settings...");
    return page;
  }
  if (!m_status.empty()) {
    addMuted(std::string(term_frame::kMarkerNeutral) + " " + m_status);
  }

  std::vector<const Entry*> sectionEntries;
  for (const Entry& entry : m_entries) {
    if (entry.section == section) {
      sectionEntries.push_back(&entry);
    }
  }
  if (sectionEntries.empty()) {
    addMuted("no settings in this section");
    return page;
  }

  if (section == "appearance") {
    addAppearanceBoxes(*page, sectionEntries, scale);
    return page;
  }

  auto box = buildBox("¹", std::string(section), sectionEntries, scale);
  if (section == "login") {
    box->addChild(mutedLabel("staged: applies to the login screen after \"apply to login\"", fontSize));
    auto actions = ui::row({.align = FlexAlign::Center, .gap = Style::spaceSm * scale, .fillWidth = true});
    actions->addChild(
        term_frame::tile(
            {
                .label = "apply to login",
                .tooltip = "write the staged login settings (asks for authentication)",
                .center = true,
                .onClick = [this]() { runLoginApply(); },
            },
            scale
        )
    );
    box->addChild(std::move(actions));
  }
  page->addChild(std::move(box));
  return page;
}

// Fixed groups in reading order; keys the schema adds later land in a trailing "other" box.
void TerminusCentralSettings::addAppearanceBoxes(Flex& page, const std::vector<const Entry*>& entries, float scale) {
  std::vector<bool> used(entries.size(), false);
  for (const Group& group : appearanceGroups()) {
    std::vector<const Entry*> members;
    for (const std::string_view key : group.keys) {
      for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i]->key == key) {
          members.push_back(entries[i]);
          used[i] = true;
        }
      }
    }
    if (!members.empty()) {
      page.addChild(buildBox(group.numeral, group.title, members, scale));
    }
  }
  std::vector<const Entry*> rest;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (!used[i]) {
      rest.push_back(entries[i]);
    }
  }
  if (!rest.empty()) {
    page.addChild(buildBox("⁵", "other", rest, scale));
  }
}

std::unique_ptr<Flex> TerminusCentralSettings::buildBox(
    std::string_view numeral, std::string_view title, const std::vector<const Entry*>& entries, float scale
) {
  auto box = term_frame::box(numeral, title, scale);
  for (const Entry* entry : entries) {
    box->addChild(buildRow(*entry, scale));
  }
  return box;
}

std::unique_ptr<Flex> TerminusCentralSettings::buildRow(const Entry& entry, float scale) {
  const float fontSize = Style::fontSizeCaption * scale;
  const float gap = Style::spaceSm * scale;
  const float controlWidth = term_frame::monoWidth(kControlChars, fontSize);
  const float resetWidth = term_frame::monoWidth(kResetChars, fontSize) + 2.0F * gap + kCellInsetPx;
  const float dotWidth = term_frame::monoWidth(1, fontSize) + kCellInsetPx;

  const json* current = currentValue(entry);
  const json value = current != nullptr ? *current : entry.def;
  const bool changed = differsFromDefault(entry);

  auto row = ui::row({.align = FlexAlign::Center, .gap = gap, .fillWidth = true});
  row->addChild(
      ui::label({
          .text = changed ? std::string(term_frame::kMarkerOn) : std::string(" "),
          .fontSize = fontSize,
          .fontFamily = std::string(term_frame::kFont),
          .color = colorSpecFromRole(ColorRole::Primary),
          .minWidth = dotWidth,
          .maxWidth = dotWidth,
          .maxLines = 1,
      })
  );
  row->addChild(
      ui::label({
          .text = lower(entry.label),
          .fontSize = fontSize,
          .fontFamily = std::string(term_frame::kFont),
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .maxLines = 1,
          .ellipsize = TextEllipsize::End,
          .flexGrow = 1.0F,
      })
  );

  auto cell = ui::row({
      .align = FlexAlign::Center,
      .gap = gap,
      .minWidth = controlWidth,
      .maxWidth = controlWidth,
  });
  buildControl(*cell, entry, value, scale);
  row->addChild(std::move(cell));

  auto resetCell = ui::row({
      .align = FlexAlign::Center,
      .minWidth = resetWidth,
      .maxWidth = resetWidth,
  });
  if (changed) {
    auto reset = term_frame::tile(
        {
            .label = "reset",
            .tooltip = "restore the default",
            .dimWhenOff = true,
            .center = true,
            .onClick = [this, key = entry.key]() { commit(key, json(), true); },
        },
        scale
    );
    reset->setFlexGrow(1.0F);
    resetCell->addChild(std::move(reset));
  }
  row->addChild(std::move(resetCell));
  return row;
}

void TerminusCentralSettings::buildControl(Flex& cell, const Entry& entry, const json& current, float scale) {
  if (entry.kind == "boolean") {
    buildToggle(cell, entry, current, scale);
  } else if (entry.kind == "number" || entry.kind == "integer") {
    buildNumberControl(cell, entry, current, scale);
  } else if (entry.kind == "select") {
    buildSelectControl(cell, entry, current, scale);
  } else if (entry.kind == "color") {
    buildColorControl(cell, entry, current, scale);
  } else {
    // Unknown kind: show the raw value, read-only.
    cell.addChild(mutedLabel(current.dump(), Style::fontSizeCaption * scale, 1.0F));
  }
}

void TerminusCentralSettings::buildToggle(Flex& cell, const Entry& entry, const json& current, float scale) {
  const bool on = current.is_boolean() && current.get<bool>();
  Label* marker = nullptr;
  auto toggle = term_frame::tile(
      {
          .label = on ? "on" : "off",
          .marker = &marker,
          .toggle = true,
          .on = on,
          .dimWhenOff = true,
          .onClick = [this, key = entry.key, on]() { commit(key, json(!on)); },
      },
      scale
  );
  toggle->setFlexGrow(1.0F);
  cell.addChild(std::move(toggle));
}

void TerminusCentralSettings::buildNumberControl(Flex& cell, const Entry& entry, const json& current, float scale) {
  const float fontSize = Style::fontSizeCaption * scale;
  struct Refs {
    Slider* slider = nullptr;
    Label* value = nullptr;
  };
  auto refs = std::make_shared<Refs>();
  const double number = current.is_number() ? current.get<double>() : entry.min;
  cell.addChild(
      ui::slider({
          .out = &refs->slider,
          .minValue = entry.min,
          .maxValue = entry.max,
          .step = entry.step > 0.0 ? entry.step : defaultStep(entry.kind),
          .value = number,
          .trackHeight = Style::sliderTrackHeight * scale,
          .controlHeight = kRowHeight * scale,
          .flexGrow = 1.0F,
          .onValueChanged =
              [refs, kind = entry.kind](double v) {
                if (refs->value != nullptr) {
                  refs->value->setText(formatNumber(kind, v));
                }
              },
          .onDragEnd =
              [this, refs, key = entry.key, kind = entry.kind]() {
                if (refs->slider != nullptr) {
                  commit(key, numberValue(kind, refs->slider->value()));
                }
              },
      })
  );
  const float valueWidth = term_frame::monoWidth(kValueChars, fontSize);
  cell.addChild(
      ui::label({
          .out = &refs->value,
          .text = formatNumber(entry.kind, number),
          .fontSize = fontSize,
          .fontFamily = std::string(term_frame::kFont),
          .color = colorSpecFromRole(ColorRole::OnSurface),
          .minWidth = valueWidth,
          .maxWidth = valueWidth,
          .maxLines = 1,
          .textAlign = TextAlign::End,
      })
  );
}

void TerminusCentralSettings::buildSelectControl(Flex& cell, const Entry& entry, const json& current, float scale) {
  const std::string selected = current.is_string() ? current.get<std::string>() : std::string();
  if (entry.choices.size() <= kMaxTileChoices) {
    for (const std::string& choice : entry.choices) {
      auto option = term_frame::tile(
          {
              .label = choice,
              .on = choice == selected,
              .dimWhenOff = true,
              .center = true,
              .onClick = [this, key = entry.key, choice]() { commit(key, json(choice)); },
          },
          scale
      );
      option->setFlexGrow(1.0F);
      cell.addChild(std::move(option));
    }
    return;
  }
  std::optional<std::size_t> index;
  for (std::size_t i = 0; i < entry.choices.size(); ++i) {
    if (entry.choices[i] == selected) {
      index = i;
    }
  }
  cell.addChild(
      ui::select({
          .options = entry.choices,
          .selectedIndex = index,
          .fontSize = Style::fontSizeCaption * scale,
          .controlHeight = kRowHeight * scale,
          .horizontalPadding = Style::spaceSm * scale,
          .flexGrow = 1.0F,
          .onSelectionChanged = [this, key = entry.key](std::size_t /*index*/, std::string_view text) {
            commit(key, json(std::string(text)));
          },
      })
  );
}

void TerminusCentralSettings::buildColorControl(Flex& cell, const Entry& entry, const json& current, float scale) {
  struct Refs {
    Input* input = nullptr;
    Box* swatch = nullptr;
  };
  auto refs = std::make_shared<Refs>();
  const float rowHeight = kRowHeight * scale;
  const std::string text = current.is_string() ? lower(current.get<std::string>()) : std::string();
  const float swatchSize = rowHeight * kSwatchRatio;
  cell.addChild(
      ui::box({
          .out = &refs->swatch,
          .fill = validHex(text) ? fixedColorSpec(hexColor(text)) : clearColorSpec(),
          .border = term_frame::lineColor(),
          .borderWidth = Style::borderWidth,
          .radius = 0.0F,
          .width = swatchSize,
          .height = swatchSize,
      })
  );
  cell.addChild(
      ui::input({
          .out = &refs->input,
          .value = text,
          .placeholder = "#rrggbb",
          .fontSize = Style::fontSizeCaption * scale,
          .controlHeight = rowHeight,
          .horizontalPadding = Style::spaceSm * scale,
          .flexGrow = 1.0F,
          .onChange =
              [refs](const std::string& typed) {
                const bool valid = validHex(typed);
                if (refs->input != nullptr) {
                  refs->input->setInvalid(!valid);
                }
                if (valid && refs->swatch != nullptr) {
                  refs->swatch->setFill(hexColor(typed));
                }
              },
          .onSubmit =
              [this, refs, key = entry.key](const std::string& typed) {
                if (validHex(typed)) {
                  commit(key, json(lower(typed)));
                } else if (refs->input != nullptr) {
                  refs->input->setInvalid(true);
                }
              },
          .submitOnFocusLoss = true,
      })
  );
}
