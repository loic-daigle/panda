#include "BangleGadgetbridgeServer.h"

#if !defined(CONFIG_NIMBLE_ENABLED)
#include <BLE2902.h>
#endif
#include <BLEDevice.h>
#include <BLEServer.h>
#include <Logging.h>

#include "activities/Activity.h"
#include "util/RadioManager.h"

namespace {
// Nordic UART Service -- the exact GATT shape Gadgetbridge's Bangle.js
// support (and Espruino's own UART-over-BLE convention) expects: one write
// characteristic the phone sends commands on, one notify characteristic for
// replies. Confirmed to also be the shape a sibling project on this same
// hardware/SDK family (andrewjiang/flowe-os) already ships successfully for
// its own (non-Gadgetbridge) phone-companion protocol.
constexpr const char* kServiceUuid = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr const char* kRxCharUuid = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
constexpr const char* kTxCharUuid = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
}  // namespace

class BangleGadgetbridgeServer::ServerCallbacks : public BLEServerCallbacks {
  BangleGadgetbridgeServer& server;

 public:
  explicit ServerCallbacks(BangleGadgetbridgeServer& s) : server(s) {}

  void onConnect(BLEServer*) override {
    server.deviceConnected = true;
    server.pendingConnect = true;
    server.owner.requestUpdate();
    LOG_DBG("BGB", "Gadgetbridge connected");
  }

  void onDisconnect(BLEServer*) override {
    // Flag only -- stop() (BLE teardown) must run on the main task, not
    // here. poll() picks this up on its next tick.
    server.deviceConnected = false;
    server.pendingDisconnect = true;
    server.owner.requestUpdate();
    LOG_DBG("BGB", "Gadgetbridge disconnected");
  }
};

class BangleGadgetbridgeServer::RxCallbacks : public BLECharacteristicCallbacks {
  BangleGadgetbridgeServer& server;

 public:
  explicit RxCallbacks(BangleGadgetbridgeServer& s) : server(s) {}

  void onWrite(BLECharacteristic* characteristic) override {
    String value = characteristic->getValue();
    if (value.length() == 0) return;

    xSemaphoreTake(server.rxMutex, portMAX_DELAY);
    if (server.rxBuffer.size() + value.length() <= BangleGadgetbridgeServer::MAX_RX_BUFFER) {
      server.rxBuffer.append(value.c_str(), value.length());
    } else {
      // Overflow guard (a stalled main loop, or a chatty non-calendar
      // stream): drop and reset rather than growing unbounded.
      server.rxBuffer.clear();
    }
    xSemaphoreGive(server.rxMutex);
  }
};

class BangleGadgetbridgeServer::SecurityCallbacks : public BLESecurityCallbacks {
 public:
  // This board's sdkconfig builds the BLE stack on NimBLE, not Bluedroid
  // (confirmed via framework-arduinoespressif32-libs/esp32c3/sdkconfig:
  // CONFIG_NIMBLE_ENABLED=y) -- despite the "BLEDevice"/"BLEServer" class
  // names, which are a compatibility facade over either backend depending
  // on that build-time config. NimBLE's callback carries a ble_gap_conn_desc,
  // not Bluedroid's esp_ble_auth_cmpl_t.
  void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
    if (!desc) return;
    LOG_DBG("BGB", "BLE authentication complete: encrypted=%d bonded=%d", desc->sec_state.encrypted,
            desc->sec_state.bonded);
  }
  // No PIN/passkey UI on this device (IO_CAP_NONE => "Just Works" pairing),
  // so the BLESecurityCallbacks base class defaults for onPassKeyRequest /
  // onSecurityRequest / onConfirmPIN / onAuthorizationRequest are fine as-is.
};

BangleGadgetbridgeServer::BangleGadgetbridgeServer(Activity& owner) : owner(owner) {}

