#include "NotificationsActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

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

    if (millis() - lastSpinnerUpdate >= 600) {
      lastSpinnerUpdate = millis();
      spinnerFrame = (spinnerFrame + 1) % 3;
      requestUpdate();
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

    if (state == SYNC_ADVERTISING && millis() - syncStartMs > SYNC_TIMEOUT_MS) {
      syncResultMessage = "No connection - check Gadgetbridge and try again";
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

std::string NotificationsActivity::relativeTimeLabel(time_t receivedAt) {
  time_t now = time(nullptr);
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
    localtime_r(&receivedAt, &t);
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

// Rounded "Sync with Phone" pill above a flat, newest-first list of received
// notifications -- same visual language as CalendarActivity/WikipediaActivity/
// DuckDuckGoActivity.
void NotificationsActivity::renderListState() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int pad = metrics.contentSidePadding;
  const int listTop = metrics.topPadding + metrics.headerHeight + 6;
  const int listBottom = pageHeight - metrics.buttonHintsHeight;
  const int listH = listBottom - listTop;

  const int pillH = 40;
  const int rowH = 44;

  const int totalHeight = pillH + data.entryCount * rowH;
  const int selectedTop = selectedIndex == 0 ? 0 : pillH + (selectedIndex - 1) * rowH;
  const int selectedHeight = selectedIndex == 0 ? pillH : rowH;

  int scrollTop = 0;
  if (totalHeight > listH) {
    scrollTop = selectedTop + selectedHeight / 2 - listH / 2;
    scrollTop = std::max(0, std::min(scrollTop, totalHeight - listH));
  }

  {
    const int y = listTop - scrollTop;
    if (y + pillH >= listTop && y <= listBottom) {
      const bool selected = selectedIndex == 0;
      const int pillY = y + 4;
      const int pillHeight = pillH - 8;
      if (selected) {
        renderer.fillRoundedRect(pad, pillY, pageWidth - pad * 2, pillHeight, 10, Color::Black);
      } else {
        renderer.drawRoundedRect(pad, pillY, pageWidth - pad * 2, pillHeight, 2, 10, true);
      }
      renderer.drawText(UI_10_FONT_ID, pad + 14, pillY + 9, "Sync with Phone", !selected, EpdFontFamily::BOLD);
    }
  }

  // Newest first: walk data.entries back-to-front (entries are stored
  // oldest-first, see addNotification).
  for (int i = 0; i < data.entryCount; i++) {
    const int entryIdx = data.entryCount - 1 - i;
    const NotificationEntry& n = data.entries[entryIdx];
    const int y = listTop + pillH + i * rowH - scrollTop;
    if (y + rowH < listTop || y > listBottom) continue;

    const bool selected = (i + 1) == selectedIndex;
    if (selected) renderer.fillRect(0, y, pageWidth, rowH, true);

    const int maxW = pageWidth - pad * 2;
    char titleLine[80];
    if (n.source[0] != '\0') {
      snprintf(titleLine, sizeof(titleLine), "%s - %s", n.source, n.title);
    } else {
      snprintf(titleLine, sizeof(titleLine), "%s", n.title);
    }
    std::string title = renderer.truncatedText(UI_10_FONT_ID, titleLine, maxW);
    renderer.drawText(UI_10_FONT_ID, pad, y + 4, title.c_str(), !selected);

    std::string subtitle = renderer.truncatedText(SMALL_FONT_ID, n.body[0] ? n.body : relativeTimeLabel(n.receivedAt).c_str(), maxW);
    renderer.drawText(SMALL_FONT_ID, pad, y + 4 + renderer.getLineHeight(UI_10_FONT_ID), subtitle.c_str(), !selected);
  }

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
