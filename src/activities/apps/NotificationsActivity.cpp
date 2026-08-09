#include "NotificationsActivity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// ---- Storage ----

void NotificationsActivity::load() {
  memset(&data, 0, sizeof(data));
  auto file = Storage.open(SAVE_PATH);
  if (file) {
    file.read(reinterpret_cast<uint8_t*>(&data), sizeof(data));
    file.close();
  }
  if (data.entryCount < 0 || data.entryCount > MAX_NOTIFICATIONS) {
    data.entryCount = 0;  // corrupt/missing file guard
  }
}

void NotificationsActivity::save() {
  Storage.mkdir("/biscuit");
  auto file = Storage.open(SAVE_PATH, O_WRITE | O_CREAT | O_TRUNC);
  if (file) {
    file.write(reinterpret_cast<const uint8_t*>(&data), sizeof(data));
    file.close();
  }
}

// ---- Lifecycle ----

void NotificationsActivity::onEnter() {
  Activity::onEnter();
  load();
  state = LIST;
  selectedIndex = 0;
  requestUpdate();
}

void NotificationsActivity::onExit() {
  Activity::onExit();
  if (state == SYNC_ADVERTISING || state == SYNC_CONNECTED) {
    stopSync();
  }
}

// ---- BLE sync ----

void NotificationsActivity::startSync() {
  notificationsReceivedThisSync = 0;
  bleServer.start(BLE_DEVICE_NAME);
  syncStartMs = millis();
  state = SYNC_ADVERTISING;
}

void NotificationsActivity::stopSync() {
  save();
  bleServer.stop();
}

void NotificationsActivity::handleGbJson(const std::string& jsonPart) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, jsonPart);
  if (err) {
    LOG_DBG("NOTIF", "GB() payload parse error: %s", err.c_str());
    return;
  }

  const char* t = doc["t"] | "";
  LOG_DBG("NOTIF", "GB() message type: \"%s\"", t);
  if (strcmp(t, "notify") != 0) return;  // calendar/weather/etc. -- out of scope for this app

  uint32_t id = doc["id"].as<uint32_t>();
  const char* src = doc["src"] | "";
  const char* title = doc["title"] | "";
  // Gadgetbridge's notify payload carries both "subject" and "body" for some
  // sources (e.g. email); fall back through subject to whichever is present
  // so a notification never shows up blank.
  const char* body = doc["body"] | (doc["subject"] | "");
  if (title[0] == '\0') title = (doc["subject"] | "(no title)");

  addNotification(id, src, title, body);
  notificationsReceivedThisSync++;
  requestUpdate();
}

void NotificationsActivity::addNotification(uint32_t id, const char* source, const char* title, const char* body) {
  // Dedup by id in case Gadgetbridge resends on reconnect -- notifications
  // are otherwise append-only (no add/remove semantics like calendar events;
  // a phone notification is a one-shot alert, not an editable object).
  for (int i = 0; i < data.entryCount; i++) {
    if (data.entries[i].id == id) return;
  }

  if (data.entryCount >= MAX_NOTIFICATIONS) {
    // Drop the oldest (index 0) to make room; entries are stored oldest-first.
    for (int i = 0; i < data.entryCount - 1; i++) data.entries[i] = data.entries[i + 1];
    data.entryCount--;
  }

  NotificationEntry& entry = data.entries[data.entryCount++];
  entry.id = id;
  entry.receivedAt = time(nullptr);
  strncpy(entry.source, source, sizeof(entry.source) - 1);
  strncpy(entry.title, title, sizeof(entry.title) - 1);
  strncpy(entry.body, body, sizeof(entry.body) - 1);
}

// ---- Input ----

