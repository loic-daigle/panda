#include "CalendarActivity.h"

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

void CalendarActivity::upsertEvent(uint32_t id, int type, time_t timestamp, uint32_t durationSec, const char* title,
                                    const char* location, bool allDay) {
  int existing = -1;
  for (int i = 0; i < data.eventCount; i++) {
    if (data.events[i].id == id) {
      existing = i;
      break;
    }
  }

  // type==1 as "delete" matches the Pebble calendar bridge this was adapted
  // from -- Gadgetbridge's own Bangle.js calendar support doesn't fully spec
  // this yet (see CalendarActivity.h). Adjust if real-device testing shows
  // otherwise.
  if (type == 1) {
    if (existing >= 0) {
      for (int i = existing; i < data.eventCount - 1; i++) data.events[i] = data.events[i + 1];
      data.eventCount--;
    }
    return;
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

    if (millis() - lastSpinnerUpdate >= 600) {
      lastSpinnerUpdate = millis();
      spinnerFrame = (spinnerFrame + 1) % 3;
      requestUpdate();
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

std::string CalendarActivity::relativeDayLabel(time_t dayStart) {
  time_t now = time(nullptr);
  struct tm nowTm;
  struct tm dayTm;
  localtime_r(&now, &nowTm);
  localtime_r(&dayStart, &dayTm);

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

// Agenda-style list: a "Sync with Phone" pill at the top, then events
// grouped under a small-caps day header ("TODAY", "TOMORROW", "WED 14 JAN"),
// closer to a real calendar app than a flat row list. Row heights differ
// between the pill/header/event rows, so scroll position is recomputed each
// frame from the selected row's cumulative offset rather than tracked as a
// persisted `int scrollOffset` -- simpler, and always correct even if the
// event list changes underneath the current selection.
void CalendarActivity::renderListState() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int pad = metrics.contentSidePadding;
  const int listTop = metrics.topPadding + metrics.headerHeight + 6;
  const int listBottom = pageHeight - metrics.buttonHintsHeight;
  const int listH = listBottom - listTop;

  const int pillH = 40;
  const int headerH = 22;
  const int eventH = 40;

  struct Row {
    enum Kind { Pill, DayHeader, Event } kind;
    int selectableIndex = -1;  // matches `selectedIndex` for Pill/Event rows
    int eventIdx = -1;         // index into data.events, for Event rows
    std::string label;         // day label, for DayHeader rows
    int height = 0;
    int top = 0;  // filled in below once every row's height is known
  };

  std::vector<Row> rows;
  rows.push_back({Row::Pill, 0, -1, "", pillH, 0});

  int prevDayKey = INT32_MIN;
  for (size_t i = 0; i < sortedOrder.size(); i++) {
    const CalendarEvent& ev = data.events[sortedOrder[i]];
    struct tm t;
    localtime_r(&ev.timestamp, &t);
    const int dayKey = t.tm_year * 1000 + t.tm_yday;
    if (dayKey != prevDayKey) {
      rows.push_back({Row::DayHeader, -1, -1, relativeDayLabel(ev.timestamp), headerH, 0});
      prevDayKey = dayKey;
    }
    rows.push_back({Row::Event, 1 + static_cast<int>(i), static_cast<int>(sortedOrder[i]), "", eventH, 0});
  }

  int cumulative = 0;
  int selectedTop = 0, selectedHeight = pillH;
  for (auto& row : rows) {
    row.top = cumulative;
    if (row.selectableIndex == selectedIndex) {
      selectedTop = row.top;
      selectedHeight = row.height;
    }
    cumulative += row.height;
  }
  const int totalHeight = cumulative;

  // Center the selected row in the viewport when the agenda is taller than
  // the screen; otherwise just start at the top.
  int scrollTop = 0;
  if (totalHeight > listH) {
    scrollTop = selectedTop + selectedHeight / 2 - listH / 2;
    scrollTop = std::max(0, std::min(scrollTop, totalHeight - listH));
  }

  for (const auto& row : rows) {
    const int y = listTop + row.top - scrollTop;
    if (y + row.height < listTop || y > listBottom) continue;  // off-screen, skip drawing

    const bool selected = row.selectableIndex >= 0 && row.selectableIndex == selectedIndex;

    if (row.kind == Row::Pill) {
      const int pillY = y + 4;
      const int pillHeight = row.height - 8;
      if (selected) {
        renderer.fillRoundedRect(pad, pillY, pageWidth - pad * 2, pillHeight, 10, Color::Black);
      } else {
        renderer.drawRoundedRect(pad, pillY, pageWidth - pad * 2, pillHeight, 2, 10, true);
      }
      renderer.drawText(UI_10_FONT_ID, pad + 14, pillY + 8, "Sync with Phone", !selected, EpdFontFamily::BOLD);
      renderer.drawText(SMALL_FONT_ID, pad + 14, pillY + 8 + renderer.getLineHeight(UI_10_FONT_ID),
                        "Pulls events from Gadgetbridge", !selected);
    } else if (row.kind == Row::DayHeader) {
      const int textY = y + row.height - renderer.getLineHeight(SMALL_FONT_ID) - 2;
      renderer.drawText(SMALL_FONT_ID, pad, textY, row.label.c_str(), true, EpdFontFamily::BOLD);
      const int lineY = y + row.height - 2;
      const int labelW = renderer.getTextWidth(SMALL_FONT_ID, row.label.c_str());
      renderer.drawLine(pad + labelW + 10, lineY - 4, pageWidth - pad, lineY - 4, true);
    } else {
      const CalendarEvent& ev = data.events[row.eventIdx];
      if (selected) renderer.fillRect(0, y, pageWidth, row.height, true);

      char timeBuf[8];
      if (ev.allDay) {
        snprintf(timeBuf, sizeof(timeBuf), "ALL");
      } else {
        struct tm t;
        localtime_r(&ev.timestamp, &t);
        snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", t.tm_hour, t.tm_min);
      }
      constexpr int timeColW = 46;
      renderer.drawText(SMALL_FONT_ID, pad, y + 6, timeBuf, !selected);

      const int titleX = pad + timeColW;
      const int titleMaxW = pageWidth - titleX - pad;
      std::string title = renderer.truncatedText(UI_10_FONT_ID, ev.title, titleMaxW);
      renderer.drawText(UI_10_FONT_ID, titleX, y + 4, title.c_str(), !selected);
      if (ev.location[0] != '\0') {
        std::string loc = renderer.truncatedText(SMALL_FONT_ID, ev.location, titleMaxW);
        renderer.drawText(SMALL_FONT_ID, titleX, y + 4 + renderer.getLineHeight(UI_10_FONT_ID), loc.c_str(),
                          !selected);
      }
    }
  }

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

  char timeBuf[24];
  if (ev.allDay) {
    struct tm t;
    localtime_r(&ev.timestamp, &t);
    snprintf(timeBuf, sizeof(timeBuf), "All day");
  } else {
    struct tm t;
    localtime_r(&ev.timestamp, &t);
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", t.tm_hour, t.tm_min);
  }
  renderer.drawText(SMALL_FONT_ID, pad, y, "TIME", true, EpdFontFamily::BOLD);
  renderer.drawText(UI_10_FONT_ID, pad + 90, y - 2, timeBuf);
  y += renderer.getLineHeight(UI_10_FONT_ID) + 8;

  if (!ev.allDay && ev.durationSec > 0) {
    char durBuf[24];
    snprintf(durBuf, sizeof(durBuf), "%lu min", static_cast<unsigned long>(ev.durationSec / 60));
    renderer.drawText(SMALL_FONT_ID, pad, y, "DURATION", true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, pad + 90, y - 2, durBuf);
    y += renderer.getLineHeight(UI_10_FONT_ID) + 8;
  }

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
