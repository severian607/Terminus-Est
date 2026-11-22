#pragma once

#include <algorithm>
#include <cctype>
#include <string_view>

namespace secret_shape {

  // What a masked clipboard preview reads as. Shared by the dashboard inbox and the msg interface.
  inline constexpr std::string_view kMaskedPreview = "•••••••••••• hidden";

  // True when `text` is a URL whose userinfo carries a password (`scheme://user:pass@host`).
  [[nodiscard]] inline bool hasUrlPassword(std::string_view text) {
    const auto scheme = text.find("://");
    if (scheme == std::string_view::npos) {
      return false;
    }
    std::string_view authority = text.substr(scheme + 3);
    authority = authority.substr(0, authority.find_first_of("/?# \t\r\n"));
    const auto at = authority.rfind('@');
    return at != std::string_view::npos && authority.substr(0, at).find(':') != std::string_view::npos;
  }

  // The clipboard history carries no sensitive mark (password-manager copies are never stored), so secrets are
  // recognised by shape, on the text with surrounding whitespace trimmed (a copy often ends in a newline):
  //   - a PEM block;
  //   - a URL with a password in its userinfo;
  //   - a first word of 20+ bytes starting with a well-known token prefix;
  //   - otherwise a single run of 20+ bytes with no whitespace and no "://" that has at least one letter and one
  //     digit, which also masks long hashes and file names; that errs on the side of hiding.
  [[nodiscard]] inline bool looksSensitive(std::string_view text) {
    constexpr std::string_view kSpace = " \t\r\n";
    const auto first = text.find_first_not_of(kSpace);
    if (first == std::string_view::npos) {
      return false;
    }
    text = text.substr(first, text.find_last_not_of(kSpace) - first + 1);

    if (text.starts_with("-----BEGIN") || hasUrlPassword(text)) {
      return true;
    }
    const std::string_view word = text.substr(0, text.find_first_of(kSpace));
    if (word.size() >= 20) {
      for (const std::string_view prefix : {"sk-", "ghp_", "gho_", "github_pat_", "glpat-", "xox", "AKIA", "eyJ"}) {
        if (word.starts_with(prefix)) {
          return true;
        }
      }
    }
    if (text.size() < 20
        || text.find_first_of(kSpace) != std::string_view::npos
        || text.find("://") != std::string_view::npos) {
      return false;
    }
    const bool letter =
        std::ranges::any_of(text, [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; });
    const bool digit =
        std::ranges::any_of(text, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
    return letter && digit;
  }

} // namespace secret_shape
