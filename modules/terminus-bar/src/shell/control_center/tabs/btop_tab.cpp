#include "shell/control_center/tabs/btop_tab.h"

#include "core/files/resource_paths.h"
#include "core/input/key_modifiers.h"
#include "render/core/renderer.h"
#include "shell/control_center/term_frame.h"
#include "shell/panel/panel_manager.h"
#include "ui/builders.h"
#include "ui/controls/flex.h"
#include "ui/controls/label.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <format>
#include <pty.h>
#include <string>
#include <string_view>
#include <thread>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace {

  constexpr std::string_view kFontFamily = "MonaspiceNe Nerd Font";
  // Ghostty's configured 9 pt terminal font rasterizes to 12 logical pixels at 96 DPI.
  constexpr float kFontSize = 12.0F;
  // The title row above the terminal, like the dashboard tiles' own: `⁸manager` and a rule.
  constexpr float kTitleHeight = 22.0F;
  constexpr std::string_view kDefaultForeground = "cfd8dc";
  constexpr auto kFallbackDrainInterval = std::chrono::milliseconds(2000);
  constexpr int kMaxResponseDrainFrames = 12;
  constexpr int kMaxConsecutiveEmptyDrains = 6;
  constexpr std::string_view kDefaultBackground = "14151a";

  struct Rgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    [[nodiscard]] bool operator==(const Rgb&) const = default;
  };

  struct CellStyle {
    Rgb foreground;
    Rgb background;
    bool drawBackground = false;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strike = false;

    [[nodiscard]] bool operator==(const CellStyle&) const = default;
  };

  [[nodiscard]] Rgb hexRgb(std::string_view value) {
    const auto byte = [value](std::size_t offset) {
      return static_cast<std::uint8_t>(std::stoul(std::string(value.substr(offset, 2)), nullptr, 16));
    };
    return {.r = byte(0), .g = byte(2), .b = byte(4)};
  }

  [[nodiscard]] std::string hexColor(const Rgb& color) {
    return std::format("#{:02x}{:02x}{:02x}", color.r, color.g, color.b);
  }

  [[nodiscard]] Rgb resolvedColor(VTermScreen* screen, VTermColor color, const Rgb& fallback, bool isDefault) {
    if (isDefault) {
      return fallback;
    }
    vterm_screen_convert_color_to_rgb(screen, &color);
    return {.r = color.rgb.red, .g = color.rgb.green, .b = color.rgb.blue};
  }

  void appendEscaped(std::string& out, std::string_view value) {
    for (const char ch : value) {
      switch (ch) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '\'':
        out += "&apos;";
        break;
      case '"':
        out += "&quot;";
        break;
      default:
        out.push_back(ch);
        break;
      }
    }
  }

  void appendUtf8(std::string& out, std::uint32_t codepoint) {
    if (codepoint <= 0x7FU) {
      out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFU) {
      out.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
      out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else if (codepoint <= 0xFFFFU) {
      out.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
      out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else if (codepoint <= 0x10FFFFU) {
      out.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
      out.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    }
  }

  void appendStyledRun(std::string& markup, const CellStyle& style, std::string_view text) {
    if (text.empty()) {
      return;
    }
    markup += "<span foreground=\"";
    markup += hexColor(style.foreground);
    markup += '"';
    if (style.drawBackground) {
      markup += " background=\"";
      markup += hexColor(style.background);
      markup += '"';
    }
    if (style.bold) {
      markup += " weight=\"bold\"";
    }
    if (style.italic) {
      markup += " style=\"italic\"";
    }
    if (style.underline) {
      markup += " underline=\"single\"";
    }
    if (style.strike) {
      markup += " strikethrough=\"true\"";
    }
    markup += '>';
    appendEscaped(markup, text);
    markup += "</span>";
  }

  [[nodiscard]] VTermModifier vtermModifiers(std::uint32_t modifiers) {
    unsigned int out = VTERM_MOD_NONE;
    if ((modifiers & KeyMod::Shift) != 0U) {
      out |= VTERM_MOD_SHIFT;
    }
    if ((modifiers & KeyMod::Alt) != 0U) {
      out |= VTERM_MOD_ALT;
    }
    if ((modifiers & KeyMod::Ctrl) != 0U) {
      out |= VTERM_MOD_CTRL;
    }
    return static_cast<VTermModifier>(out);
  }

  [[nodiscard]] VTermKey vtermKey(std::uint32_t sym) {
    switch (sym) {
    case XKB_KEY_Return:
    case XKB_KEY_ISO_Enter:
      return VTERM_KEY_ENTER;
    case XKB_KEY_KP_Enter:
      return VTERM_KEY_KP_ENTER;
    case XKB_KEY_Tab:
    case XKB_KEY_ISO_Left_Tab:
      return VTERM_KEY_TAB;
    case XKB_KEY_BackSpace:
      return VTERM_KEY_BACKSPACE;
    case XKB_KEY_Escape:
      return VTERM_KEY_ESCAPE;
    case XKB_KEY_Up:
      return VTERM_KEY_UP;
    case XKB_KEY_Down:
      return VTERM_KEY_DOWN;
    case XKB_KEY_Left:
      return VTERM_KEY_LEFT;
    case XKB_KEY_Right:
      return VTERM_KEY_RIGHT;
    case XKB_KEY_Insert:
      return VTERM_KEY_INS;
    case XKB_KEY_Delete:
      return VTERM_KEY_DEL;
    case XKB_KEY_Home:
      return VTERM_KEY_HOME;
    case XKB_KEY_End:
      return VTERM_KEY_END;
    case XKB_KEY_Page_Up:
      return VTERM_KEY_PAGEUP;
    case XKB_KEY_Page_Down:
      return VTERM_KEY_PAGEDOWN;
    default:
      if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F35) {
        return static_cast<VTermKey>(VTERM_KEY_FUNCTION(static_cast<int>(sym - XKB_KEY_F1 + 1U)));
      }
      return VTERM_KEY_NONE;
    }
  }

  constexpr auto kTermGrace = std::chrono::milliseconds(1500);
  constexpr auto kReapPoll = std::chrono::milliseconds(20);

  // Takes ownership of `child` (already detached from the tab): SIGTERM, poll with WNOHANG, SIGKILL after the
  // grace period, then reap. The pid is only signalled while unreaped, so it cannot have been recycled.
  // Runs on its own thread so the UI thread never waits; the thread outlives the tab.
  void reapChildAsync(pid_t child) {
    std::thread([child]() {
      const auto reaped = [child]() {
        int status = 0;
        for (;;) {
          const pid_t r = ::waitpid(child, &status, WNOHANG);
          if (r == child) {
            return true;
          }
          if (r < 0) {
            if (errno == EINTR) {
              continue;
            }
            return true; // ECHILD: nothing left to reap
          }
          return false;
        }
      };
      (void)::kill(child, SIGTERM);
      const auto deadline = std::chrono::steady_clock::now() + kTermGrace;
      while (std::chrono::steady_clock::now() < deadline) {
        if (reaped()) {
          return;
        }
        std::this_thread::sleep_for(kReapPoll);
      }
      if (reaped()) {
        return;
      }
      (void)::kill(child, SIGKILL);
      int status = 0;
      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
    }).detach();
  }

} // namespace

