#pragma once

#include "core/timer_manager.h"
#include "render/scene/input_area.h"
#include "shell/control_center/tab.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vterm.h>
class Flex;
class Label;
class Renderer;

class BtopTab final : public Tab {
public:
  BtopTab();
  ~BtopTab() override;

  std::unique_ptr<Flex> create() override;
  void setActive(bool active) override;
  void onClose() override;
  void onFrameTick(float deltaMs) override;
  // Starts the task manager before the panel is ever opened, so the first open shows live data.
  void preload();

  [[nodiscard]] bool ownsKeyboardFocus() const noexcept;
  // The terminal's input area; focusing it hands the keyboard to the task manager.
  [[nodiscard]] InputArea* focusArea() const noexcept;
  bool handleKey(std::uint32_t sym, std::uint32_t utf32, std::uint32_t modifiers, bool pressed, bool preedit);

private:
  void doLayout(Renderer& renderer, float contentWidth, float bodyHeight) override;
  void doUpdate(Renderer& renderer) override;

  void startProcess();
  void stopProcess();
  struct PtyDrainResult {
    bool activity = false;
    bool needsRefresh = false;
  };

  [[nodiscard]] PtyDrainResult drainPty();
  [[nodiscard]] bool drainAndRefresh();
  void armResponseDrain();
  void scheduleFallbackDrain();
  void resizeTerminal(int rows, int cols);
  void writePty(const char* data, std::size_t size);
  void writeMarkup();
  void setStatus(std::string message);
  void sendPointerMotion(float localX, float localY, std::uint32_t modifiers = 0);
  void sendPointerButton(const InputArea::PointerData& data);
  bool sendPointerAxis(const InputArea::PointerData& data);

  static void onVtermOutput(const char* data, std::size_t size, void* user);
  static int onVtermDamage(VTermRect rect, void* user);
  static int onVtermCursor(VTermPos pos, VTermPos oldPos, int visible, void* user);
  static int onVtermProperty(VTermProp prop, VTermValue* value, void* user);

  Flex* m_root = nullptr;
  Label* m_terminal = nullptr;
  Flex* m_titleRow = nullptr;
  VTerm* m_vterm = nullptr;
  VTermScreen* m_screen = nullptr;
  int m_ptyFd = -1;
  pid_t m_childPid = -1;
  int m_rows = 24;
  int m_cols = 80;
  bool m_sizeKnown = false;
  float m_cellWidth = 7.0F;
  float m_cellHeight = 12.0F;
  float m_fontSize = 9.0F;
  int m_cursorRow = 0;
  int m_cursorCol = 0;
  bool m_cursorVisible = false;
  bool m_active = false;
  bool m_screenDirty = true;
  std::chrono::steady_clock::time_point m_lastStart{};
  int m_responseDrainFramesRemaining = 0;
  int m_consecutiveEmptyDrains = 0;
  Timer m_fallbackDrainTimer;
};
