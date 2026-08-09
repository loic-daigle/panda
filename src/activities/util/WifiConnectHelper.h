#pragma once
#include <Logging.h>
#include <WiFi.h>

#include "WifiCredentialStore.h"

// Auto-connects to a saved network without pushing an interactive activity —
// used by background fetch tasks (Weather/Wikipedia/DuckDuckGo) that need
// WiFi but can't push activities from a non-render FreeRTOS task.
namespace WifiConnectHelper {

inline bool connectToDefaultWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  LOG_DBG("WIFI_HELP", "Powering on WiFi antenna...");
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  delay(150);

  String mac = WiFi.macAddress();
  mac.replace(":", "");
  String hostname = "CrossPoint-Reader-" + mac;
  WiFi.setHostname(hostname.c_str());

  WIFI_STORE.loadFromFile();
  const std::string lastSsid = WIFI_STORE.getLastConnectedSsid();
  if (!lastSsid.empty()) {
    const auto cred = WIFI_STORE.findCredential(lastSsid);
    if (cred) {
      LOG_DBG("WIFI_HELP", "Auto-connecting to saved SSID: %s", lastSsid.c_str());
      WiFi.persistent(false);
      if (!cred->password.empty()) {
        WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
      } else {
        WiFi.begin(cred->ssid.c_str());
      }

      int retries = 0;
      while (WiFi.status() != WL_CONNECTED && retries < 100) {  // 10 second timeout
        delay(100);
        retries++;
      }
      if (WiFi.status() == WL_CONNECTED) {
        LOG_DBG("WIFI_HELP", "Successfully connected to %s", lastSsid.c_str());
        return true;
      }
    }
  }

  // Try all other saved credentials
  const size_t credCount = WIFI_STORE.getCredentialCount();
  for (size_t i = 0; i < credCount; i++) {
    const auto cred = WIFI_STORE.getCredentialAt(i);
    if (!cred || cred->ssid == lastSsid) {
      continue;
    }
    LOG_DBG("WIFI_HELP", "Auto-connecting to other saved SSID: %s", cred->ssid.c_str());
    WiFi.persistent(false);
    if (!cred->password.empty()) {
      WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
    } else {
      WiFi.begin(cred->ssid.c_str());
    }

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 80) {  // 8 second timeout
      delay(100);
      retries++;
    }
    if (WiFi.status() == WL_CONNECTED) {
      LOG_DBG("WIFI_HELP", "Successfully connected to %s", cred->ssid.c_str());
      WIFI_STORE.setLastConnectedSsid(cred->ssid);
      return true;
    }
  }

  // Fallback: let the SDK try whatever it has in NVS
  LOG_DBG("WIFI_HELP", "Fallback begin connecting...");
  WiFi.begin();
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 80) {
    delay(100);
    retries++;
  }
  return WiFi.status() == WL_CONNECTED;
}

inline bool ensureWifiConnected() { return connectToDefaultWifi(); }

inline bool waitForTimeSync(int timeoutMs = 5000) {
  configTzTime("UTC0", "pool.ntp.org", "time.nist.gov");
  time_t now = time(nullptr);
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);
  int elapsed = 0;
  while (timeinfo.tm_year < (2020 - 1900) && elapsed < timeoutMs) {
    delay(100);
    elapsed += 100;
    now = time(nullptr);
    gmtime_r(&now, &timeinfo);
  }
  if (timeinfo.tm_year >= (2020 - 1900)) {
    return true;
  }
  LOG_ERR("WIFI_HELP", "NTP time sync timed out");
  return false;
}

}  // namespace WifiConnectHelper