BtopTab::BtopTab() {
  m_vterm = vterm_new(m_rows, m_cols);
  if (m_vterm == nullptr) {
    return;
  }
  vterm_set_utf8(m_vterm, 1);
  vterm_output_set_callback(m_vterm, &BtopTab::onVtermOutput, this);
  m_screen = vterm_obtain_screen(m_vterm);
  vterm_screen_enable_altscreen(m_screen, 1);
  vterm_screen_set_damage_merge(m_screen, VTERM_DAMAGE_SCREEN);

  static constexpr VTermScreenCallbacks callbacks{
      .damage = &BtopTab::onVtermDamage,
      .moverect = nullptr,
      .movecursor = &BtopTab::onVtermCursor,
      .settermprop = &BtopTab::onVtermProperty,
      .bell = nullptr,
      .resize = nullptr,
      .sb_pushline = nullptr,
      .sb_popline = nullptr,
      .sb_clear = nullptr,
  };
  vterm_screen_set_callbacks(m_screen, &callbacks, this);

  VTermColor foreground;
  VTermColor background;
  const Rgb fg = hexRgb(kDefaultForeground);
  const Rgb bg = hexRgb(kDefaultBackground);
  vterm_color_rgb(&foreground, fg.r, fg.g, fg.b);
  vterm_color_rgb(&background, bg.r, bg.g, bg.b);
  vterm_screen_set_default_colors(m_screen, &foreground, &background);
  vterm_screen_reset(m_screen, 1);
}