void BangleGadgetbridgeServer::start(const char* deviceName) {
  if (!rxMutex) rxMutex = xSemaphoreCreateMutex();
  rxBuffer.clear();
  deviceConnected = false;
  pendingConnect = false;
  pendingDisconnect = false;

  RADIO.ensureBle(deviceName);

  BLESecurity* security = new BLESecurity();
  // Bonding on, MITM off, secure connections on; IO_CAP_NONE => "Just Works"
  // pairing since this device has no way to show/enter a passkey. Config
  // proven working for phone pairing on this same hardware/SDK family by
  // flowe-os's companion BLE service.
  security->setAuthenticationMode(true, false, true);
  security->setCapability(ESP_IO_CAP_NONE);
  security->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  security->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  BLEDevice::setSecurityCallbacks(new SecurityCallbacks());

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks(*this));

  BLEService* service = pServer->createService(kServiceUuid);

  // PROPERTY_*_ENC (not setAccessPermissions(ESP_GATT_PERM_*_ENCRYPTED),
  // which is Bluedroid's mechanism -- this board's sdkconfig builds on
  // NimBLE, see the SecurityCallbacks comment above) requiring encryption
  // makes Android's own BLE stack auto-trigger the system pairing dialog on
  // Gadgetbridge's first write attempt, rather than needing this device to
  // proactively initiate security itself (the latter is what flowe-os
  // needed for iOS -- Android's stack handles this differently, so that
  // extra pump isn't ported here; verify this assumption on a real
  // Gadgetbridge sync).
  pRxChar = service->createCharacteristic(kRxCharUuid, BLECharacteristic::PROPERTY_WRITE |
                                                            BLECharacteristic::PROPERTY_WRITE_NR |
                                                            BLECharacteristic::PROPERTY_WRITE_ENC);
  pRxChar->setCallbacks(new RxCallbacks(*this));

  pTxChar = service->createCharacteristic(
      kTxCharUuid, BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ |
                       BLECharacteristic::PROPERTY_READ_ENC);
#if !defined(CONFIG_NIMBLE_ENABLED)
  pTxChar->addDescriptor(new BLE2902());
#endif

  service->start();

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUuid);
  advertising->setScanResponse(true);
  advertising->start();
}

void BangleGadgetbridgeServer::stop() {
  RADIO.shutdown();  // tears down advertising + the whole BLE stack (deinit)
  pServer = nullptr;
  pRxChar = nullptr;
  pTxChar = nullptr;
  deviceConnected = false;
  pendingConnect = false;
  pendingDisconnect = false;
}

bool BangleGadgetbridgeServer::consumeJustConnected() {
  if (!pendingConnect) return false;
  pendingConnect = false;
  return true;
}

bool BangleGadgetbridgeServer::consumePendingDisconnect() {
  if (!pendingDisconnect) return false;
  pendingDisconnect = false;
  return true;
}

void BangleGadgetbridgeServer::poll() {
  std::string chunk;
  {
    xSemaphoreTake(rxMutex, portMAX_DELAY);
    size_t lastNewline = rxBuffer.find_last_of('\n');
    if (lastNewline != std::string::npos) {
      chunk = rxBuffer.substr(0, lastNewline);
      rxBuffer.erase(0, lastNewline + 1);
    }
    xSemaphoreGive(rxMutex);
  }
  if (chunk.empty()) return;

  size_t start = 0;
  while (start <= chunk.size()) {
    size_t nl = chunk.find('\n', start);
    std::string line = (nl == std::string::npos) ? chunk.substr(start) : chunk.substr(start, nl - start);
    if (!line.empty()) handleLine(line);
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
}

void BangleGadgetbridgeServer::handleLine(const std::string& rawLine) {
  std::string line = rawLine;
  if (!line.empty() && line.front() == '\x10') line.erase(0, 1);
  while (!line.empty() && line.back() == '\r') line.pop_back();

  size_t gbPos = line.find("GB(");
  if (gbPos == std::string::npos) return;  // setTime(), app queries, etc. -- not our concern

  if (onLine) onLine(line.substr(gbPos + 3));
}
