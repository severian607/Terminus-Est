#pragma once

// Data helpers of the NETWORK quick panel: saved NetworkManager profiles (via `nmcli -t`), tailscale state
// (`tailscale status --json`), latency probes and the traffic axis.
// Header-only; everything here is pure parsing.

#include "core/process/process.h"
#include "shell/control_center/tabs/async_command.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace netq {

  [[nodiscard]] inline bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
  }

  // A short, human reason for a failed command: known NetworkManager phrases are mapped, otherwise the first line of
  // stderr (stdout when stderr is empty) without the `Error:` prefix, clipped to fit a row.
  [[nodiscard]] inline std::string failureReason(const process::RunResult& result) {
    if (result.timedOut) {
      return "timeout";
    }
    std::string line = asynccmd::firstLine(result.err);
    if (line.empty()) {
      line = asynccmd::firstLine(result.out);
    }
    if (line.empty()) {
      return result.exitCode < 0 ? "could not start" : std::format("exit code {}", result.exitCode);
    }
    if (contains(line, "Secrets were required") || contains(line, "No secrets") || contains(line, "wrong password")) {
      return "wrong password";
    }
    if (contains(line, "No suitable device") || contains(line, "device is not available")) {
      return "no device";
    }
    if (contains(line, "Timeout expired") || contains(line, "timeout")) {
      return "timeout";
    }
    if (line.starts_with("Error: ")) {
      line.erase(0, 7);
    }
    if (const auto pos = line.find("Connection activation failed: "); pos != std::string::npos) {
      line.erase(pos, 30);
    }
    constexpr std::size_t kMax = 38;
    if (line.size() > kMax) {
      // Do not cut a UTF-8 sequence in half.
      std::size_t cut = kMax - 1;
      while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0U) == 0x80U) {
        --cut;
      }
      line.resize(cut);
      line += "…";
    }
    return line;
  }

  // ---- saved NetworkManager profiles ---------------------------------------------------------------------

  enum class ProfileKind : std::uint8_t { Ethernet, Wifi, Vpn, Wireguard };

  struct Profile {
    std::string name;
    std::string uuid;
    ProfileKind kind = ProfileKind::Ethernet;
    std::string device;
    bool active = false;
    bool activating = false;
    bool autoconnect = false;
  };

  [[nodiscard]] inline const char* kindLabel(ProfileKind kind) {
    switch (kind) {
    case ProfileKind::Ethernet:
      return "ethernet";
    case ProfileKind::Wifi:
      return "wi-fi";
    case ProfileKind::Vpn:
      return "vpn";
    case ProfileKind::Wireguard:
      return "wireguard";
    }
    return "";
  }

  // `nmcli -t` separates fields with ':' and escapes ':' and '\' inside a field with a backslash.
  [[nodiscard]] inline std::vector<std::string> splitTerse(std::string_view line) {
    std::vector<std::string> fields(1);
    for (std::size_t i = 0; i < line.size(); ++i) {
      const char c = line[i];
      if (c == '\\' && i + 1 < line.size()) {
        fields.back().push_back(line[++i]);
      } else if (c == ':') {
        fields.emplace_back();
      } else {
        fields.back().push_back(c);
      }
    }
    return fields;
  }

  inline constexpr const char* kProfileFields = "NAME,UUID,TYPE,DEVICE,ACTIVE,STATE,AUTOCONNECT";

  [[nodiscard]] inline std::vector<Profile> parseProfiles(std::string_view out) {
    std::vector<Profile> profiles;
    std::size_t begin = 0;
    while (begin < out.size()) {
      std::size_t end = out.find('\n', begin);
      if (end == std::string_view::npos) {
        end = out.size();
      }
      const std::string_view line = out.substr(begin, end - begin);
      begin = end + 1;
      if (line.empty()) {
        continue;
      }
      const auto fields = splitTerse(line);
      if (fields.size() < 7) {
        continue;
      }
      Profile profile;
      profile.name = fields[0];
      profile.uuid = fields[1];
      const std::string& type = fields[2];
      if (type == "802-3-ethernet" || type == "ethernet") {
        profile.kind = ProfileKind::Ethernet;
      } else if (type == "802-11-wireless" || type == "wifi") {
        profile.kind = ProfileKind::Wifi;
      } else if (type == "vpn") {
        profile.kind = ProfileKind::Vpn;
      } else if (type == "wireguard") {
        profile.kind = ProfileKind::Wireguard;
      } else {
        continue;
      }
      profile.device = fields[3];
      profile.activating = fields[5].starts_with("activating");
      profile.active = fields[4] == "yes" && !profile.activating;
      profile.autoconnect = fields[6] == "yes";
      if (profile.uuid.empty()) {
        continue;
      }
      profiles.push_back(std::move(profile));
    }
    std::ranges::stable_sort(profiles, [](const Profile& a, const Profile& b) {
      if (a.kind != b.kind) {
        return a.kind < b.kind;
      }
      return a.name < b.name;
    });
    return profiles;
  }

  // ---- tailscale -----------------------------------------------------------------------------------------

  struct TsPeer {
    std::string name;
    std::string ip;
    bool online = false;
    std::int64_t lastSeen = 0; // unix seconds; 0 = never
    bool exitOption = false;
    bool exitActive = false;
  };

  struct TsState {
    bool loaded = false;      // a status read has completed
    bool running = false;     // BackendState == Running
    std::string problem;      // "needs login", "daemon down", ... empty when fine
    std::string exitNode;     // name of the exit node in use, empty for none
    std::vector<TsPeer> peers; // online first, then by name
  };

  [[nodiscard]] inline std::string firstIpv4(const nlohmann::json& ips) {
    if (!ips.is_array()) {
      return {};
    }
    std::string fallback;
    for (const auto& item : ips) {
      if (!item.is_string()) {
        continue;
      }
      std::string ip = item.get<std::string>();
      if (ip.find(':') == std::string::npos) {
        return ip;
      }
      if (fallback.empty()) {
        fallback = std::move(ip);
      }
    }
    return fallback;
  }

  // "2026-11-19T10:00:00.123Z" (or with a numeric offset) -> unix seconds; 0 for absent, zero-valued or unreadable.
  [[nodiscard]] inline std::int64_t parseRfc3339Utc(const std::string& text) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) != 6 || y <= 1970) {
      return 0;
    }
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = sec;
    std::int64_t unix = static_cast<std::int64_t>(timegm(&tm));
    // Numeric offset ("+01:00" / "-05:00") after the optional fraction; "Z" means UTC.
    const auto tz = text.find_last_of("+-");
    if (tz != std::string::npos && tz > 10) {
      int oh = 0, om = 0;
      if (std::sscanf(text.c_str() + tz + 1, "%d:%d", &oh, &om) >= 1) {
        const std::int64_t offset = oh * 3600 + om * 60;
        unix += text[tz] == '+' ? -offset : offset;
      }
    }
    return unix;
  }

  // "3 d" / "5 h" / "12 m" / "now" for an offline peer; "never" when unknown.
  [[nodiscard]] inline std::string lastSeenText(std::int64_t lastSeen) {
    if (lastSeen <= 0) {
      return "last seen never";
    }
    const std::int64_t age = std::max<std::int64_t>(
        0, static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                         std::chrono::system_clock::now().time_since_epoch()
        ).count()) - lastSeen
    );
    if (age < 60) {
      return "last seen now";
    }
    if (age < 3600) {
      return std::format("last seen {} m", age / 60);
    }
    if (age < 86400) {
      return std::format("last seen {} h", age / 3600);
    }
    return std::format("last seen {} d", age / 86400);
  }

  [[nodiscard]] inline std::string peerName(const nlohmann::json& peer) {
    std::string name = peer.value("HostName", std::string{});
    if (name.empty()) {
      name = peer.value("DNSName", std::string{});
      if (const auto dot = name.find('.'); dot != std::string::npos) {
        name.resize(dot);
      }
    }
    return name;
  }

  // `tailscale status --json`. A malformed document leaves `problem` set.
  [[nodiscard]] inline TsState parseTailscaleStatus(std::string_view json) {
    TsState state;
    state.loaded = true;
    const auto doc = nlohmann::json::parse(json, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
      state.problem = "unreadable status";
      return state;
    }
    const std::string backend = doc.value("BackendState", std::string{});
    state.running = backend == "Running";
    if (backend == "NeedsLogin" || backend == "NeedsMachineAuth") {
      state.problem = "needs login";
    }
    if (const auto it = doc.find("Peer"); it != doc.end() && it->is_object()) {
      for (const auto& [key, peer] : it->items()) {
        if (!peer.is_object()) {
          continue;
        }
        TsPeer entry;
        entry.name = peerName(peer);
        if (entry.name.empty()) {
          entry.name = key.substr(0, 8);
        }
        entry.ip = peer.contains("TailscaleIPs") ? firstIpv4(peer["TailscaleIPs"]) : std::string{};
        entry.online = peer.value("Online", false);
        entry.lastSeen = parseRfc3339Utc(peer.value("LastSeen", std::string{}));
        entry.exitOption = peer.value("ExitNodeOption", false);
        entry.exitActive = peer.value("ExitNode", false);
        if (entry.exitActive) {
          state.exitNode = entry.name;
        }
        state.peers.push_back(std::move(entry));
      }
    }
    std::ranges::stable_sort(state.peers, [](const TsPeer& a, const TsPeer& b) {
      if (a.online != b.online) {
        return a.online;
      }
      return a.name < b.name;
    });
    return state;
  }

  // ---- latency -------------------------------------------------------------------------------------------

  // Milliseconds from `ping -c1` output (`time=18.3 ms`), nullopt when there was no reply.
  [[nodiscard]] inline std::optional<double> parsePingMs(std::string_view out) {
    const auto pos = out.find("time=");
    if (pos == std::string_view::npos) {
      return std::nullopt;
    }
    const std::string tail(out.substr(pos + 5, 16));
    char* end = nullptr;
    const double value = std::strtod(tail.c_str(), &end);
    if (end == tail.c_str() || value < 0.0) {
      return std::nullopt;
    }
    return value;
  }

  struct Probe {
    bool known = false;               // a probe has completed
    bool inFlight = false;
    std::optional<double> ms;         // nullopt after a completed probe = no reply
  };

  // ---- traffic axis --------------------------------------------------------------------------------------

  // Smallest 1/2/2.5/5 x 10^n bytes/s ceiling at or above `value`.
  [[nodiscard]] inline double niceCeiling(double value) {
    if (value <= 1.0) {
      return 1.0;
    }
    const double magnitude = std::pow(10.0, std::floor(std::log10(value)));
    for (const double step : {1.0, 2.0, 2.5, 5.0, 10.0}) {
      if (step * magnitude >= value) {
        return step * magnitude;
      }
    }
    return 10.0 * magnitude;
  }

  // `25K`, `1M`, `2.5M` style axis label for a bytes/s ceiling.
  [[nodiscard]] inline std::string axisLabel(double bytesPerSec) {
    const auto compact = [](double value, const char* unit) {
      return value == std::floor(value) ? std::format("{:.0f}{}", value, unit) : std::format("{:.1f}{}", value, unit);
    };
    if (bytesPerSec >= 1e9) {
      return compact(bytesPerSec / 1e9, "G");
    }
    if (bytesPerSec >= 1e6) {
      return compact(bytesPerSec / 1e6, "M");
    }
    return compact(bytesPerSec / 1e3, "K");
  }

} // namespace netq
