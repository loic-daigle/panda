#include "BangleGadgetbridgeServer.h"

#include <Arduino.h>
#include <sys/time.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include <HalClock.h>
#include <Logging.h>
#include <NimBLEDevice.h>

#include "CrossPointSettings.h"
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

// NimBLE-Arduino's NimBLEServerCallbacks unifies what the classic split
// BLEServerCallbacks/BLESecurityCallbacks API needed two separate classes
// for -- connect/disconnect AND onAuthenticationComplete all live here now,
// each handed a NimBLEConnInfo with the conn_handle and security state
// directly, no ble_gap_conn_desc pointer juggling or dual onConnect
// overloads required.
class BangleGadgetbridgeServer::ServerCallbacks : public NimBLEServerCallbacks {
  BangleGadgetbridgeServer& server;

 public:
  explicit ServerCallbacks(BangleGadgetbridgeServer& s) : server(s) {}

  // Carries the conn_handle we need to proactively start pairing. Relying on
  // Android to trigger pairing itself the first time Gadgetbridge writes to
  // an *_ENC characteristic was an unverified assumption (see start()); on
  // real hardware that write is simply rejected and nothing happens, so the
  // phone never gets prompted to pair. NimBLEDevice::startSecurity() is the
  // NimBLE equivalent of the flowe-os-proven "peripheral kicks off its own
  // pairing" pattern this app's plan called out as needed.
  //
  // Flag only, like every other BLE callback here -- an earlier version
  // called the security-start call directly from this callback (BLE host
  // task), which re-enters the NimBLE host stack from inside its own event
  // dispatch. That's the prime suspect for a heap-corruption crash
  // (multi_heap_free assert) seen on real hardware, surfacing later on an
  // unrelated free(). poll() (main task) does the actual call.
  void onConnect(NimBLEServer*, NimBLEConnInfo& connInfo) override {
    server.deviceConnected = true;
    server.pendingConnect = true;
    server.pendingSecurityConnHandle = connInfo.getConnHandle();
    server.currentConnHandle = connInfo.getConnHandle();
    server.pendingSecurityStart = true;
    server.owner.requestUpdate();
    LOG_DBG("BGB", "Gadgetbridge connected");
  }

  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    // Flag only -- stop() (BLE teardown) must run on the main task, not
    // here. poll() picks this up on its next tick.
    server.deviceConnected = false;
    server.pendingDisconnect = true;
    server.owner.requestUpdate();
    LOG_DBG("BGB", "Gadgetbridge disconnected");
  }

  // No PIN/passkey UI on this device (IO_CAP_NONE => "Just Works" pairing),
  // so the NimBLEServerCallbacks base class defaults for onPassKeyDisplay /
  // onPassKeyEntry / onConfirmPassKey are fine as-is.
  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    LOG_DBG("BGB", "BLE authentication complete: encrypted=%d bonded=%d", connInfo.isEncrypted(),
            connInfo.isBonded());
  }
};

class BangleGadgetbridgeServer::RxCallbacks : public NimBLECharacteristicCallbacks {
  BangleGadgetbridgeServer& server;

 public:
  explicit RxCallbacks(BangleGadgetbridgeServer& s) : server(s) {}