BtopTab::~BtopTab() {
  stopProcess();
  if (m_vterm != nullptr) {
    vterm_free(m_vterm);
    m_vterm = nullptr;
    m_screen = nullptr;
  }
}

std::unique_ptr<Flex> BtopTab::create() {
  auto root = ui::column({
      .out = &m_root,
      .align = FlexAlign::Stretch,
      .fillWidth = true,
      .fillHeight = true,
      .clipChildren = true,
  });

  auto terminal = ui::label({
      .out = &m_terminal,
      .text = "Starting task manager…",
      .fontSize = m_fontSize,
      .fontFamily = std::string{kFontFamily},
      .color = colorSpecFromRole(ColorRole::OnSurface),
      .maxLines = 0,
      .flexGrow = 1.0F,
      .configure = [this](Label& label) {
        label.setUseMarkup(true);
        label.setFocusable(true);
        label.setTabStop(true);
        label.setTabFocusKey("control-center-btop");
        label.setRetainsFocusOnPointerRelease(true);
        label.setHitTestVisible(true);
        label.setAcceptedButtons(InputArea::buttonMask({BTN_LEFT, BTN_MIDDLE, BTN_RIGHT}));
        label.setOnMotion([this](const InputArea::PointerData& data) { sendPointerMotion(data.localX, data.localY); });
        label.setOnPress([this](const InputArea::PointerData& data) { sendPointerButton(data); });
        label.setOnAxisHandler([this](const InputArea::PointerData& data) { return sendPointerAxis(data); });
        label.setOnFocusGain([this]() {
          if (m_vterm != nullptr) {
            vterm_state_focus_in(vterm_obtain_state(m_vterm));
            if (m_cursorVisible) {
              m_screenDirty = true;
            }
            (void)drainAndRefresh();
            armResponseDrain();
          }
        });
        label.setOnKeyDown([this](const InputArea::KeyData& data) {
          (void)handleKey(data.sym, data.utf32, data.modifiers, data.pressed, data.preedit);
        });
        label.setOnFocusLoss([this]() {
          if (m_vterm != nullptr) {
            vterm_state_focus_out(vterm_obtain_state(m_vterm));
            if (m_cursorVisible) {
              m_screenDirty = true;
            }
            (void)drainAndRefresh();
            armResponseDrain();
          }
        });
      },
  });
  root->addChild(std::move(terminal));
  auto titleRow = term_frame::titleRow("⁸", "manager", contentScale());
  m_titleRow = titleRow.get();
  root->addChild(std::move(titleRow));
  m_screenDirty = true;

  if (m_active) {
    startProcess();
  }
  return root;
}

void BtopTab::preload() {
  // Warm start at boot: spawn at the typical dashboard grid. The first real layout resizes the pty and
  // Ctrl-R makes the task manager repaint at the final size.
  m_sizeKnown = true;
  startProcess();
}

void BtopTab::setActive(bool active) {
  if (m_active == active) {
    return;
  }
  m_active = active;
  if (active) {
    startProcess();
    (void)drainAndRefresh();
    armResponseDrain();
  } else {
    m_responseDrainFramesRemaining = 0;
    m_consecutiveEmptyDrains = 0;
  }
}

void BtopTab::onClose() {
  m_responseDrainFramesRemaining = 0;
  m_consecutiveEmptyDrains = 0;
  m_root = nullptr;
  m_terminal = nullptr;
  m_titleRow = nullptr;
}

