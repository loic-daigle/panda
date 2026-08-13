#include "RadioManager.h"

#include <BleKeyboardHost.h>
#include <Logging.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WiFi.h>

bool RadioManager::ensureWifi() {
  if (state == RadioState::WIFI) return true;

  if (state == RadioState::BLE) {
    deinitBle();
  } else if (state == RadioState::BLE_HID_HOST) {
    deinitBleHidHost();
  }

  WiFi.mode(WIFI_STA);
  state = RadioState::WIFI;
  LOG_DBG("RADIO", "Switched to WiFi mode, heap: %d", ESP.getFreeHeap());
  return true;
}

bool RadioManager::ensureBle(const char* deviceName) {
  if (state == RadioState::BLE) return true;

  if (state == RadioState::WIFI) {
    deinitWifi();
  } else if (state == RadioState::BLE_HID_HOST) {
    deinitBleHidHost();
  }

  NimBLEDevice::init(deviceName);
  state = RadioState::BLE;
  LOG_DBG("RADIO", "Switched to BLE mode, heap: %d", ESP.getFreeHeap());
  return true;
}

bool RadioManager::ensureBleHidHost(const char* hostName) {
  if (state == RadioState::BLE_HID_HOST) return true;

  if (state == RadioState::WIFI) {
    deinitWifi();
  } else if (state == RadioState::BLE) {
    deinitBle();
  }

  if (!BleHid.begin(hostName)) {
    LOG_ERR("RADIO", "BleKeyboardHost begin() failed, heap: %d", ESP.getFreeHeap());
    return false;
  }
  state = RadioState::BLE_HID_HOST;
  LOG_DBG("RADIO", "Switched to BLE HID host mode, heap: %d", ESP.getFreeHeap());
  return true;
}

void RadioManager::pollBleHidHost() {
  if (state != RadioState::BLE_HID_HOST) return;

  BleHid.poll();

  // Retry every tick until actually held, not just once on the connect
  // transition -- HalPowerManager::Lock has a single mutex slot shared with
  // the display renderer's own brief per-render Lock, so a single attempt can
  // lose that race at the exact instant a connection completes (same
  // reasoning as BangleGadgetbridgeServer::poll()'s powerLock handling).
  if (BleHid.isConnected()) {
    if (!bleHidPowerLock || !bleHidPowerLock->held()) {
      bleHidPowerLock = std::make_unique<HalPowerManager::Lock>();
    }
  } else if (bleHidPowerLock) {
    bleHidPowerLock.reset();
  }
}

void RadioManager::shutdown() {
  if (state == RadioState::WIFI) deinitWifi();
  if (state == RadioState::BLE) deinitBle();
  if (state == RadioState::BLE_HID_HOST) deinitBleHidHost();
  state = RadioState::OFF;
}

void RadioManager::deinitWifi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(50);
  LOG_DBG("RADIO", "WiFi deinitialized");
}

void RadioManager::deinitBle() {
  // clearAll=true: deinit(false) stops the NimBLE host but leaves the
  // NimBLEServer/NimBLEService/NimBLECharacteristic C++ objects allocated and
  // still registered. The next ensureBle() + createServer() call then reuses
  // that same stale NimBLEServer (createServer() only allocates a new one
  // when NimBLEDevice's singleton is null) while createService()/
  // createCharacteristic() unconditionally append brand-new duplicate
  // objects rather than replacing -- leaving the OLD ones (whose callbacks
  // point back into whatever Activity registered them, since destroyed)
  // still live in the GATT table. An already-bonded phone reconnecting can
  // then have a write routed to that stale characteristic, invoking a
  // virtual call through freed memory. Seen on real hardware as a Guru
  // Meditation Load access fault inside NimBLECharacteristic::writeEvent
  // (m_pCallbacks->onWrite on a destroyed callbacks object) right after a
  // second-in-the-same-boot Gadgetbridge BLE sync connected.
  NimBLEDevice::deinit(true);
  delay(50);
  LOG_DBG("RADIO", "BLE deinitialized");
}

void RadioManager::deinitBleHidHost() {
  bleHidPowerLock.reset();  // safety net if torn down without a clean disconnect

  // Wait for a real Link Layer disconnect before end() calls
  // NimBLEDevice::deinit() internally. end() only gives a stray still-
  // connected peer ~600ms before deiniting anyway; on a slow/lossy link
  // that's not always enough, and racing ahead into deinit() while the host
  // still considers the connection live makes NimBLE's own internal stop
  // timeout force-terminate it instead ("ble_hs_stop_terminate_timeout_cb, 1
  // connection(s) still up" / "ble_hs_stop: failed to terminate connection"),
  // which left the stack flaky for the next begin()/connect(). Done here via
  // BleKeyboardHost's public API (disconnect()/isConnected()) rather than
  // patching end()'s own wait, since freeink-sdk is a submodule we don't
  // modify.
  if (BleHid.isConnected()) {
    BleHid.disconnect();
    const uint32_t waitStart = millis();
    while (BleHid.isConnected() && millis() - waitStart < 2000) {
      delay(10);
    }
    delay(300);  // let DISCONNECTING settle to DISCONNECTED
  }

  BleHid.end();
  delay(50);
  LOG_DBG("RADIO", "BLE HID host deinitialized");
}

bool RadioManager::isDisclaimerAcknowledged() const {
  Preferences prefs;
  prefs.begin("biscuit", true);
  bool ack = prefs.getBool("disc_ack", false);
  prefs.end();
  return ack;
}

void RadioManager::setDisclaimerAcknowledged() {
  Preferences prefs;
  prefs.begin("biscuit", false);
  prefs.putBool("disc_ack", true);
  prefs.end();
}