  void onWrite(NimBLECharacteristic* characteristic, NimBLEConnInfo&) override {
    NimBLEAttValue value = characteristic->getValue();
    if (value.length() == 0) return;
    LOG_DBG("BGB", "RX write: %u bytes", (unsigned)value.length());

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

BangleGadgetbridgeServer::BangleGadgetbridgeServer(Activity& owner) : owner(owner) {}

// Out-of-line so the unique_ptr members can destroy ServerCallbacks/
// RxCallbacks, which are only forward-declared in the header (see
// BangleGadgetbridgeServer.h).
BangleGadgetbridgeServer::~BangleGadgetbridgeServer() = default;

void BangleGadgetbridgeServer::start(const char* deviceName) {
  if (!rxMutex) rxMutex = xSemaphoreCreateMutex();
  rxBuffer.clear();
  deviceConnected = false;
  pendingConnect = false;
  pendingDisconnect = false;
  pendingSecurityStart = false;

  RADIO.ensureBle(deviceName);

  // Every method used below is a static NimBLEDevice method -- no
  // per-instance security object to allocate/leak (the earlier classic-API
  // code `new`'d a BLESecurity-adjacent callbacks object per start() with
  // nothing ever freeing the previous one).
  //
  // Bonding on, MITM off, secure connections off (legacy Just Works, not
  // SC-only): IO_CAP_NONE => "Just Works" pairing since this device has no
  // way to show/enter a passkey. SC-only Just Works (sc=true) authenticated
  // but never reached encrypted=1 against a real Gadgetbridge/Android peer
  // (onAuthenticationComplete fired with encrypted=0 bonded=0, immediate
  // disconnect) -- legacy pairing is the broadly-compatible fallback.
  NimBLEDevice::setSecurityAuth(true, false, false);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

  pServer = NimBLEDevice::createServer();
  // Callback object is allocated once and reused across every start() (see
  // the member comment in the header) rather than `new`'d fresh each time
  // with nothing ever freeing the previous one. Also now the single place
  // authentication-complete is handled -- see the ServerCallbacks comment
  // above for why the classic split security-callbacks class is gone.
  if (!serverCallbacks) serverCallbacks = std::make_unique<ServerCallbacks>(*this);
  pServer->setCallbacks(serverCallbacks.get());

  NimBLEService* service = pServer->createService(kServiceUuid);

  // *_ENC properties (not setAccessPermissions(ESP_GATT_PERM_*_ENCRYPTED),
  // which is Bluedroid's mechanism) requiring encryption makes Android's own
  // BLE stack auto-trigger the system pairing dialog on Gadgetbridge's first
  // write attempt, rather than needing this device to proactively initiate
  // security itself (the latter is what flowe-os needed for iOS -- Android's
  // stack handles this differently, so that extra pump isn't ported here;
  // verify this assumption on a real Gadgetbridge sync).
  pRxChar = service->createCharacteristic(
      kRxCharUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC);
  if (!rxCallbacks) rxCallbacks = std::make_unique<RxCallbacks>(*this);
  pRxChar->setCallbacks(rxCallbacks.get());

  pTxChar = service->createCharacteristic(
      kTxCharUuid, NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC);
  // No BLE2902 descriptor needed -- NimBLE manages the CCCD automatically
  // for NOTIFY/INDICATE characteristics.

  service->start();

  NimBLEAdvertising* advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUuid);
  advertising->enableScanResponse(true);
  advertising->start();
}

void BangleGadgetbridgeServer::stop() {
  // NimBLEDevice::deinit() (inside RADIO.shutdown() below) immediately
  // deletes the NimBLEServer/NimBLEService/NimBLECharacteristic objects and
  // stops the NimBLE host, with no regard for whether a peer is still
  // connected. That's fine
  // when we get here via a real disconnect (deviceConnected is already
  // false by then), but calling it while still connected -- e.g. the user
  // hits Back/Cancel mid-SYNC_CONNECTED -- races deinit against whatever the
  // NimBLE host task is still doing for that live connection, and was the
  // cause of a heap-corruption crash (multi_heap_free assert) seen on real
  // hardware. Requesting a disconnect first and giving the host task a brief
  // window to actually process it (deviceConnected flips false from
  // ServerCallbacks::onDisconnect, BLE task) avoids that race.
  if (deviceConnected && pServer) {
    pServer->disconnect(currentConnHandle);
    unsigned long deadline = millis() + 300;
    while (deviceConnected && millis() < deadline) delay(10);
  }

  RADIO.shutdown();  // tears down advertising + the whole BLE stack (deinit)
  pServer = nullptr;
  pRxChar = nullptr;
  pTxChar = nullptr;
  deviceConnected = false;
  pendingConnect = false;
  pendingDisconnect = false;
  pendingSecurityStart = false;
  powerLock.reset();  // safety net if stop() is called without a clean disconnect
}

void BangleGadgetbridgeServer::send(const std::string& jsonLine) {
  if (!deviceConnected || !pTxChar) {
    LOG_DBG("BGB", "send() dropped (connected=%d, txChar=%p): %s", deviceConnected, (void*)pTxChar, jsonLine.c_str());
    return;
  }
  // Gadgetbridge's Bangle.js line-splitter (onCharacteristicChanged in
  // BangleJSDeviceSupport.java) does `receivedLine.substring(0, p-1)` where p
  // is the index of '\n' -- i.e. it unconditionally drops the character
  // right before the newline, assuming a real Bangle.js/Espruino device's
  // console always ends lines with "\r\n" and that's the '\r' being
  // stripped. A bare '\n' (no '\r') makes it drop the *last real character*
  // of our JSON instead -- e.g. the closing '}' -- which is exactly what
  // produced "malformed json from Bangle.js: unterminated object" on real
  // hardware. Match the real protocol's line ending, not just what looks
  // sufficient as a delimiter.
  std::string packet = jsonLine;
  packet += "\r\n";

  // Nordic UART notifications are capped at (ATT MTU - 3) bytes per packet.
  // notify() does NOT chunk an oversized payload for us -- it just logs a
  // "Truncating" warning and hands the whole buffer to the NimBLE host,
  // which drops everything past the MTU limit on the wire rather than
  // sending it as follow-up packets. Real Bangle.js firmware sends
  // multi-packet replies chunked to MTU size for exactly this reason, and
  // Gadgetbridge's Bangle.js support reassembles our notifications the same
  // way we reassemble its writes (see poll(): buffer raw bytes, split on
  // '\n'). Without this, any reply longer than one MTU's worth reached
  // Gadgetbridge as a truncated fragment -- seen on real hardware as
  // "malformed json from Bangle.js: unterminated object".
  uint16_t mtu = pServer ? pServer->getPeerMTU(currentConnHandle) : 0;
  size_t chunkSize = (mtu > 3) ? static_cast<size_t>(mtu - 3) : 20;

  LOG_DBG("BGB", "TX notify (%u bytes, mtu=%u, chunk=%u): %s", (unsigned)packet.size(), (unsigned)mtu,
          (unsigned)chunkSize, jsonLine.c_str());

  for (size_t offset = 0; offset < packet.size(); offset += chunkSize) {
    size_t len = std::min(chunkSize, packet.size() - offset);
    pTxChar->setValue(reinterpret_cast<const uint8_t*>(packet.data() + offset), len);
    pTxChar->notify();
  }
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
  // Keep CPU at normal speed for as long as the BLE link is up -- see the
  // powerLock member comment. Done here (main task), not in ServerCallbacks,
  // since acquiring/releasing the lock does real work (mutex + frequency
  // change), not just a flag flip.
  if (deviceConnected) {
    // Retry every tick until actually held, not just once on the connect
    // transition: HalPowerManager::Lock has a single mutex slot shared with
    // the display renderer's own brief per-render Lock, so our attempt can
    // lose that race at the exact instant we try (observed on real hardware:
    // "Lock already held, ignore" logged right as Gadgetbridge connects).
    // Treating a lost race as permanent (the old `!powerLock` check) meant
    // CPU frequency kept oscillating for the entire connection -- the known
    // cause of BLE connection-interval desync that makes the phone silently
    // drop the link (observed as Gadgetbridge showing "reconnecting" while
    // this side still believed it was connected).
    if (!powerLock || !powerLock->held()) {
      powerLock = std::make_unique<HalPowerManager::Lock>();
    }
  } else if (!deviceConnected && powerLock) {
    powerLock.reset();
  }

  // Deferred from ServerCallbacks::onConnect (BLE host task) -- see that
  // callback's comment for why ble_gap_security_initiate() must not be
  // called from there directly.
  if (pendingSecurityStart) {
    pendingSecurityStart = false;
    int rc = 0;
    bool ok = NimBLEDevice::startSecurity(pendingSecurityConnHandle, &rc);
    LOG_DBG("BGB", "startSecurity(connHandle=%d) -> %d (rc=%d)", pendingSecurityConnHandle, ok, rc);
  }

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

  LOG_DBG("BGB", "RX line (%u chars): %s", (unsigned)line.size(), line.substr(0, 120).c_str());

  // Sent on every connect (BangleJSDeviceSupport.transmitTime()), not wrapped
  // in GB(...) -- it's meant to be eval()'d by a real Espruino REPL, not
  // parsed as JSON. Handled here (protocol-level, shared by every activity
  // built on this class) rather than surfaced through onLine.
  if (line.rfind("setTime(", 0) == 0) {
    handleSetTimeCommand(line);
    return;
  }

  size_t gbPos = line.find("GB(");
  if (gbPos == std::string::npos) return;  // other app queries -- not our concern

  if (onLine) onLine(line.substr(gbPos + 3));
}

// Parses a line like:
//   setTime(1786299193);E.setTimeZone(-4.0);(s=>s&&(...))(...)
// Real Bangle.js firmware eval()s this whole thing as JS; we only care about
// the two numbers, so pull them out directly rather than pattern-matching
// the surrounding call syntax.
void BangleGadgetbridgeServer::handleSetTimeCommand(const std::string& line) {
  size_t timePos = line.find("setTime(");
  if (timePos == std::string::npos) return;
  time_t epoch = static_cast<time_t>(strtol(line.c_str() + timePos + 8, nullptr, 10));
  if (epoch <= 0) return;

  // Update the system clock immediately -- CalendarActivity's "today"/"now"
  // comparisons read time(nullptr) directly, and nothing else in this
  // codebase syncs the system clock from the hardware RTC at boot (only from
  // WiFi NTP, which a BLE-only sync session never touches).
  struct timeval tv = {.tv_sec = epoch, .tv_usec = 0};
  settimeofday(&tv, nullptr);
  // Persist to the hardware RTC too, same as an NTP sync would, so the time
  // survives a reboot rather than reverting to whatever the RTC last held.
  halClock.setTimeUtc(epoch);
  LOG_DBG("BGB", "Synced clock from phone: epoch=%ld", static_cast<long>(epoch));

  size_t tzPos = line.find("setTimeZone(");
  if (tzPos == std::string::npos) return;
  double tzHours = strtod(line.c_str() + tzPos + 12, nullptr);

  // Same biased-quarter-hour encoding as ClockOffsetActivity's manual UTC
  // offset setting (48 = UTC+0, 15 minutes per step) -- reuse that setting
  // rather than adding a separate timezone store, so every other clock
  // display in the app (status bar, ClockActivity, ...) picks this up too.
  long biasedQuarter = lround(tzHours * 4.0) + 48;
  if (biasedQuarter < 0) biasedQuarter = 0;
  if (biasedQuarter > 104) biasedQuarter = 104;
  if (SETTINGS.clockUtcOffsetQ != static_cast<uint8_t>(biasedQuarter)) {
    SETTINGS.clockUtcOffsetQ = static_cast<uint8_t>(biasedQuarter);
    SETTINGS.saveToFile();
  }
  LOG_DBG("BGB", "Synced timezone from phone: %.2fh -> offsetQ=%ld", tzHours, biasedQuarter);
}