void BtopTab::onFrameTick(float /*deltaMs*/) {
  if (!m_active || m_ptyFd < 0 || m_responseDrainFramesRemaining <= 0) {
    return;
  }

  --m_responseDrainFramesRemaining;
  if (drainAndRefresh()) {
    m_consecutiveEmptyDrains = 0;
  } else {
    ++m_consecutiveEmptyDrains;
  }

  if (m_ptyFd >= 0 && m_responseDrainFramesRemaining > 0 && m_consecutiveEmptyDrains < kMaxConsecutiveEmptyDrains) {
    PanelManager::instance().requestFrameTick();
    PanelManager::instance().requestRedraw();
  } else {
    m_responseDrainFramesRemaining = 0;
  }
}

bool BtopTab::ownsKeyboardFocus() const noexcept { return m_terminal != nullptr && m_terminal->focused(); }

InputArea* BtopTab::focusArea() const noexcept { return m_terminal; }

bool BtopTab::handleKey(std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers, bool pressed, bool preedit) {
  if (!ownsKeyboardFocus() || preedit) {
    return false;
  }
  if (!pressed) {
    return true;
  }
  if (m_childPid <= 0) {
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
      startProcess();
    }
    return true;
  }
  if (m_vterm == nullptr || (modifiers & KeyMod::Super) != 0U) {
    return false;
  }

  const VTermModifier mods = vtermModifiers(modifiers);
  const VTermKey key = vtermKey(sym);
  if (key != VTERM_KEY_NONE) {
    vterm_keyboard_key(m_vterm, key, mods);
    (void)drainAndRefresh();
    armResponseDrain();
    return true;
  }
  if (utf32 != 0U) {
    vterm_keyboard_unichar(m_vterm, utf32, mods);
    (void)drainAndRefresh();
    armResponseDrain();
    return true;
  }
  return true;
}

void BtopTab::doLayout(Renderer& renderer, float contentWidth, float bodyHeight) {
  if (m_root == nullptr || m_terminal == nullptr) {
    return;
  }

  m_fontSize = kFontSize * contentScale();
  m_terminal->setFontSize(m_fontSize);
  m_terminal->setFontFamily(std::string{kFontFamily});
  m_terminal->setMaxWidth(contentWidth);

  const TextMetrics cell =
      renderer.measureText("M", m_fontSize, FontWeight::Normal, 0.0F, 1, TextAlign::Start, kFontFamily);
  m_cellWidth = std::max(1.0F, cell.width);
  m_cellHeight = std::max(1.0F, cell.bottom - cell.top);
  const int cols = std::max(40, static_cast<int>(std::floor(contentWidth / m_cellWidth)));
  const float titleHeight = kTitleHeight * contentScale();
  const int rows = std::max(12, static_cast<int>(std::floor((bodyHeight - titleHeight) / m_cellHeight)));
  resizeTerminal(rows, cols);
  if (!m_sizeKnown) {
    m_sizeKnown = true;
    startProcess();
  }

  m_root->setSize(contentWidth, bodyHeight);
  m_root->layout(renderer);
  if (m_titleRow != nullptr) {
    m_titleRow->setPosition(0.0F, 0.0F);
    m_titleRow->setSize(contentWidth, titleHeight);
    m_titleRow->layout(renderer);
  }
  m_terminal->setPosition(0.0F, titleHeight);
  m_terminal->setSize(contentWidth, std::max(1.0F, bodyHeight - titleHeight));
}

void BtopTab::doUpdate(Renderer& /*renderer*/) {
  if (m_screenDirty && m_screen != nullptr && m_terminal != nullptr) {
    writeMarkup();
  }
}

