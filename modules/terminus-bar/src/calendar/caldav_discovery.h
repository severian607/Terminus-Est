#pragma once

#include "security/secure_buffer.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class HttpClient;

namespace calendar {

  struct CalDavCollection {
    std::string id;
    std::string name;
    std::string url;
    std::string color;
  };

  enum class DiscoveryFailure : std::uint8_t { None, AuthFailed, Unreachable, NoCalendars, Other };
  using DiscoveryCallback = std::function<void(bool ok, std::vector<CalDavCollection>, DiscoveryFailure failure)>;

  void discoverCalDavCollections(
      HttpClient& http, const std::string& serverUrl, const std::string& username,
      std::shared_ptr<const security::SecureBuffer> password, bool allowRedirectAuth,
      DiscoveryCallback cb
  );

} // namespace calendar
