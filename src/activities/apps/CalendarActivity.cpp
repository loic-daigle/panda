#include "CalendarActivity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

// ---- Storage ----

void CalendarActivity::load() {
  memset(&data, 0, sizeof(data));
  auto file = Storage.open(SAVE_PATH);
  if (file) {
    file.read(reinterpret_cast<uint8_t*>(&data), sizeof(data));
    file.close();
  }
  if (data.eventCount < 0 || data.eventCount > MAX_EVENTS) {
    data.eventCount = 0;  // corrupt/missing file guard
  }
  rebuildSortedOrder();
}

void CalendarActivity::save() {
  Storage.mkdir("/biscuit");
  auto file = Storage.open(SAVE_PATH, O_WRITE | O_CREAT | O_TRUNC);
  if (file) {
    file.write(reinterpret_cast<const uint8_t*>(&data), sizeof(data));
    file.close();
  }
}

void CalendarActivity::rebuildSortedOrder() {
  sortedOrder.clear();
  sortedOrder.reserve(data.eventCount);
  for (int i = 0; i < data.eventCount; i++) sortedOrder.push_back(i);
  std::sort(sortedOrder.begin(), sortedOrder.end(),
            [this](int a, int b) { return data.events[a].timestamp < data.events[b].timestamp; });
}

// ---- Lifecycle ----

void CalendarActivity::onEnter() {
  Activity::onEnter();
  load();
  state = LIST;
  selectedIndex = 0;
  requestUpdate();
}

void CalendarActivity::onExit() {
  Activity::onExit();
  if (state == SYNC_ADVERTISING || state == SYNC_CONNECTED) {
    stopSync();
  }
}

// ---- BLE sync ----

void CalendarActivity::startSync() {
  eventsReceivedThisSync = 0;
  bleServer.start(BLE_DEVICE_NAME);
  syncStartMs = millis();
  state = SYNC_ADVERTISING;
}

void CalendarActivity::stopSync() {
  save();  // persist whatever arrived, even on a mid-sync cancel/timeout
  bleServer.stop();
  rebuildSortedOrder();
}

void CalendarActivity::handleGbJson(const std::string& jsonPart) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, jsonPart);
  if (err) {
    LOG_DBG("CAL", "GB() payload parse error: %s", err.c_str());
    return;
  }

  const char* t = doc["t"] | "";
  LOG_DBG("CAL", "GB() message type: \"%s\"", t);

  if (strcmp(t, "force_calendar_sync_start") == 0) {
    sendForceCalendarSync();
    return;
  }

  if (strcmp(t, "calendar-") == 0) {
    removeEvent(doc["id"].as<uint32_t>());
    requestUpdate();
    return;
  }

  if (strcmp(t, "calendar") != 0) return;  // notifications/weather/etc. -- out of scope for this app

  // .as<T>() (rather than `| 0`, which lets ArduinoJson deduce a plain `int`
  // from the literal fallback) keeps large/unsigned event ids from
  // truncating or going negative.
  uint32_t id = doc["id"].as<uint32_t>();
  int type = doc["type"].as<int>();
  time_t timestamp = static_cast<time_t>(doc["timestamp"].as<int64_t>());
  uint32_t durationSec = doc["durationInSeconds"].as<uint32_t>();
  const char* title = doc["title"] | "(untitled)";
  const char* location = doc["location"] | "";
  bool allDay = doc["allDay"] | false;

  upsertEvent(id, type, timestamp, durationSec, title, location, allDay);
  eventsReceivedThisSync++;
  requestUpdate();
}

void CalendarActivity::sendForceCalendarSync() {
  JsonDocument doc;
  doc["t"] = "force_calendar_sync";
  JsonArray ids = doc["ids"].to<JsonArray>();
  for (int i = 0; i < data.eventCount; i++) ids.add(data.events[i].id);

  std::string out;
  serializeJson(doc, out);
  bleServer.send(out);
}

void CalendarActivity::removeEvent(uint32_t id) {
  for (int i = 0; i < data.eventCount; i++) {
    if (data.events[i].id == id) {
      for (int j = i; j < data.eventCount - 1; j++) data.events[j] = data.events[j + 1];
      data.eventCount--;
      return;
    }
  }
}

void CalendarActivity::upsertEvent(uint32_t id, int type, time_t timestamp, uint32_t durationSec, const char* title,
                                    const char* location, bool allDay) {
  (void)type;  // event category (general/absence/birthday/alarm) -- not stored, not a delete flag
  int existing = -1;
  for (int i = 0; i < data.eventCount; i++) {
    if (data.events[i].id == id) {
      existing = i;
      break;
    }
  }

  CalendarEvent ev;
  ev.id = id;
  ev.timestamp = timestamp;
  ev.durationSec = durationSec;
  strncpy(ev.title, title, sizeof(ev.title) - 1);
  strncpy(ev.location, location, sizeof(ev.location) - 1);
  ev.allDay = allDay;

  if (existing >= 0) {
    data.events[existing] = ev;
    return;
  }

  if (data.eventCount >= MAX_EVENTS) {
    // Evict the earliest-timestamp slot to make room for the new one.
    int oldestIdx = 0;
    for (int i = 1; i < data.eventCount; i++) {
      if (data.events[i].timestamp < data.events[oldestIdx].timestamp) oldestIdx = i;
    }
    for (int i = oldestIdx; i < data.eventCount - 1; i++) data.events[i] = data.events[i + 1];
    data.eventCount--;
  }

  data.events[data.eventCount++] = ev;
}

