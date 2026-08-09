#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/BangleGadgetbridgeServer.h"
#include "util/ButtonNavigator.h"

// Notifications app: advertises this device as a Bangle.js smartwatch
// (Nordic UART Service, device name "Bangle.js Panda" -- same virtual
// device identity as CalendarActivity) so Gadgetbridge mirrors phone
// notifications to it, same as it would to a real Bangle.js watch.
//
// The BLE GATT server, NimBLE bonding, and raw-bytes-to-lines plumbing all
// live in BangleGadgetbridgeServer (shared with CalendarActivity, the other
// Bangle.js-protocol consumer) -- this class only interprets the
// `"t":"notify"` lines it hands back.
//
// Protocol: Gadgetbridge sends `GB({"t":"notify","id":...,"src":"AppName",
// "title":"...","subject":"...","body":"...",...})` per notification. Only
// `t":"notify"` lines are handled here; everything else (calendar, weather,
// music, etc.) is discarded -- see CalendarActivity for that message type.
class NotificationsActivity final : public Activity {
 public:
  explicit NotificationsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Notifications", renderer, mappedInput), bleServer(*this) {
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

  static constexpr int MAX_NOTIFICATIONS = 30;
  static constexpr const char* SAVE_PATH = "/biscuit/notifications.dat";
  static constexpr const char* BLE_DEVICE_NAME = "Bangle.js Panda";
  static constexpr uint32_t SYNC_TIMEOUT_MS = 90000;

  struct NotificationEntry {
    uint32_t id = 0;
    time_t receivedAt = 0;
    char source[24] = {0};  // "src" field -- originating app/service name
    char title[48] = {0};
    char body[96] = {0};
  };

  struct SaveData {
    int entryCount = 0;
    NotificationEntry entries[MAX_NOTIFICATIONS];
  };

  SaveData data;
  int selectedIndex = 0;  // 0 = "Sync with Phone" row, 1..N = data.entries[N-1] (newest first)

  void load();
  void save();
  void renderListState() const;
  void renderDetailState() const;
  void renderSyncState() const;
  static std::string relativeTimeLabel(time_t receivedAt);

  // ---- BLE sync ----
  BangleGadgetbridgeServer bleServer;
  void startSync();
  void stopSync();
  // Invoked (on the main task, from bleServer.poll()) with the JSON body of
  // each `GB({...})` line already stripped of its envelope.
  void handleGbJson(const std::string& json);
  void addNotification(uint32_t id, const char* source, const char* title, const char* body);

  uint32_t syncStartMs = 0;
  int notificationsReceivedThisSync = 0;
  std::string syncResultMessage;

  int spinnerFrame = 0;
  unsigned long lastSpinnerUpdate = 0;
};
