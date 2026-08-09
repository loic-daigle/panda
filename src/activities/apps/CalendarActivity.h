#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/BangleGadgetbridgeServer.h"
#include "util/ButtonNavigator.h"

// Calendar app: advertises this device as a Bangle.js smartwatch (Nordic
// UART Service, device name "Bangle.js Panda") so Gadgetbridge -- an
// Android app many people already use to sync wearables without a vendor
// app -- talks to it using its existing Bangle.js calendar-sync support.
// No new phone-side software is required.
//
// This is a foreground-only, user-triggered sync (see RadioManager /
// SCOPE.md's radio-aware, battery-respectful principles): the device
// advertises for a bounded window, the phone connects and pushes calendar
// events, then the radio is released and events are persisted to SD.
//
// The BLE GATT server, NimBLE bonding, and raw-bytes-to-lines plumbing all
// live in BangleGadgetbridgeServer (shared with NotificationsActivity, the
// other Bangle.js-protocol consumer) -- this class only interprets the
// `"t":"calendar"` lines it hands back.
//
// Protocol notes (ported from research, not from any Bangle.js firmware):
//  - Gadgetbridge writes newline-terminated text to the RX characteristic,
//    mostly `\x10GB({...json...})\n` -- a JSON object wrapped in a GB() call
//    meant to be eval()'d by a real Espruino REPL. We are not a JS
//    interpreter: only lines containing `GB({"t":"calendar",...})` are
//    handled, everything else (time sync, app queries, etc.) is discarded.
//  - The add/remove ("type") semantics are not fully/crisply specified even
//    in Gadgetbridge's own in-progress support for this -- type 0 (or
//    absent) is treated as add/update, type 1 as delete, matching the
//    Pebble calendar bridge this was adapted from. Verify against a real
//    Gadgetbridge sync and adjust if needed.
class CalendarActivity final : public Activity {
 public:
  explicit CalendarActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Calendar", renderer, mappedInput), bleServer(*this) {
    bleServer.onLine = [this](const std::string& json) { handleGbJson(json); };
  }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum State { LIST, DETAIL, SYNC_ADVERTISING, SYNC_CONNECTED, SYNC_DONE };
  State state = LIST;
  ButtonNavigator buttonNavigator;

  static constexpr int MAX_EVENTS = 30;
  static constexpr const char* SAVE_PATH = "/biscuit/calendar.dat";
  static constexpr const char* BLE_DEVICE_NAME = "Bangle.js Panda";
  static constexpr uint32_t SYNC_TIMEOUT_MS = 90000;

  struct CalendarEvent {
    uint32_t id = 0;
    time_t timestamp = 0;
    uint32_t durationSec = 0;
    char title[48] = {0};
    char location[32] = {0};
    bool allDay = false;
  };

  struct SaveData {
    int eventCount = 0;
    CalendarEvent events[MAX_EVENTS];
  };

  SaveData data;
  std::vector<int> sortedOrder;  // indices into data.events, sorted by timestamp ascending
  int selectedIndex = 0;         // 0 = "Sync with Phone" row, 1..N = sortedOrder[i-1]

  void load();
  void save();
  void rebuildSortedOrder();
  void renderListState() const;
  void renderDetailState() const;
  void renderSyncState() const;
  // "TODAY" / "TOMORROW" / "WED" / "WED 14 JAN" depending on how far `dayStart`
  // (any timestamp within the target day) is from the device's current date.
  static std::string relativeDayLabel(time_t dayStart);

  // Sync-screen spinner (BaseTheme::drawSpinner), advanced on a 600ms timer
  // -- same pattern as SweepActivity/ProbeSnifferActivity elsewhere in this
  // codebase.
  int spinnerFrame = 0;
  unsigned long lastSpinnerUpdate = 0;

  // ---- BLE sync ----
  BangleGadgetbridgeServer bleServer;
  void startSync();
  void stopSync();
  // Invoked (on the main task, from bleServer.poll()) with the JSON body of
  // each `GB({...})` line already stripped of its envelope.
  void handleGbJson(const std::string& json);
  void upsertEvent(uint32_t id, int type, time_t timestamp, uint32_t durationSec, const char* title,
                    const char* location, bool allDay);

  uint32_t syncStartMs = 0;
  int eventsReceivedThisSync = 0;
  std::string syncResultMessage;
};
