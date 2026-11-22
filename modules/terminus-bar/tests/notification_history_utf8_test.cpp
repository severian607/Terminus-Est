#include "notification/notification_history_store.h"
#include "notification/notification_manager.h"
#include "util/string_utils.h"

#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>

namespace {

  Notification makeNotification(std::string body) {
    const auto now = Clock::now();
    const auto wallNow = WallClock::now();
    return Notification{
        .id = 314,
        .origin = NotificationOrigin::External,
        .appName = "test",
        .summary = "summary",
        .body = std::move(body),
        .timeout = 6000,
        .urgency = Urgency::Normal,
        .actions = {},
        .icon = std::nullopt,
        .imageData = std::nullopt,
        .category = std::nullopt,
        .desktopEntry = std::nullopt,
        .receivedTime = now,
        .expiryTime = std::nullopt,
        .receivedWallClock = wallNow,
        .expiryWallClock = std::nullopt,
    };
  }

  struct TempFile {
    std::filesystem::path path;

    explicit TempFile(const std::string& suffix)
        : path(
              std::filesystem::temp_directory_path()
              / ("terminusbar-test-" + std::to_string(static_cast<long long>(getpid())) + "-" + suffix)
          ) {
      std::filesystem::remove(path);
      std::filesystem::remove(path.string() + ".tmp");
    }

    ~TempFile() {
      std::filesystem::remove(path);
      std::filesystem::remove(path.string() + ".tmp");
    }
  };

  bool check(bool cond, const char* msg) {
    if (!cond) {
      std::cerr << "FAIL: " << msg << '\n';
    }
    return cond;
  }

} // namespace

int main() {
  bool ok = true;

  // 2-byte: split at boundary
  {
    std::string s(1023, 'a');
    s.push_back(static_cast<char>(0xD1));
    s.push_back(static_cast<char>(0x80));
    ok &= check(StringUtils::truncateUtf8(s, 1024).size() == 1023, "2-byte split: kept partial code point");
  }

  // 2-byte: fits at boundary
  {
    std::string s(1022, 'a');
    s.push_back(static_cast<char>(0xD1));
    s.push_back(static_cast<char>(0x80));
    ok &= check(StringUtils::truncateUtf8(s, 1024).size() == 1024, "2-byte exact: removed complete code point");
  }

  // 4-byte: split after 1st byte
  {
    std::string s(1023, 'a');
    s.push_back(static_cast<char>(0xF0));
    s.push_back(static_cast<char>(0x9F));
    s.push_back(static_cast<char>(0x98));
    s.push_back(static_cast<char>(0x80));
    ok &= check(StringUtils::truncateUtf8(s, 1024).size() == 1023, "4-byte split@1: kept partial code point");
  }

  // 4-byte: split after 2nd byte
  {
    std::string s(1022, 'a');
    s.push_back(static_cast<char>(0xF0));
    s.push_back(static_cast<char>(0x9F));
    s.push_back(static_cast<char>(0x98));
    s.push_back(static_cast<char>(0x80));
    ok &= check(StringUtils::truncateUtf8(s, 1024).size() == 1022, "4-byte split@2: kept partial code point");
  }

  // 4-byte: split after 3rd byte
  {
    std::string s(1021, 'a');
    s.push_back(static_cast<char>(0xF0));
    s.push_back(static_cast<char>(0x9F));
    s.push_back(static_cast<char>(0x98));
    s.push_back(static_cast<char>(0x80));
    ok &= check(StringUtils::truncateUtf8(s, 1024).size() == 1021, "4-byte split@3: kept partial code point");
  }

  // 4-byte: fits exactly
  {
    std::string s(1020, 'a');
    s.push_back(static_cast<char>(0xF0));
    s.push_back(static_cast<char>(0x9F));
    s.push_back(static_cast<char>(0x98));
    s.push_back(static_cast<char>(0x80));
    ok &= check(StringUtils::truncateUtf8(s, 1024).size() == 1024, "4-byte exact: removed complete code point");
  }

  // Persistence: malformed body serializes without throwing and produces valid JSON
  {
    TempFile tmp("utf8-history.json");

    std::string body = "prefix ";
    body.push_back(static_cast<char>(0xD1));

    std::deque<NotificationHistoryEntry> entries;
    entries.push_back(
        NotificationHistoryEntry{
            .notification = makeNotification(std::move(body)),
            .active = true,
            .closeReason = std::nullopt,
            .eventSerial = 1,
        }
    );

    try {
      if (!saveNotificationHistoryToFile(tmp.path, entries, 315, 2)) {
        ok &= check(false, "saveNotificationHistoryToFile returned false");
      }
    } catch (const std::exception& e) {
      std::cerr << "saveNotificationHistoryToFile threw: " << e.what() << '\n';
      ok &= check(false, "saveNotificationHistoryToFile threw");
    }

    std::ifstream in(tmp.path, std::ios::binary);
    ok &= check(in.good(), "history file was not written");

    if (in.good()) {
      std::ostringstream buf;
      buf << in.rdbuf();
      const auto parsed = nlohmann::json::parse(buf.str(), nullptr, false);
      ok &= check(!parsed.is_discarded(), "history file is not valid JSON");
    }
  }

  // Stored icon metadata upgrades without rewriting notification text.
  {
    TempFile tmp("previous-icon-history.json");
    {
      std::ofstream out(tmp.path);
      out << R"({
        "version": 2,
        "entries": [{
          "notification": {
            "id": 1,
            "origin": "internal",
            "icon": "noctalia-glyph:battery-exclamation",
            "body": "literal noctalia-glyph:message"
          }
        }]
      })";
    }
    std::deque<NotificationHistoryEntry> loaded;
    std::uint32_t nextId = 0;
    std::uint64_t serial = 0;
    const bool readable = loadNotificationHistoryFromFile(tmp.path, loaded, nextId, serial);
    ok &= check(readable && loaded.size() == 1, "previous history entry was lost");
    if (readable && loaded.size() == 1) {
      ok &= check(
          loaded.front().notification.icon == "terminusbar-glyph:battery-exclamation",
          "previous stored glyph identifier is not renderable"
      );
      ok &= check(
          loaded.front().notification.body == "literal noctalia-glyph:message",
          "metadata migration changed notification text"
      );
    }
  }

  return ok ? 0 : 1;
}