// ---- Input ----

void CalendarActivity::loop() {
  if (state == LIST) {
    const int itemCount = 1 + static_cast<int>(sortedOrder.size());

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
      eventsReceivedThisSync = 0;
      requestUpdate();
    }

    // SYNC_CONNECTED can sit here for minutes waiting on Gadgetbridge's own
    // decision to send force_calendar_sync_start -- a full e-ink refresh
    // every 600ms for that whole wait is both hard on the display and (per a
    // real-device capture where free heap dropped from tens of KB to ~1.5KB
    // over ~14 minutes of continuous re-rendering, eventually failing to
    // parse the very message we were waiting for with a NoMemory error) a
    // prime suspect for whatever is slowly eating heap during the wait.
    // Advertising is normally short, so keep it snappy; slow the connected
    // wait down substantially as a mitigation while the real source is
    // narrowed down (see the heap log below).
    const unsigned long spinnerIntervalMs = (state == SYNC_CONNECTED) ? 3000 : 600;
    if (millis() - lastSpinnerUpdate >= spinnerIntervalMs) {
      lastSpinnerUpdate = millis();
      spinnerFrame = (spinnerFrame + 1) % 3;
      requestUpdate();
    }

    // Targeted, higher-frequency heap sample (main.cpp already logs this
    // every 10s system-wide) so a future capture can correlate the decline
    // precisely against render-tick count / BLE traffic during this specific
    // screen, rather than just elapsed time.
    if (millis() - lastHeapLogMs >= 5000) {
      lastHeapLogMs = millis();
      LOG_DBG("CAL", "heap during sync: free=%u maxAlloc=%u", (unsigned)ESP.getFreeHeap(),
              (unsigned)ESP.getMaxAllocHeap());
    }

    if (bleServer.consumePendingDisconnect()) {
      char buf[48];
      if (eventsReceivedThisSync > 0) {
        snprintf(buf, sizeof(buf), "Synced %d event(s)", eventsReceivedThisSync);
      } else {
        snprintf(buf, sizeof(buf), "Disconnected - no events received");
      }
      syncResultMessage = buf;
      stopSync();
      state = SYNC_DONE;
      requestUpdate();
      return;
    }

    // Applies in both ADVERTISING and CONNECTED: Gadgetbridge only requests
    // calendar sync (force_calendar_sync_start) from its own onSetTime()
    // callback, not automatically on every connect, and only replies to our
    // response if "Sync calendar" is enabled for this device in Gadgetbridge
    // -- so a connected-but-idle session (0 events, no further traffic) is a
    // real possibility, not just a slow advertise. Time out either way rather
    // than waiting indefinitely for Gadgetbridge to decide to disconnect.
    if (millis() - syncStartMs > SYNC_TIMEOUT_MS) {
      syncResultMessage = (state == SYNC_ADVERTISING) ? "No connection - check Gadgetbridge and try again"
                                                        : "Timed out - no calendar sync request from phone";
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

void CalendarActivity::localBrokenDownTime(time_t utcTimestamp, struct tm& out) {
  const int offsetQuarterHours = static_cast<int>(SETTINGS.clockUtcOffsetQ) - 48;
  const time_t shifted = utcTimestamp + static_cast<time_t>(offsetQuarterHours) * 15 * 60;
  gmtime_r(&shifted, &out);
}

std::string CalendarActivity::relativeDayLabel(time_t dayStart) {
  time_t now = time(nullptr);
  struct tm nowTm;
  struct tm dayTm;
  localBrokenDownTime(now, nowTm);
  localBrokenDownTime(dayStart, dayTm);

  const int nowDay = nowTm.tm_year * 1000 + nowTm.tm_yday;
  const int thatDay = dayTm.tm_year * 1000 + dayTm.tm_yday;
  const int diff = thatDay - nowDay;

  if (diff == 0) return "TODAY";
  if (diff == 1) return "TOMORROW";
  if (diff == -1) return "YESTERDAY";

  static const char* const kWeekdays[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  static const char* const kMonths[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
  char buf[24];
  if (diff > 1 && diff < 7) {
    snprintf(buf, sizeof(buf), "%s", kWeekdays[dayTm.tm_wday]);
  } else {
    snprintf(buf, sizeof(buf), "%s %d %s", kWeekdays[dayTm.tm_wday], dayTm.tm_mday, kMonths[dayTm.tm_mon]);
  }
  return buf;
}

void CalendarActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();

  if (state == LIST) {
    char subtitle[32];
    if (sortedOrder.empty()) {
      snprintf(subtitle, sizeof(subtitle), "No events yet");
    } else {
      snprintf(subtitle, sizeof(subtitle), "%d upcoming event%s", static_cast<int>(sortedOrder.size()),
                sortedOrder.size() == 1 ? "" : "s");
    }
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Calendar", subtitle);
  } else {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Calendar");
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
// is built on (settings, EventLogger, HabitTracker, WifiScanner, ...) rather
// than a bespoke agenda layout -- consistent look-and-feel for free, and
// row-height/subtitle spacing that's already correct and battle-tested
// instead of hand-computed here (a previous hand-rolled version had a real
// row-height bug: fixed-height rows overlapped the next row whenever an
// event's location line needed extra space). "Sync with Phone" is just the
// first row, matching how HabitTrackerActivity's own edit list puts
// "+ Add Habit" as a plain row rather than a separate special control.
void CalendarActivity::renderListState() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int listTop = metrics.topPadding + metrics.headerHeight;
  const int listH = pageHeight - listTop - metrics.buttonHintsHeight;

  const int itemCount = 1 + static_cast<int>(sortedOrder.size());
  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listH}, itemCount, selectedIndex,
      [this](int i) -> std::string {
        if (i == 0) return "Sync with Phone";
        return data.events[sortedOrder[i - 1]].title;
      },
      [this](int i) -> std::string {
        if (i == 0) return "Pulls events from Gadgetbridge";
        const CalendarEvent& ev = data.events[sortedOrder[i - 1]];
        if (ev.allDay) return ev.location[0] != '\0' ? std::string("All day - ") + ev.location : "All day";
        struct tm t;
        localBrokenDownTime(ev.timestamp, t);
        char buf[48];
        if (ev.location[0] != '\0') {
          snprintf(buf, sizeof(buf), "%02d:%02d - %s", t.tm_hour, t.tm_min, ev.location);
        } else {
          snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
        }
        return buf;
      },
      nullptr,  // rowIcon
      [this](int i) -> std::string {
        if (i == 0) return "";
        return relativeDayLabel(data.events[sortedOrder[i - 1]].timestamp);
      });

  const auto labels = mappedInput.mapLabels("Back", "Select", "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CalendarActivity::renderDetailState() const {
  if (selectedIndex <= 0 || selectedIndex - 1 >= static_cast<int>(sortedOrder.size())) return;
  const CalendarEvent& ev = data.events[sortedOrder[selectedIndex - 1]];

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pad = metrics.contentSidePadding;
  int y = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;

  std::string dayLabel = relativeDayLabel(ev.timestamp);
  renderer.drawText(SMALL_FONT_ID, pad, y, dayLabel.c_str(), true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(SMALL_FONT_ID) + 4;

  auto titleLines = renderer.wrappedText(UI_10_FONT_ID, ev.title, pageWidth - pad * 2, 3);
  for (const auto& line : titleLines) {
    renderer.drawText(UI_10_FONT_ID, pad, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += renderer.getLineHeight(UI_10_FONT_ID);
  }
  y += 6;

  renderer.drawLine(pad, y, pageWidth - pad, y, true);
  y += 12;

  // Start-end range rather than a separate start time + duration-in-minutes
  // field: a glance at "09:00 - 10:30" answers "when does this end" directly,
  // where "45 min" needs the reader to do the arithmetic themselves.
  char timeBuf[24];
  if (ev.allDay) {
    snprintf(timeBuf, sizeof(timeBuf), "All day");
  } else {
    struct tm startTm;
    localBrokenDownTime(ev.timestamp, startTm);
    if (ev.durationSec > 0) {
      struct tm endTm;
      localBrokenDownTime(ev.timestamp + static_cast<time_t>(ev.durationSec), endTm);
      snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d - %02d:%02d", startTm.tm_hour, startTm.tm_min, endTm.tm_hour,
                endTm.tm_min);
    } else {
      snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", startTm.tm_hour, startTm.tm_min);
    }
  }
  renderer.drawText(SMALL_FONT_ID, pad, y, "TIME", true, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, pad + 90, y - 2, timeBuf);
  y += renderer.getLineHeight(UI_10_FONT_ID) + 8;

  if (ev.location[0] != '\0') {
    renderer.drawText(SMALL_FONT_ID, pad, y, "WHERE", true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, pad + 90, y - 2, ev.location);
    y += renderer.getLineHeight(UI_10_FONT_ID) + 8;
  }

  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CalendarActivity::renderSyncState() const {
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
    snprintf(buf, sizeof(buf), "%d event(s) received so far", eventsReceivedThisSync);
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