void BtopTab::startProcess() {
  // Starting before the first layout makes btop draw into a stale 80x24 grid (a small square in the corner)
  // until the resize lands, so wait for the real panel size.
  if (!m_sizeKnown || m_childPid > 0 || m_vterm == nullptr || m_screen == nullptr) {
    return;
  }

  const auto now = std::chrono::steady_clock::now();
  if (m_lastStart.time_since_epoch().count() != 0 && now - m_lastStart < std::chrono::milliseconds(250)) {
    return;
  }
  m_lastStart = now;
  vterm_screen_reset(m_screen, 1);
  m_screenDirty = true;

  const std::string configPath = paths::assetPath("btop/terminusbar.conf").string();
  // btop >= 1.4.8 ignores ~/.config/btop/themes when --config is given; the generated theme lives there.
  std::string themesDir;
  if (const char* xdgConfig = std::getenv("XDG_CONFIG_HOME"); xdgConfig != nullptr && xdgConfig[0] == '/') {
    themesDir = std::string(xdgConfig) + "/btop/themes";
  } else if (const char* home = std::getenv("HOME"); home != nullptr && home[0] == '/') {
    themesDir = std::string(home) + "/.config/btop/themes";
  }
  winsize size{
      .ws_row = static_cast<unsigned short>(std::clamp(m_rows, 1, 65535)),
      .ws_col = static_cast<unsigned short>(std::clamp(m_cols, 1, 65535)),
      .ws_xpixel = 0,
      .ws_ypixel = 0,
  };
  int ptyFd = -1;
  const pid_t child = ::forkpty(&ptyFd, nullptr, nullptr, &size);
  if (child < 0) {
    setStatus("Unable to start the task manager");
    return;
  }
  if (child == 0) {
    ::setenv("TERM", "xterm-256color", 1);
    ::setenv("COLORTERM", "truecolor", 1);
    ::unsetenv("NO_COLOR");
    // Terminus-TaskManager, square-cornered like the panel's other boxes; btop where it is not installed.
    ::execlp("terminus-taskmanager", "terminus-taskmanager", "--square", static_cast<char*>(nullptr));
    if (themesDir.empty()) {
      ::execl("/usr/bin/btop", "btop", "--config", configPath.c_str(), static_cast<char*>(nullptr));
    } else {
      ::execl("/usr/bin/btop", "btop", "--config", configPath.c_str(), "--themes-dir", themesDir.c_str(),
              static_cast<char*>(nullptr));
    }
    _exit(127);
  }

  const int flags = ::fcntl(ptyFd, F_GETFL, 0);
  if (flags >= 0) {
    (void)::fcntl(ptyFd, F_SETFL, flags | O_NONBLOCK);
  }
  m_ptyFd = ptyFd;
  m_childPid = child;
  (void)drainAndRefresh();
  armResponseDrain();
  scheduleFallbackDrain();
}

void BtopTab::stopProcess() {
  m_fallbackDrainTimer.stop();
  m_responseDrainFramesRemaining = 0;
  m_consecutiveEmptyDrains = 0;
  const pid_t child = m_childPid;
  m_childPid = -1;
  if (m_ptyFd >= 0) {
    ::close(m_ptyFd);
    m_ptyFd = -1;
  }
  if (child <= 0) {
    return;
  }

  reapChildAsync(child);
}

BtopTab::PtyDrainResult BtopTab::drainPty() {
  if (m_ptyFd < 0 || m_vterm == nullptr) {
    return {};
  }

  std::array<char, 65536> buffer{};
  bool received = false;
  bool ended = false;
  for (;;) {
    const ssize_t count = ::read(m_ptyFd, buffer.data(), buffer.size());
    if (count > 0) {
      (void)vterm_input_write(m_vterm, buffer.data(), static_cast<std::size_t>(count));
      received = true;
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    }

    ::close(m_ptyFd);
    m_ptyFd = -1;
    if (m_childPid > 0) {
      // PTY EOF normally means the child exited; if it lingers, the reaper escalates TERM then KILL.
      reapChildAsync(m_childPid);
      m_childPid = -1;
    }
    setStatus("Task manager exited — press Enter to restart");
    ended = true;
    break;
  }

  bool needsRefresh = ended;
  if (received) {
    vterm_screen_flush_damage(m_screen);
    if (m_screenDirty && m_terminal != nullptr) {
      writeMarkup();
      needsRefresh = true;
    }
  }
  return {
      .activity = received || ended,
      .needsRefresh = needsRefresh,
  };
}

bool BtopTab::drainAndRefresh() {
  const PtyDrainResult result = drainPty();
  // With the panel closed (preloaded task manager) there is nothing to relayout or redraw.
  if (result.needsRefresh && m_terminal != nullptr) {
    PanelManager::instance().requestLayout();
    PanelManager::instance().requestRedraw();
  }
  return result.activity;
}

