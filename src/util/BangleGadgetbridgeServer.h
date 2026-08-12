#pragma once

#include <HalPowerManager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

class Activity;
class NimBLEServer;
class NimBLECharacteristic;

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
  // Out-of-line (see .cpp): the unique_ptr members below hold pointers to
  // types (ServerCallbacks etc.) that are only forward-declared here, so the
  // implicit destructor can't be generated in this header.
  ~BangleGadgetbridgeServer();

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

  // Sends jsonLine (a raw JSON object, no envelope) as one notify packet plus
  // a trailing '\n', matching what real Bangle.js firmware sends back to
  // Gadgetbridge (e.g. {"t":"force_calendar_sync","ids":[...]}) -- unlike the
  // GB(...) wrapper Gadgetbridge uses for messages it sends us. No-op if not
  // connected. Call from the main task (onLine/poll), never a BLE callback.
  void send(const std::string& jsonLine);

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

  NimBLEServer* pServer = nullptr;
  NimBLECharacteristic* pRxChar = nullptr;
  NimBLECharacteristic* pTxChar = nullptr;

  volatile bool deviceConnected = false;
  volatile bool pendingConnect = false;
  volatile bool pendingDisconnect = false;

  // Set by ServerCallbacks::onConnect(NimBLEServer*, NimBLEConnInfo&) (BLE
  // host task) with the just-connected conn_handle; poll() (main task) picks
  // this up and calls NimBLEDevice::startSecurity() there. Must not call it
  // directly from the host-task callback: it was doing so previously and
  // that's the prime suspect for a heap-corruption crash (multi_heap_free
  // assert, triggered later on an unrelated disconnect) -- see class-level
  // comment above on why BLE callbacks here must stay flag-only.
  volatile bool pendingSecurityStart = false;
  uint16_t pendingSecurityConnHandle = 0;

  // Set alongside pendingSecurityConnHandle by the same onConnect callback;
  // read by stop() (main task) to disconnect the live peer before tearing
  // down the BLE stack -- see stop()'s comment for why that ordering matters.
  uint16_t currentConnHandle = 0;

  SemaphoreHandle_t rxMutex = nullptr;
  std::string rxBuffer;  // guarded by rxMutex; raw bytes appended by the BLE task

  // Held for the duration of an active connection so HalPowerManager's idle
  // CPU-frequency scaling (which only exempts WiFi, not BLE) can't drop the
  // clock mid-connection and desync the BLE controller's connection-interval
  // timing -- observed as Gadgetbridge pairing fine then dropping seconds
  // later. Created/destroyed from poll() (main task) by watching
  // deviceConnected, never from a BLE callback -- see the class-level comment
  // on why BLE callbacks here must stay flag-only.
  std::unique_ptr<HalPowerManager::Lock> powerLock;

  void handleLine(const std::string& rawLine);
  void handleSetTimeCommand(const std::string& line);

  class ServerCallbacks;
  class RxCallbacks;

  // Allocated once (lazily, in start()) and reused across every subsequent
  // start()/stop() cycle -- these used to be `new`'d fresh on every start()
  // with no matching delete anywhere, leaking a handful of small objects per
  // "Sync with Phone" attempt. Harmless on any one attempt, but real-device
  // testing showed free heap trending down across repeated attempts within
  // the same app session. Security config itself needs no such member: every
  // method this class calls (setSecurityAuth, setSecurityIOCap,
  // startSecurity, ...) is a static NimBLEDevice method, so start() calls
  // NimBLEDevice:: directly rather than allocating an instance at all.
  // (Authentication-complete handling now lives on ServerCallbacks itself --
  // NimBLEServerCallbacks carries an onAuthenticationComplete() hook,
  // unlike the classic split BLEServerCallbacks/BLESecurityCallbacks API --
  // so there's no separate security-callbacks object to hold either.)
  std::unique_ptr<ServerCallbacks> serverCallbacks;
  std::unique_ptr<RxCallbacks> rxCallbacks;
};