void NotificationsActivity::loop() {
  if (state == LIST) {
    const int itemCount = 1 + data.entryCount;

    buttonNavigator.onNext([this, itemCount] {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this, itemCount] {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
      requestUpdate();
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (selectedIndex == 0) {
        startSync();
      } else {
        state = DETAIL;
      }
      requestUpdate();
    }
    return;
  }

  if (state == DETAIL) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = LIST;
      requestUpdate();
    }
    return;
  }

  if (state == SYNC_ADVERTISING || state == SYNC_CONNECTED) {
    bleServer.poll();

    if (state == SYNC_ADVERTISING && bleServer.consumeJustConnected()) {
      state = SYNC_CONNECTED;
      notificationsReceivedThisSync = 0;
      requestUpdate();
    }

    // See CalendarActivity::loop() for why SYNC_CONNECTED slows this down --
    // a real-device capture of that screen showed free heap dropping from
    // tens of KB to ~1.5KB over ~14 minutes of continuous 600ms re-rendering.
    const unsigned long spinnerIntervalMs = (state == SYNC_CONNECTED) ? 3000 : 600;
    if (millis() - lastSpinnerUpdate >= spinnerIntervalMs) {
      lastSpinnerUpdate = millis();
      spinnerFrame = (spinnerFrame + 1) % 3;
      requestUpdate();
    }

    if (millis() - lastHeapLogMs >= 5000) {
      lastHeapLogMs = millis();
      LOG_DBG("NOTIF", "heap during sync: free=%u maxAlloc=%u", (unsigned)ESP.getFreeHeap(),
              (unsigned)ESP.getMaxAllocHeap());
    }

    if (bleServer.consumePendingDisconnect()) {
      char buf[48];
      if (notificationsReceivedThisSync > 0) {
        snprintf(buf, sizeof(buf), "Received %d notification(s)", notificationsReceivedThisSync);
      } else {
        snprintf(buf, sizeof(buf), "Disconnected - no notifications received");
      }
      syncResultMessage = buf;
      stopSync();
      state = SYNC_DONE;
      requestUpdate();
      return;
    }

    // Applies in both ADVERTISING and CONNECTED -- once connected this is
    // just "no notification happened to fire during the sync window", which
    // is normal, but the session should still time out on its own rather
    // than wait indefinitely with no way out but a phone-side disconnect.
    if (millis() - syncStartMs > SYNC_TIMEOUT_MS) {
      syncResultMessage = (state == SYNC_ADVERTISING) ? "No connection - check Gadgetbridge and try again"
                                                      : "Timed out - no notifications received";
      stopSync();
      state = SYNC_DONE;
      requestUpdate();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      syncResultMessage = "Cancelled";
      stopSync();
      state = SYNC_DONE;
      requestUpdate();
      return;
    }
    return;
  }

  if (state == SYNC_DONE) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      state = LIST;
      selectedIndex = 0;
      requestUpdate();
    }
    return;
  }
}

// ---- Rendering ----

void NotificationsActivity::localBrokenDownTime(time_t utcTimestamp, struct tm& out) {
  const int offsetQuarterHours = static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48;
  const time_t shifted = utcTimestamp + static_cast<time_t>(offsetQuarterHours) * 15 * 60;
  gmtime_r(&shifted, &out);
}

std::string NotificationsActivity::relativeTimeLabel(time_t receivedAt) {
  time_t now = time(nullptr);
  // Both sides are UTC epoch values, so their difference is a correct
  // elapsed duration regardless of timezone -- only the >24h fallback below
  // (an absolute clock time) needs the local-time conversion.
  long diffSec = static_cast<long>(now - receivedAt);
  if (diffSec < 0) diffSec = 0;

  char buf[24];
  if (diffSec < 60) {
    snprintf(buf, sizeof(buf), "just now");
  } else if (diffSec < 3600) {
    snprintf(buf, sizeof(buf), "%ld min ago", diffSec / 60);
  } else if (diffSec < 86400) {
    snprintf(buf, sizeof(buf), "%ld hr ago", diffSec / 3600);
  } else {
    struct tm t;
    localBrokenDownTime(receivedAt, t);
    snprintf(buf, sizeof(buf), "%02d:%02d %02d/%02d", t.tm_hour, t.tm_min, t.tm_mon + 1, t.tm_mday);
  }
  return buf;
}

void NotificationsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  if (state == LIST) {
    char subtitle[32];
    if (data.entryCount == 0) {
      snprintf(subtitle, sizeof(subtitle), "No notifications yet");
    } else {
      snprintf(subtitle, sizeof(subtitle), "%d notification%s", data.entryCount, data.entryCount == 1 ? "" : "s");
    }
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Notifications", subtitle);
  } else {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Notifications");
  }

  if (state == LIST) {
    renderListState();
  } else if (state == DETAIL) {
    renderDetailState();
  } else {
    renderSyncState();
  }

  renderer.displayBuffer();
}

