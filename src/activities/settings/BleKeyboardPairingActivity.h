#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct Rect;
struct ThemeMetrics;

// Settings screen for pairing a physical BLE keyboard (or page-turner remote)
// with the device, via the SDK's BleKeyboardHost (NimBLE central role). Lists
// already-paired devices (for quick forget) plus HID peripherals seen during
// a scan, drives connect()/forget(), and shows the Just-Works pairing passkey
// while a connection is settling.
//
// Owns the BLE HID host radio for its lifetime only: onEnter() claims it via
// RadioManager::ensureBleHidHost(), onExit() releases it via
// RadioManager::shutdown() -- matches KeyboardEntryActivity's opt-in pattern,
// so leaving this screen never leaves the radio resident.
class BleKeyboardPairingActivity final : public Activity {
 public:
  explicit BleKeyboardPairingActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BleKeyboardPairing", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return scanning; }

 private:
  enum class State { SCANNING, LIST, CONNECTING, PASSKEY, CONNECTED, ERROR, FORGET_PROMPT };

  struct Entry {
    std::string name;
    std::string addr;
    int rssi = 0;
    bool paired = false;
  };

  State state = State::SCANNING;
  std::vector<Entry> entries;
  ButtonNavigator buttonNavigator;
  size_t selectedIndex = 0;
  bool scanning = false;

  std::string connectingName;
  std::string errorMessage;
  uint32_t passkey = 0;

  int forgetPromptSelection = 0;
  std::string forgetAddr;
  std::string forgetName;

  void startScan();
  void rebuildEntries();
  void connectToSelected();

  void renderList(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderConnecting(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderPasskey(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderConnected(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderError(const Rect* screen, const ThemeMetrics* metrics) const;
  void renderForgetPrompt(const Rect* screen, const ThemeMetrics* metrics) const;
};
