#pragma once

#include "ui/controls/flex.h"

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace process {
  struct RunResult;
}

// Front end for the desktop's central settings (config/settings.toml), which are owned by the
// `terminus-settings` CLI. The class only shells out to that CLI (never on the UI thread), keeps the
// schema and the current values, and builds the "appearance", "screensaver" and "login" pages.
// buildPage() is the only place that creates nodes; no node pointers are cached between calls.
class TerminusCentralSettings {
public:
  TerminusCentralSettings();
  ~TerminusCentralSettings();

  TerminusCentralSettings(const TerminusCentralSettings&) = delete;
  TerminusCentralSettings& operator=(const TerminusCentralSettings&) = delete;

  // Re-reads the values (and the schema, once). Overlapping calls are coalesced.
  void refresh();
  // Invoked on the main loop whenever the data or the status line changed.
  void setChangedCallback(std::function<void()> cb);
  [[nodiscard]] bool ready() const;
  [[nodiscard]] std::unique_ptr<Flex> buildPage(std::string_view section, float scale);
  // Number of keys of `section` whose current value differs from the schema default.
  [[nodiscard]] int changedCount(std::string_view section) const;

private:
  struct Entry {
    std::string key;
    std::string section;
    std::string name;
    std::string kind;
    std::string label;
    nlohmann::json def;
    double min = 0.0;
    double max = 1.0;
    double step = 0.0;
    std::vector<std::string> choices;
  };

  struct Write {
    std::string key;
    bool reset = false;
    nlohmann::json value;
  };

  void run(std::vector<std::string> args, long timeoutMs, std::function<void(process::RunResult)> done);
  void notify();

  [[nodiscard]] static std::optional<Entry> parseEntry(const nlohmann::json& item);
  void startSchemaRead();
  void startGetRead();
  void failRead(const std::string& message);
  void finishRead();

  void commit(const std::string& key, nlohmann::json value, bool reset = false);
  void pumpWrites();
  void onWriteDone(bool ok, const std::string& message);
  void runLoginApply();

  [[nodiscard]] const Entry* findEntry(std::string_view key) const;
  [[nodiscard]] const nlohmann::json* currentValue(const Entry& entry) const;
  void storeValue(const Entry& entry, const nlohmann::json& value);
  [[nodiscard]] bool differsFromDefault(const Entry& entry) const;

  void addAppearanceBoxes(Flex& page, const std::vector<const Entry*>& entries, float scale);
  [[nodiscard]] std::unique_ptr<Flex>
  buildBox(std::string_view numeral, std::string_view title, const std::vector<const Entry*>& entries, float scale);
  [[nodiscard]] std::unique_ptr<Flex> buildRow(const Entry& entry, float scale);
  void buildControl(Flex& cell, const Entry& entry, const nlohmann::json& current, float scale);
  void buildToggle(Flex& cell, const Entry& entry, const nlohmann::json& current, float scale);
  void buildNumberControl(Flex& cell, const Entry& entry, const nlohmann::json& current, float scale);
  void buildSelectControl(Flex& cell, const Entry& entry, const nlohmann::json& current, float scale);
  void buildColorControl(Flex& cell, const Entry& entry, const nlohmann::json& current, float scale);

  std::shared_ptr<void> m_alive;
  std::shared_ptr<std::atomic<bool>> m_cancel;
  std::function<void()> m_changed;

  std::vector<Entry> m_entries;
  bool m_schemaLoaded = false;
  nlohmann::json m_values;
  bool m_valuesLoaded = false;
  std::string m_error;
  std::string m_status;

  bool m_readBusy = false;
  bool m_readQueued = false;
  bool m_rereadNeeded = false;

  std::deque<Write> m_queue;
  std::optional<Write> m_inFlight;
  std::map<std::string, nlohmann::json> m_confirmed;
  bool m_loginBusy = false;
};