// Uses the same GUI.drawList component every other list screen in this app
// is built on, including the just-modernized CalendarActivity (see there for
// the fuller rationale), rather than a bespoke fixed-row-height list.
void NotificationsActivity::renderListState() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int listTop = metrics.topPadding + metrics.headerHeight;
  const int listH = pageHeight - listTop - metrics.buttonHintsHeight;

  // Newest first: row i (1..N) maps to entries[entryCount - i] (entries are
  // stored oldest-first, see addNotification).
  auto entryFor = [this](int i) -> const NotificationEntry& { return data.entries[data.entryCount - i]; };

  const int itemCount = 1 + data.entryCount;
  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listH}, itemCount, selectedIndex,
      [this, entryFor](int i) -> std::string {
        if (i == 0) return "Sync with Phone";
        const NotificationEntry& n = entryFor(i);
        if (n.source[0] != '\0') return std::string(n.source) + " - " + n.title;
        return n.title;
      },
      [entryFor](int i) -> std::string {
        if (i == 0) return "Pulls notifications from Gadgetbridge";
        return entryFor(i).body;
      },
      nullptr,  // rowIcon
      [this, entryFor](int i) -> std::string {
        if (i == 0) return "";
        return relativeTimeLabel(entryFor(i).receivedAt);
      });

  const auto labels = mappedInput.mapLabels("Back", "Select", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void NotificationsActivity::renderDetailState() const {
  const int entryIdx = data.entryCount - selectedIndex;  // selectedIndex 1..N maps to newest-first display order
  if (selectedIndex <= 0 || entryIdx < 0 || entryIdx >= data.entryCount) return;
  const NotificationEntry& n = data.entries[entryIdx];

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pad = metrics.contentSidePadding;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  if (n.source[0] != '\0') {
    renderer.drawText(SMALL_FONT_ID, pad, y, n.source, true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(SMALL_FONT_ID) + 4;
  }

  auto titleLines = renderer.wrappedText(UI_10_FONT_ID, n.title, pageWidth - pad * 2, 3);
  for (const auto& line : titleLines) {
    renderer.drawText(UI_10_FONT_ID, pad, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_10_FONT_ID);
  }
  y += 6;

  renderer.drawLine(pad, y, pageWidth - pad, y, true);
  y += 12;

  if (n.body[0] != '\0') {
    auto bodyLines = renderer.wrappedText(UI_10_FONT_ID, n.body, pageWidth - pad * 2, 8);
    for (const auto& line : bodyLines) {
      renderer.drawText(UI_10_FONT_ID, pad, y, line.c_str());
      y += renderer.getLineHeight(UI_10_FONT_ID);
    }
    y += 8;
  }

  std::string when = relativeTimeLabel(n.receivedAt);
  renderer.drawText(SMALL_FONT_ID, pad, y, when.c_str());

  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void NotificationsActivity::renderSyncState() const {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int centerY = pageHeight / 2;

  if (state == SYNC_ADVERTISING) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - 50, "Waiting for phone...", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(SMALL_FONT_ID, centerY - 20, "Open Gadgetbridge and sync \"Bangle.js Panda\"");
    GUI.drawSpinner(renderer, pageWidth / 2, centerY + 30, nullptr, spinnerFrame);
    const auto labels = mappedInput.mapLabels("Cancel", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == SYNC_CONNECTED) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - 50, "Connected", true, EpdFontFamily::BOLD);
    char buf[40];
    snprintf(buf, sizeof(buf), "%d notification(s) received so far", notificationsReceivedThisSync);
    renderer.drawCenteredText(SMALL_FONT_ID, centerY - 20, buf);
    GUI.drawSpinner(renderer, pageWidth / 2, centerY + 30, nullptr, spinnerFrame);
    const auto labels = mappedInput.mapLabels("Done", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (state == SYNC_DONE) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, syncResultMessage.c_str(), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels("Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
}
