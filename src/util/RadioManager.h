#pragma once
#include <HalPowerManager.h>

#include <cstdint>
#include <memory>

/**
 * Manages WiFi/BLE radio coexistence on ESP32-C3.
 * The radio is shared — WiFi and BLE cannot run simultaneously.
 * Call ensureWifi() before any WiFi operation and ensureBle() before any BLE operation.
 */
class RadioManager {
 public:
  enum class RadioState { OFF, WIFI, BLE, BLE_HID_HOST };

  static RadioManager& getInstance() {
    static RadioManager instance;
    return instance;
  }

  // Ensure WiFi is available (deinits BLE if active)
  bool ensureWifi();

  // Ensure BLE peripheral/advertising mode is available (deinits WiFi if
  // active). deviceName only takes effect when BLE actually (re)initializes
  // here — a no-op call (state is already BLE) keeps whatever name is
  // already advertising.
  bool ensureBle(const char* deviceName = "panda");

  // Ensure the BLE HID host (central role — pairs with an external keyboard
  // via freeink::BleKeyboardHost, backed by NimBLE-Arduino, independent of
  // ESP-IDF's own BT host selection used by ensureBle()) is available
  // (deinits WiFi/BLE peripheral mode if active). Owns only the state
  // transition; the actual NimBLE init/deinit is delegated to
  // BleKeyboardHost::begin()/end() so there is a single owner of that
  // lifecycle instead of RadioManager also touching the radio directly, the
  // way it does for ensureBle()'s raw BLEDevice calls.
  bool ensureBleHidHost(const char* hostName = "panda");

  // Call every loop() tick while the BLE HID host is in use (e.g. from
  // KeyboardEntryActivity/BleKeyboardPairingActivity): drains BleKeyboardHost's
  // own poll() and holds a HalPowerManager::Lock for as long as a peripheral is
  // actually connected. Idle CPU-frequency scaling only exempts WiFi, not BLE
  // (see BangleGadgetbridgeServer's powerLock, which hit this same failure
  // mode first) -- a frequency drop mid-connection desyncs the BLE
  // controller's connection-interval timing, observed on real hardware as the
  // keyboard silently dropping after ~a minute idle (HalPowerManager's
  // IDLE_POWER_SAVING_MS) and NimBLE later failing to cleanly terminate the
  // already-desynced link ("ble_hs_stop: failed to terminate connection").
  // No-op when the BLE HID host isn't the active radio.
  void pollBleHidHost();

  // Shut down all radios
  void shutdown();

  RadioState getState() const { return state; }

  // Check if disclaimer has been acknowledged (stored in NVS)
  bool isDisclaimerAcknowledged() const;
  void setDisclaimerAcknowledged();

 private:
  RadioManager() = default;
  RadioState state = RadioState::OFF;
  std::unique_ptr<HalPowerManager::Lock> bleHidPowerLock;

  void deinitWifi();
  void deinitBle();
  void deinitBleHidHost();
};

#define RADIO RadioManager::getInstance()
