#pragma once

#include <algorithm>
#include <cctype>
#include <string_view>

namespace secret_shape {

  // What a masked clipboard preview reads as. Shared by the dashboard inbox and the msg interface.
  inline constexpr std::string_view kMaskedPreview = "•••••••••••• hidden";

  // The clipboard history carries no sensitive mark (password-manager copies are never stored), so secrets are
  // recognised by shape: a PEM block, a well-known token prefix, or one long unbroken run of letters and digits.
  [[nodiscard]] inline bool looksSensitive(std::string_view text) {
    if (text.starts_with("-----BEGIN")) {
      return true;
    }
    if (text.size() < 20
        || text.find_first_of(" \t\r\n") != std::string_view::npos
        || text.find("://") != std::string_view::npos) {
      return false;
    }
    for (const std::string_view prefix : {"sk-", "ghp_", "gho_", "github_pat_", "glpat-", "xox", "AKIA", "eyJ"}) {
      if (text.starts_with(prefix)) {
        return true;
      }
    }
    const bool letter =
        std::ranges::any_of(text, [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; });
    const bool digit =
        std::ranges::any_of(text, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
    return letter && digit;
  }

} // namespace secret_shape
