#pragma once
#include <cstdint>

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

  // Shut down all radios
  void shutdown();

  RadioState getState() const { return state; }

  // Check if disclaimer has been acknowledged (stored in NVS)
  bool isDisclaimerAcknowledged() const;
  void setDisclaimerAcknowledged();

 private:
  RadioManager() = default;
  RadioState state = RadioState::OFF;

  void deinitWifi();
  void deinitBle();
  void deinitBleHidHost();
};

#define RADIO RadioManager::getInstance()
