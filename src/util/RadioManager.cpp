#include "RadioManager.h"

#include <Logging.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WiFi.h>

bool RadioManager::ensureWifi() {
  if (state == RadioState::WIFI) return true;

  if (state == RadioState::BLE) {
    deinitBle();
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
  }

  NimBLEDevice::init(deviceName);
  state = RadioState::BLE;
  LOG_DBG("RADIO", "Switched to BLE mode, heap: %d", ESP.getFreeHeap());
  return true;
}

void RadioManager::shutdown() {
  if (state == RadioState::WIFI) deinitWifi();
  if (state == RadioState::BLE) deinitBle();
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