void BtopTab::armResponseDrain() {
  if (!m_active || m_ptyFd < 0) {
    return;
  }
  m_responseDrainFramesRemaining = kMaxResponseDrainFrames;
  m_consecutiveEmptyDrains = 0;
  PanelManager::instance().requestFrameTick();
  PanelManager::instance().requestRedraw();
}

void BtopTab::scheduleFallbackDrain() {
  if (m_ptyFd < 0) {
    return;
  }
  m_fallbackDrainTimer.start(kFallbackDrainInterval, [this]() {
    if (m_ptyFd < 0) {
      return;
    }
    const bool activity = drainAndRefresh();
    if (m_ptyFd >= 0) {
      if (activity) {
        armResponseDrain();
      }
      scheduleFallbackDrain();
    }
  });
}

void BtopTab::resizeTerminal(int rows, int cols) {
  if (rows == m_rows && cols == m_cols) {
    return;
  }
  m_rows = rows;
  m_cols = cols;
  if (m_vterm != nullptr) {
    vterm_set_size(m_vterm, rows, cols);
  }
  if (m_ptyFd >= 0) {
    winsize size{
        .ws_row = static_cast<unsigned short>(std::clamp(rows, 1, 65535)),
        .ws_col = static_cast<unsigned short>(std::clamp(cols, 1, 65535)),
        .ws_xpixel =
            static_cast<unsigned short>(std::clamp(std::lround(static_cast<float>(cols) * m_cellWidth), 0L, 65535L)),
        .ws_ypixel =
            static_cast<unsigned short>(std::clamp(std::lround(static_cast<float>(rows) * m_cellHeight), 0L, 65535L)),
    };
    (void)::ioctl(m_ptyFd, TIOCSWINSZ, &size);
    // SIGWINCH updates btop's geometry, but its next full repaint otherwise waits
    // for update_ms. Ctrl-R requests that repaint immediately so the first visible
    // frame uses the final panel dimensions instead of the warm 80x24 buffer.
    static constexpr char kRequestRedraw = '\x12';
    writePty(&kRequestRedraw, 1);
    armResponseDrain();
  }
  m_screenDirty = true;
}

