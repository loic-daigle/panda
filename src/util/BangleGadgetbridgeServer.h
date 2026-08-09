#pragma once

#include <cstddef>
#include <functional>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class Activity;
class BLEServer;
class BLECharacteristic;

// Shared BLE peripheral plumbing for any activity that emulates a Bangle.js
// smartwatch so Gadgetbridge (the Android app) talks to it: advertises the
// Nordic UART Service under a "Bangle.js ..." name, handles NimBLE bonding,
// and turns the raw bytes Gadgetbridge writes into newline-delimited
// `GB({...})` JSON lines. Knows nothing about calendar/notification message
// semantics -- callers get each line's JSON body via `onLine` and decide
// what to do with the `"t"` field themselves.
//
// Extracted from CalendarActivity and NotificationsActivity, which
// originally duplicated all of this verbatim (see git history if the split
// ever needs undoing). A third Bangle.js-protocol activity should just add
// another BangleGadgetbridgeServer member and an onLine handler.
//
// Hard-won lesson this class exists to encapsulate (from flowe-os, a sibling
// project on the same hardware family, docs/x4-core-learnings.md "BLE
// callback stability learning"): parsing JSON directly inside a BLE host-task
// callback caused a stack protection fault. This repo's board (ESP32-C3,
// single core, less stack headroom than the hardware that lesson was learned
// on) makes that *more* of a risk, not less. So the BLE callback classes
// (defined in the .cpp) only ever flip flags or append raw bytes to a
// mutex-guarded buffer -- all JSON parsing happens in `onLine`, invoked from
// `poll()` on the caller's own loop() (main task), never from a BLE callback.
class BangleGadgetbridgeServer {
 public:
  // owner: used only to call requestUpdate() when connection state changes,
  // so the owning Activity's UI refreshes promptly.
  explicit BangleGadgetbridgeServer(Activity& owner);

  static constexpr size_t MAX_RX_BUFFER = 4096;

  // Ensures BLE is active (via RadioManager), stands up the Nordic UART GATT
  // server + NimBLE bonding/security, and starts advertising as `deviceName`.
  void start(const char* deviceName);

  // Tears down advertising + the whole BLE stack (RadioManager::shutdown()).
  // Safe to call even if start() was never called.
  void stop();

  // Call every loop() tick while advertising/connected: drains the raw RX
  // buffer, reassembles newline-delimited lines, strips the Bangle.js
  // `\x10`/`GB(...)` envelope, and invokes onLine with each JSON body.
  void poll();

  bool isConnected() const { return deviceConnected; }

  // One-shot flags: true the first time observed after the underlying BLE
  // event, false on every call after that (and after start()/stop()).
  bool consumeJustConnected();
  bool consumePendingDisconnect();

  // Invoked from poll() (main task) with the text between "GB(" and the rest
  // of the line -- deserializeJson stops at the JSON object's closing brace
  // and ignores any trailing characters (e.g. the closing ")"), so callers
  // can deserializeJson() this directly.
  std::function<void(const std::string& jsonLine)> onLine;

 private:
  Activity& owner;

  BLEServer* pServer = nullptr;
  BLECharacteristic* pRxChar = nullptr;
  BLECharacteristic* pTxChar = nullptr;

  volatile bool deviceConnected = false;
  volatile bool pendingConnect = false;
  volatile bool pendingDisconnect = false;

  SemaphoreHandle_t rxMutex = nullptr;
  std::string rxBuffer;  // guarded by rxMutex; raw bytes appended by the BLE task

  void handleLine(const std::string& rawLine);

  class ServerCallbacks;
  class RxCallbacks;
  class SecurityCallbacks;
};
