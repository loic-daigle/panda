#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class NimBLEHIDDevice;
class NimBLECharacteristic;
class NimBLEServer;

class BleKeyboardActivity final : public Activity {
 public:
  // Out-of-line (see .cpp): serverCallbacks holds a pointer to
  // ServerCallbacks, which is only forward-declared here. An inline
  // constructor would need ServerCallbacks complete too, to generate
  // exception-unwind code for that member -- so both ends of construction
  // live in the .cpp, same as BangleGadgetbridgeServer.
  explicit BleKeyboardActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~BleKeyboardActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }

 private:
  enum State { SELECT_SCRIPT, PREVIEW, ADVERTISING, PAIRED, EXECUTING, DONE };

  State state = SELECT_SCRIPT;
  ButtonNavigator buttonNavigator;

  // Script selection
  std::vector<std::string> scriptFiles;
  int selectedIndex = 0;

  // Script content
  std::string selectedScript;
  std::vector<std::string> scriptLines;
  int currentLine = 0;

  // BLE HID
  NimBLEServer* pServer = nullptr;
  NimBLEHIDDevice* pHid = nullptr;
  NimBLECharacteristic* pInputChar = nullptr;
  bool deviceConnected = false;

  // Execution
  unsigned long delayUntil = 0;
  std::string lastCommand;
  int repeatCount = 0;

  static constexpr const char* DUCKY_DIR = "/biscuit/ducky/";
  static constexpr const char* BLE_DEVICE_NAME = "panda. keyboard";

  void loadScriptList();
  void loadScriptContent(const std::string& path);
  bool startAdvertising();
  void stopAdvertising();
  void executeCurrentLine();
  void executeLine(const std::string& line);

  // DuckyScript command handlers
  void sendString(const std::string& text);
  void sendKey(uint8_t keyCode, uint8_t modifiers = 0);
  void sendKeyCombo(uint8_t modifiers, uint8_t keyCode);
  void releaseKeys();

  // Character to HID scan code mapping
  static uint8_t charToKeyCode(char c);
  static uint8_t charToModifier(char c);
  static uint8_t specialKeyCode(const std::string& keyName);
  static uint8_t modifierBit(const std::string& modName);

  // BLE server callbacks. Allocated once (lazily, in startAdvertising()) and
  // reused across every subsequent start/stop cycle -- see the member
  // comment on BangleGadgetbridgeServer::serverCallbacks for why: `new`ing a
  // fresh one every time with nothing ever freeing the previous one leaks a
  // small object per BadBLE session.
  class ServerCallbacks;
  std::unique_ptr<ServerCallbacks> serverCallbacks;
};