void BtopTab::writePty(const char* data, std::size_t size) {
  if (m_ptyFd < 0) {
    return;
  }
  std::size_t offset = 0;
  while (offset < size) {
    const ssize_t count = ::write(m_ptyFd, data + offset, size - offset);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
}

void BtopTab::writeMarkup() {
  if (m_screen == nullptr || m_terminal == nullptr) {
    return;
  }

  const Rgb defaultForeground = hexRgb(kDefaultForeground);
  const Rgb defaultBackground = hexRgb(kDefaultBackground);
  std::string markup;
  markup.reserve(static_cast<std::size_t>(m_rows * m_cols * 2));

  for (int row = 0; row < m_rows; ++row) {
    CellStyle runStyle{};
    std::string runText;
    bool hasRun = false;

    const auto flush = [&]() {
      if (hasRun) {
        appendStyledRun(markup, runStyle, runText);
        runText.clear();
      }
    };

    for (int col = 0; col < m_cols; ++col) {
      VTermScreenCell cell{};
      if (vterm_screen_get_cell(m_screen, {.row = row, .col = col}, &cell) == 0 || cell.width == 0) {
        continue;
      }

      const bool defaultFg = VTERM_COLOR_IS_DEFAULT_FG(&cell.fg);
      const bool defaultBg = VTERM_COLOR_IS_DEFAULT_BG(&cell.bg);
      CellStyle style{
          .foreground = resolvedColor(m_screen, cell.fg, defaultForeground, defaultFg),
          .background = resolvedColor(m_screen, cell.bg, defaultBackground, defaultBg),
          .drawBackground = !defaultBg,
          .bold = cell.attrs.bold != 0U,
          .italic = cell.attrs.italic != 0U,
          .underline = cell.attrs.underline != 0U,
          .strike = cell.attrs.strike != 0U,
      };
      if (cell.attrs.reverse != 0U) {
        std::swap(style.foreground, style.background);
        style.drawBackground = true;
      }
      if (m_cursorVisible && row == m_cursorRow && col == m_cursorCol && ownsKeyboardFocus()) {
        std::swap(style.foreground, style.background);
        style.drawBackground = true;
      }

      std::string glyph;
      if (cell.attrs.conceal != 0U || cell.chars[0] == 0U) {
        glyph = " ";
      } else {
        for (const std::uint32_t codepoint : cell.chars) {
          if (codepoint == 0U) {
            break;
          }
          appendUtf8(glyph, codepoint);
        }
      }

      if (!hasRun || !(style == runStyle)) {
        flush();
        runStyle = style;
        hasRun = true;
      }
      runText += glyph;
    }
    flush();
    if (row + 1 < m_rows) {
      markup.push_back('\n');
    }
  }

  m_terminal->setText(markup);
  m_screenDirty = false;
}

void BtopTab::setStatus(std::string message) {
  if (m_terminal == nullptr) {
    return;
  }
  std::string markup = "<span foreground=\"" + hexColor(hexRgb(kDefaultForeground)) + "\">";
  appendEscaped(markup, message);
  markup += "</span>";
  m_terminal->setText(markup);
  m_screenDirty = false;
}

void BtopTab::sendPointerMotion(float localX, float localY, std::uint32_t modifiers) {
  if (m_vterm == nullptr || m_childPid <= 0) {
    return;
  }
  const int col = std::clamp(static_cast<int>(localX / m_cellWidth), 0, m_cols - 1);
  const int row = std::clamp(static_cast<int>(localY / m_cellHeight), 0, m_rows - 1);
  vterm_mouse_move(m_vterm, row, col, vtermModifiers(modifiers));
}

void BtopTab::sendPointerButton(const InputArea::PointerData& data) {
  if (m_vterm == nullptr || m_childPid <= 0) {
    return;
  }
  sendPointerMotion(data.localX, data.localY);
  int button = 0;
  if (data.button == BTN_LEFT) {
    button = 1;
  } else if (data.button == BTN_MIDDLE) {
    button = 2;
  } else if (data.button == BTN_RIGHT) {
    button = 3;
  }
  if (button != 0) {
    vterm_mouse_button(m_vterm, button, data.pressed, VTERM_MOD_NONE);
    (void)drainAndRefresh();
    armResponseDrain();
  }
}

bool BtopTab::sendPointerAxis(const InputArea::PointerData& data) {
  if (m_vterm == nullptr || m_childPid <= 0) {
    return false;
  }
  const float steps = data.scrollSteps();
  if (steps == 0.0F) {
    return false;
  }
  sendPointerMotion(data.localX, data.localY);
  const int button = steps > 0.0F ? 5 : 4;
  const int count = std::max(1, static_cast<int>(std::lround(std::abs(steps))));
  for (int i = 0; i < count; ++i) {
    vterm_mouse_button(m_vterm, button, true, VTERM_MOD_NONE);
    vterm_mouse_button(m_vterm, button, false, VTERM_MOD_NONE);
  }
  (void)drainAndRefresh();
  armResponseDrain();
  return true;
}

void BtopTab::onVtermOutput(const char* data, std::size_t size, void* user) {
  static_cast<BtopTab*>(user)->writePty(data, size);
}

int BtopTab::onVtermDamage(VTermRect /*rect*/, void* user) {
  static_cast<BtopTab*>(user)->m_screenDirty = true;
  return 1;
}

int BtopTab::onVtermCursor(VTermPos pos, VTermPos /*oldPos*/, int visible, void* user) {
  auto* tab = static_cast<BtopTab*>(user);
  const bool wasVisible = tab->m_cursorVisible;
  const bool cursorChanged = tab->m_cursorRow != pos.row || tab->m_cursorCol != pos.col || wasVisible != (visible != 0);
  tab->m_cursorRow = pos.row;
  tab->m_cursorCol = pos.col;
  tab->m_cursorVisible = visible != 0;
  if (cursorChanged && (wasVisible || tab->m_cursorVisible) && tab->ownsKeyboardFocus()) {
    tab->m_screenDirty = true;
  }
  return 1;
}

int BtopTab::onVtermProperty(VTermProp /*prop*/, VTermValue* /*value*/, void* /*user*/) { return 1; }
