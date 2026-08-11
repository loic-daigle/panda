#include "AppsMenuActivity.h"

#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_timer.h>

#include "ApHistoryLoggerActivity.h"
#include "AppCategoryActivity.h"
#include "AutomationActivity.h"
#include "BackgroundManagerActivity.h"
#include "BarcodeActivity.h"
#include "BatteryMonitorActivity.h"
#include "BleContactExchangeActivity.h"
#include "BleProximityActivity.h"
#include "BleScannerActivity.h"
#include "BreadcrumbTrailActivity.h"
#include "BulletinBoardActivity.h"
#include "CalculatorActivity.h"
#include "CalendarActivity.h"
#include "CasinoActivity.h"
#include "ChessActivity.h"
#include "CipherActivity.h"
#include "ClockActivity.h"
#include "CountdownActivity.h"
#include "CrowdDensityActivity.h"
#include "DeadDropActivity.h"
#include "DeauthDetectorActivity.h"
#include "DeviceFingerprinterActivity.h"
#include "DeviceInfoActivity.h"
#include "DiceRollerActivity.h"
#include "DnsLookupActivity.h"
#include "DuckDuckGoActivity.h"
#include "EmergencyActivity.h"
#include "EtchASketchActivity.h"
#include "EventLoggerActivity.h"
#include "FlashcardActivity.h"
#include "GameOfLifeActivity.h"
#include "GhostActivity.h"
#include "HabitTrackerActivity.h"
#include "HostScannerActivity.h"
#include "HttpClientActivity.h"
#include "KeyCopierActivity.h"
#include "LootActivity.h"
#include "MappedInputManager.h"
#include "MatrixRainActivity.h"
#include "MazeActivity.h"
#include "MdnsBrowserActivity.h"
#include "MedicalCardActivity.h"
#include "MeshChatActivity.h"
#include "MinesweeperActivity.h"
#include "MorseCodeActivity.h"
#include "NetworkChangeActivity.h"
#include "NetworkMonitorActivity.h"
#include "NotificationsActivity.h"
#include "OffenseMenuActivity.h"
#include "OpdsServerStore.h"
#include "OtpGeneratorActivity.h"
#include "PacketMonitorActivity.h"
#include "PasswordManagerActivity.h"
#include "PerimeterWatchActivity.h"
#include "PhoneTetherActivity.h"
#include "PingActivity.h"
#include "ProbeSnifferActivity.h"
#include "QrGeneratorActivity.h"
#include "QrTotpActivity.h"
#include "QuickWipeActivity.h"
#include "ReadingStatsActivity.h"
#include "ScanActivity.h"
#include "ScreenDecoyActivity.h"
#include "SdEncryptionActivity.h"
#include "SdFileBrowserActivity.h"
#include "SecurityPinActivity.h"
#include "SignalTriangulationActivity.h"
#include "SnakeActivity.h"
#include "SsidChannelActivity.h"
#include "SteganographyActivity.h"
#include "SudokuActivity.h"
#include "SweepActivity.h"
#include "TaskManagerActivity.h"
#include "TetrisActivity.h"
#include "TotpActivity.h"
#include "TrackerDetectorActivity.h"
#include "TransitAlertActivity.h"
#include "UnitConverterActivity.h"
#include "VehicleFinderActivity.h"
#include "VendorOuiActivity.h"
#include "VoronoiActivity.h"
#include "WardrivingActivity.h"
#include "WeatherActivity.h"
#include "WifiConnectActivity.h"
#include "WifiCredsActivity.h"
#include "WifiHeatMapActivity.h"
#include "WifiScannerActivity.h"
#include "WikipediaActivity.h"
#include "activities/browser/OpdsBookBrowserActivity.h"
#include "activities/home/FileBrowserActivity.h"
#include "activities/home/RecentBooksActivity.h"
#include "activities/network/NetworkModeSelectionActivity.h"
#include "activities/settings/OpdsServerListActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "components/UITheme.h"
#include "components/icons/book.h"
#include "components/icons/crosshair.h"
#include "components/icons/gamepad2.h"
#include "components/icons/radar.h"
#include "components/icons/radio.h"
#include "components/icons/settings2.h"
#include "components/icons/shield.h"
#include "components/icons/wrench.h"
#include "components/themes/radar/RadarHomeRenderer.h"
#include "fontIds.h"

// Radar home node table — kept in flash (.rodata) as constexpr.
static constexpr RadarNode kRadarNodes[8] = {
    {"RECON", 14}, {"OFFENSE", 21}, {"DEFENSE", 12}, {"COMMS", 5},
    {"TOOLS", 32}, {"GAMES", 11},   {"READER", 5},   {"SETTINGS", 7},
};

void AppsMenuActivity::onEnter() {
  Activity::onEnter();
  selectorIndex = 0;
  // Check badges once on enter (SD I/O only here, not in periodic refresh)
  badgeSecurity = Storage.exists("/biscuit/security.dat") ? 0 : -1;
  refreshSystemInfo();
  loadLastUsed();
  requestUpdate();
}

void AppsMenuActivity::loop() {
  // === RADAR MODE: circular navigation ===
  if (SETTINGS.uiTheme == CrossPointSettings::UI_THEME::RADAR) {
    // Right/Down advances clockwise; Left/Up goes counter-clockwise.
    if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      selectorIndex = (selectorIndex + 1) % ITEM_COUNT;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
               mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      selectorIndex = (selectorIndex - 1 + ITEM_COUNT) % ITEM_COUNT;
      requestUpdate();
    }
    // Periodic info refresh in radar mode
    if (millis() - lastInfoRefresh > INFO_REFRESH_MS) {
      uint32_t oldHeap = freeHeap;
      bool oldWifi = wifiConnected;
      refreshSystemInfo();
      bool heapChanged = (freeHeap / 1024) != (oldHeap / 1024);
      if (heapChanged || (wifiConnected != oldWifi)) {
        requestUpdate();
      }
    }
    // Confirm and Back use the same switch as the grid — fall through to shared confirm block below.
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      std::unique_ptr<Activity> app;
      switch (selectorIndex) {
        case 0: {
          std::vector<AppCategoryActivity::AppEntry> e = {
              {tr(STR_PACKET_MONITOR), "WiFi frames + PCAP export", UIIcon::Wifi,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<PacketMonitorActivity>(r, m); }},
              {"Probe Sniffer", "Capture WiFi probe requests", UIIcon::Wifi,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<ProbeSnifferActivity>(r, m); }},
              {"Wardriving", "Log APs with signal strength", UIIcon::Wifi,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<WardrivingActivity>(r, m); }},
              {"Crowd Density", "Estimate people nearby via probes", UIIcon::Wifi,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<CrowdDensityActivity>(r, m); }},
              {"Device Fingerprint", "Identify device OS from probes", UIIcon::File,
               [](GfxRenderer& r, MappedInputManager& m) {
                 return std::make_unique<DeviceFingerprinterActivity>(r, m);
               }},
              {"Vendor Lookup", "Identify maker by MAC (OUI)", UIIcon::Library,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<VendorOuiActivity>(r, m); }},
              {"AP History", "Log APs over time to SD", UIIcon::Recent,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<ApHistoryLoggerActivity>(r, m); }},
              {"Network Change", "Diff snapshots of nearby devices", UIIcon::File,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<NetworkChangeActivity>(r, m); }},
              {"Perimeter Watch", "Alert on new devices in area", UIIcon::Hotspot,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<PerimeterWatchActivity>(r, m); }},
              {"BLE Proximity", "Track BLE device RSSI", UIIcon::Hotspot,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<BleProximityActivity>(r, m); }},
              {"WiFi Heat Map", "RSSI mapping walkabout", UIIcon::Wifi,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<WifiHeatMapActivity>(r, m); }},
              {"Signal Locator", "Estimate AP position via RSSI", UIIcon::Wifi,
               [](GfxRenderer& r, MappedInputManager& m) {
                 return std::make_unique<SignalTriangulationActivity>(r, m);
               }},
              {"Deauth Detector", "Monitor deauth frame spikes", UIIcon::Wifi,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<DeauthDetectorActivity>(r, m); }},
          };
          app = std::make_unique<AppCategoryActivity>(renderer, mappedInput, "Recon", std::move(e), true, 0);
          break;
        }
        case 1:
          app = std::make_unique<OffenseMenuActivity>(renderer, mappedInput);
          break;
        case 2: {
          std::vector<AppCategoryActivity::AppEntry> e = {
              {"* Ghost Mode", "MAC rotate + RF kill + cleanup", UIIcon::Settings,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<GhostActivity>(r, m); }},
              {"Emergency SOS", "SOS beacon + dead man switch", UIIcon::Hotspot,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<EmergencyActivity>(r, m); }},
              AppCategoryActivity::SectionHeader("DETECTION"),
              {"Tracker Detector", "Detect AirTags following you", UIIcon::Hotspot,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<TrackerDetectorActivity>(r, m); }},
              {"Security Sweep", "Scan for cameras/trackers/rogues", UIIcon::Settings,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SweepActivity>(r, m); }},
              {"Network Monitor", "Detect rogue APs + suspicious frames", UIIcon::Wifi,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<NetworkMonitorActivity>(r, m); }},
              {"Phone Tether", "BLE proximity disconnect alert", UIIcon::Hotspot,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<PhoneTetherActivity>(r, m); }},
              AppCategoryActivity::SectionHeader("DEVICE SECURITY"),
              {"Quick Wipe", "Erase all biscuit data from SD", UIIcon::Folder,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<QuickWipeActivity>(r, m); }},
              {"Captured Data", "Review captured creds/handshakes/PCAPs", UIIcon::File,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<LootActivity>(r, m); }},
              {"PIN Security", "Lock device with PIN + duress mode", UIIcon::Settings,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SecurityPinActivity>(r, m); }},
              {"Screen Decoy", "Fake screen to hide activity", UIIcon::File,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<ScreenDecoyActivity>(r, m); }},
              {"SD Encryption", "Encrypt biscuit data with PIN", UIIcon::Settings,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SdEncryptionActivity>(r, m); }},
          };
          app = std::make_unique<AppCategoryActivity>(renderer, mappedInput, "Defense", std::move(e), false, 2);
          break;
        }
        case 3: {
          std::vector<AppCategoryActivity::AppEntry> e = {
              {"Mesh Chat", "ESP-NOW text chat, no WiFi needed", UIIcon::Transfer,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<MeshChatActivity>(r, m); }},
              {"SSID Channel", "Hide messages in WiFi names", UIIcon::Wifi,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SsidChannelActivity>(r, m); }},
              {"Contact Exchange", "Swap contact cards via BLE", UIIcon::Transfer,
               [](GfxRenderer& r, MappedInputManager& m) {
                 return std::make_unique<BleContactExchangeActivity>(r, m);
               }},
              {"Dead Drop", "Anonymous file exchange AP", UIIcon::Hotspot,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<DeadDropActivity>(r, m); }},
              {"Bulletin Board", "Local anonymous message board", UIIcon::Hotspot,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<BulletinBoardActivity>(r, m); }},
          };
          app = std::make_unique<AppCategoryActivity>(renderer, mappedInput, "Comms", std::move(e), false, 3);
          break;
        }
        case 4: {
          std::vector<AppCategoryActivity::AppEntry> e = {
              {tr(STR_WIFI_CONNECT), "Join a WiFi network", UIIcon::Wifi,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<WifiConnectActivity>(r, m); }},
              AppCategoryActivity::SectionHeader("SECURITY & CRYPTO"),
              {"Authenticator", "TOTP 2FA codes (offline)", UIIcon::Settings,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<TotpActivity>(r, m); }, false,
               []() -> bool { return Storage.exists("/biscuit/totp.dat"); }},
              {"TOTP QR", "Show 2FA code as scannable QR", UIIcon::Image,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<QrTotpActivity>(r, m); }, false,
               []() -> bool { return Storage.exists("/biscuit/totp.dat"); }},
              {tr(STR_PASSWORD_MANAGER), "Encrypted credentials on SD", UIIcon::Settings,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<PasswordManagerActivity>(r, m); }},
              {"Medical Card", "Emergency medical info on screen", UIIcon::Text,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<MedicalCardActivity>(r, m); }},
              {"Stego Notes", "Hide text in BMP images", UIIcon::Image,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SteganographyActivity>(r, m); }},
              AppCategoryActivity::SectionHeader("NETWORK"),
              {tr(STR_WIFI_SCANNER), "APs, signal, channels", UIIcon::Wifi,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<WifiScannerActivity>(r, m); }},
              {tr(STR_HOST_SCANNER), "Find devices on local network", UIIcon::Wifi,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<HostScannerActivity>(r, m); }},
              {tr(STR_PING_TOOL), "Ping a host or IP address", UIIcon::Wifi,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<PingActivity>(r, m); }},
              {tr(STR_DNS_LOOKUP), "Resolve domain names", UIIcon::Wifi,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<DnsLookupActivity>(r, m); }},
              {"HTTP Client", "Send GET/POST requests", UIIcon::Transfer,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<HttpClientActivity>(r, m); }},
              {"mDNS Browser", "Discover local services", UIIcon::Wifi,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<MdnsBrowserActivity>(r, m); }},
              AppCategoryActivity::SectionHeader("WEB"),
              {"Weather", "Local forecast, cached offline", UIIcon::File,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<WeatherActivity>(r, m); }},
              {"Wikipedia", "Search and download articles", UIIcon::Book,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<WikipediaActivity>(r, m); }},
              {"DuckDuckGo", "Search the web", UIIcon::Wifi,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<DuckDuckGoActivity>(r, m); }},
              AppCategoryActivity::SectionHeader("PRODUCTIVITY"),
              {"Clock", "NTP clock / stopwatch / pomodoro", UIIcon::Recent,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<ClockActivity>(r, m); }},
              {"Calendar", "Sync events from phone via Gadgetbridge", UIIcon::Text,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<CalendarActivity>(r, m); }},
              {"Notifications", "Mirror phone notifications via Gadgetbridge", UIIcon::Text,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<NotificationsActivity>(r, m); }},
              {"Calculator", "Basic calculator", UIIcon::File,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<CalculatorActivity>(r, m); }},
              {tr(STR_QR_GENERATOR), "Generate QR codes from text", UIIcon::Image,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<QrGeneratorActivity>(r, m); }},
              {tr(STR_MORSE_CODE), "Encode/decode morse", UIIcon::Text,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<MorseCodeActivity>(r, m); }},
              {tr(STR_UNIT_CONVERTER), "Convert between units", UIIcon::File,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<UnitConverterActivity>(r, m); }},
              {"Cipher Tools", "ROT13, Caesar, Vigenere, XOR", UIIcon::Text,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<CipherActivity>(r, m); }},
              {"OTP Generator", "One-time pad random numbers", UIIcon::Text,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<OtpGeneratorActivity>(r, m); }},
              AppCategoryActivity::SectionHeader("TRACKING & LOGGING"),
              {"Event Logger", "Timestamped notes with location", UIIcon::Text,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<EventLoggerActivity>(r, m); }},
              {"Flashcards", "Study decks from SD (CSV)", UIIcon::Book,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<FlashcardActivity>(r, m); }},
              {"Habit Tracker", "Daily habits with streaks", UIIcon::Recent,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<HabitTrackerActivity>(r, m); },
               false, []() -> bool { return Storage.exists("/biscuit/habits.dat"); }},
              {"Automation", "Geofence triggers + scheduled tasks", UIIcon::Recent,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<AutomationActivity>(r, m); }},
              {"Breadcrumb Trail", "Retrace your path via WiFi", UIIcon::Wifi,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<BreadcrumbTrailActivity>(r, m); }},
              {"Vehicle Finder", "Find parked car via WiFi", UIIcon::Wifi,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<VehicleFinderActivity>(r, m); }},
              {"Transit Alert", "Alert when nearing your stop", UIIcon::Recent,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<TransitAlertActivity>(r, m); }},
              AppCategoryActivity::SectionHeader("CREATIVE"),
              {tr(STR_ETCH_A_SKETCH), "Draw on the e-ink screen", UIIcon::Image,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<EtchASketchActivity>(r, m); }},
              {"Barcode Generator", "Code 128 / Code 39 / EAN-13", UIIcon::Image,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<BarcodeActivity>(r, m); }},
              {"Key Copier", "Draw key profiles from bitting codes", UIIcon::Settings,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<KeyCopierActivity>(r, m); }},
              {"WiFi QR Share", "Share WiFi credentials as QR", UIIcon::Wifi,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<WifiCredsActivity>(r, m); }},
              {"File Browser", "Browse files on SD card", UIIcon::Folder,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SdFileBrowserActivity>(r, m); }},
              {"Countdown", "Big countdown timer", UIIcon::Recent,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<CountdownActivity>(r, m); }},
          };
          app = std::make_unique<AppCategoryActivity>(renderer, mappedInput, "Tools", std::move(e), false, 4);
          break;
        }
        case 5: {
          std::vector<AppCategoryActivity::AppEntry> e = {
              {"Casino", "Slots, blackjack, roulette + lootbox", UIIcon::File,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<CasinoActivity>(r, m); }, false,
               []() -> bool { return Storage.exists("/biscuit/casino.dat"); }},
              {tr(STR_MINESWEEPER), "Classic minesweeper", UIIcon::File,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<MinesweeperActivity>(r, m); }},
              {tr(STR_SUDOKU), "Number puzzle", UIIcon::File,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SudokuActivity>(r, m); }},
              {tr(STR_CHESS), "Play against the device", UIIcon::File,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<ChessActivity>(r, m); }},
              {tr(STR_SNAKE), "Classic snake game", UIIcon::File,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SnakeActivity>(r, m); }},
              {tr(STR_TETRIS), "Block stacking", UIIcon::File,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<TetrisActivity>(r, m); }},
              {"Maze", "Navigate random mazes", UIIcon::File,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<MazeActivity>(r, m); }},
              {tr(STR_DICE_ROLLER), "Roll dice with animation", UIIcon::File,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<DiceRollerActivity>(r, m); }},
              {tr(STR_GAME_OF_LIFE), "Conway's cellular automaton", UIIcon::File,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<GameOfLifeActivity>(r, m); }},
              {tr(STR_VORONOI), "Generate Voronoi patterns", UIIcon::File,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<VoronoiActivity>(r, m); }},
              {"Matrix Rain", "The Matrix digital rain effect", UIIcon::File,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<MatrixRainActivity>(r, m); }},
          };
          app = std::make_unique<AppCategoryActivity>(renderer, mappedInput, tr(STR_GAMES), std::move(e), false, 5);
          break;
        }
        case 6: {
          std::vector<AppCategoryActivity::AppEntry> e = {
              {"Open Book", "Browse and open an ebook", UIIcon::Book,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<FileBrowserActivity>(r, m); }},
              {"Recent Books", "Continue where you left off", UIIcon::Recent,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<RecentBooksActivity>(r, m); }},
              {"OPDS Browser", "Download books from OPDS servers", UIIcon::Library,
               [](GfxRenderer& r, MappedInputManager& m) -> std::unique_ptr<Activity> {
                 const auto& servers = OPDS_STORE.getServers();
                 if (servers.size() == 1) {
                   return std::make_unique<OpdsBookBrowserActivity>(r, m, servers[0]);
                 }
                 return std::make_unique<OpdsServerListActivity>(r, m, true);
               }},
              {"Reading Stats", "Pages read, streaks, progress", UIIcon::Book,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<ReadingStatsActivity>(r, m); }},
              {"Browse Files", "File manager for SD card", UIIcon::Folder,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<FileBrowserActivity>(r, m); }},
          };
          app = std::make_unique<AppCategoryActivity>(renderer, mappedInput, "Reader", std::move(e), false, 6);
          break;
        }
        case 7: {
          std::vector<AppCategoryActivity::AppEntry> e = {
              AppCategoryActivity::SectionHeader("PREFERENCES"),
              {"Settings", "Display, reader, controls, system", UIIcon::Settings,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SettingsActivity>(r, m); }},
              AppCategoryActivity::SectionHeader("FILE TRANSFER"),
              {"WiFi Transfer", "Upload/download via WiFi", UIIcon::Transfer,
               [](GfxRenderer& r, MappedInputManager& m) {
                 return std::make_unique<NetworkModeSelectionActivity>(r, m);
               }},
              AppCategoryActivity::SectionHeader("SYSTEM"),
              {"Task Manager", "View heap, uptime, activity stack", UIIcon::Settings,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<TaskManagerActivity>(r, m); }},
              {"Battery", "Battery level + history graph", UIIcon::File,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<BatteryMonitorActivity>(r, m); }},
              {"Device Info", "Chip, flash, RAM, firmware info", UIIcon::Settings,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<DeviceInfoActivity>(r, m); }},
              {"Background", "Radio state, SD, active timers", UIIcon::Settings,
               [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<BackgroundManagerActivity>(r, m); }},
          };
          app = std::make_unique<AppCategoryActivity>(renderer, mappedInput, "Settings", std::move(e), false, 7);
          break;
        }
      }
      if (app) activityManager.pushActivity(std::move(app));
    }
    // Back button ignored on main screen — use Power button to sleep
    return;
  }

  // === 2D GRID NAVIGATION ===
  // Left/Right (front buttons) move between columns
  // Up/Down (side volume buttons) move between rows

  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    int col = getCol();
    int row = getRow();
    col++;
    if (col >= COLS) {
      col = 0;
      row = (row + 1) % ROWS;
    }
    selectorIndex = row * COLS + col;
    if (selectorIndex >= ITEM_COUNT) selectorIndex = 0;
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    int col = getCol();
    int row = getRow();
    col--;
    if (col < 0) {
      col = COLS - 1;
      row = (row - 1 + ROWS) % ROWS;
    }
    selectorIndex = row * COLS + col;
    if (selectorIndex >= ITEM_COUNT) selectorIndex = ITEM_COUNT - 1;
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    int col = getCol();
    int row = (getRow() + 1) % ROWS;
    selectorIndex = row * COLS + col;
    if (selectorIndex >= ITEM_COUNT) selectorIndex = col;
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    int col = getCol();
    int row = (getRow() - 1 + ROWS) % ROWS;
    selectorIndex = row * COLS + col;
    if (selectorIndex >= ITEM_COUNT) {
      row = (row - 1 + ROWS) % ROWS;
      selectorIndex = row * COLS + col;
    }
    requestUpdate();
  }

  // Periodic info refresh — only redraw if visible values changed. Heap is no
  // longer shown on this screen, so only a wifi status change is worth an
  // e-ink repaint; the clock/battery caches ride along passively and are
  // simply whatever they were as of the last repaint for any reason.
  if (millis() - lastInfoRefresh > INFO_REFRESH_MS) {
    bool oldWifi = wifiConnected;
    refreshSystemInfo();
    if (wifiConnected != oldWifi) {
      requestUpdate();
    }
  }

  // === CONFIRM: open category ===
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    std::unique_ptr<Activity> app;
    switch (selectorIndex) {
      case 0: {
        // RECON — passive scanning + monitoring
        std::vector<AppCategoryActivity::AppEntry> e = {
            {tr(STR_PACKET_MONITOR), "WiFi frames + PCAP export", UIIcon::Wifi,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<PacketMonitorActivity>(r, m); }},
            {"Probe Sniffer", "Capture WiFi probe requests", UIIcon::Wifi,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<ProbeSnifferActivity>(r, m); }},
            {"Wardriving", "Log APs with signal strength", UIIcon::Wifi,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<WardrivingActivity>(r, m); }},
            {"Crowd Density", "Estimate people nearby via probes", UIIcon::Wifi,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<CrowdDensityActivity>(r, m); }},
            {"Device Fingerprint", "Identify device OS from probes", UIIcon::File,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<DeviceFingerprinterActivity>(r, m); }},
            {"Vendor Lookup", "Identify maker by MAC (OUI)", UIIcon::Library,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<VendorOuiActivity>(r, m); }},
            {"AP History", "Log APs over time to SD", UIIcon::Recent,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<ApHistoryLoggerActivity>(r, m); }},
            {"Network Change", "Diff snapshots of nearby devices", UIIcon::File,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<NetworkChangeActivity>(r, m); }},
            {"Perimeter Watch", "Alert on new devices in area", UIIcon::Hotspot,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<PerimeterWatchActivity>(r, m); }},
            {"BLE Proximity", "Track BLE device RSSI", UIIcon::Hotspot,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<BleProximityActivity>(r, m); }},
            {"WiFi Heat Map", "RSSI mapping walkabout", UIIcon::Wifi,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<WifiHeatMapActivity>(r, m); }},
            {"Signal Locator", "Estimate AP position via RSSI", UIIcon::Wifi,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SignalTriangulationActivity>(r, m); }},
            {"Deauth Detector", "Monitor deauth frame spikes", UIIcon::Wifi,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<DeauthDetectorActivity>(r, m); }},
        };
        app = std::make_unique<AppCategoryActivity>(renderer, mappedInput, "Recon", std::move(e), true, 0);
        break;
      }
      case 1:
        app = std::make_unique<OffenseMenuActivity>(renderer, mappedInput);
        break;
      case 2: {
        // DEFENSE — stealth + detection + protection
        std::vector<AppCategoryActivity::AppEntry> e = {
            {"* Ghost Mode", "MAC rotate + RF kill + cleanup", UIIcon::Settings,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<GhostActivity>(r, m); }},
            {"Emergency SOS", "SOS beacon + dead man switch", UIIcon::Hotspot,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<EmergencyActivity>(r, m); }},
            AppCategoryActivity::SectionHeader("DETECTION"),
            {"Tracker Detector", "Detect AirTags following you", UIIcon::Hotspot,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<TrackerDetectorActivity>(r, m); }},
            {"Security Sweep", "Scan for cameras/trackers/rogues", UIIcon::Settings,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SweepActivity>(r, m); }},
            {"Network Monitor", "Detect rogue APs + suspicious frames", UIIcon::Wifi,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<NetworkMonitorActivity>(r, m); }},
            {"Phone Tether", "BLE proximity disconnect alert", UIIcon::Hotspot,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<PhoneTetherActivity>(r, m); }},
            AppCategoryActivity::SectionHeader("DEVICE SECURITY"),
            {"Quick Wipe", "Erase all biscuit data from SD", UIIcon::Folder,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<QuickWipeActivity>(r, m); }},
            {"Captured Data", "Review captured creds/handshakes/PCAPs", UIIcon::File,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<LootActivity>(r, m); }},
            {"PIN Security", "Lock device with PIN + duress mode", UIIcon::Settings,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SecurityPinActivity>(r, m); }},
            {"Screen Decoy", "Fake screen to hide activity", UIIcon::File,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<ScreenDecoyActivity>(r, m); }},
            {"SD Encryption", "Encrypt biscuit data with PIN", UIIcon::Settings,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SdEncryptionActivity>(r, m); }},
        };
        app = std::make_unique<AppCategoryActivity>(renderer, mappedInput, "Defense", std::move(e), false, 2);
        break;
      }
      case 3: {
        // COMMS — communication tools
        std::vector<AppCategoryActivity::AppEntry> e = {
            {"Mesh Chat", "ESP-NOW text chat, no WiFi needed", UIIcon::Transfer,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<MeshChatActivity>(r, m); }},
            {"SSID Channel", "Hide messages in WiFi names", UIIcon::Wifi,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SsidChannelActivity>(r, m); }},
            {"Contact Exchange", "Swap contact cards via BLE", UIIcon::Transfer,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<BleContactExchangeActivity>(r, m); }},
            {"Dead Drop", "Anonymous file exchange AP", UIIcon::Hotspot,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<DeadDropActivity>(r, m); }},
            {"Bulletin Board", "Local anonymous message board", UIIcon::Hotspot,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<BulletinBoardActivity>(r, m); }},
        };
        app = std::make_unique<AppCategoryActivity>(renderer, mappedInput, "Comms", std::move(e), false, 3);
        break;
      }
      case 4: {
        // TOOLS — utilities, network tools, productivity
        std::vector<AppCategoryActivity::AppEntry> e = {
            {tr(STR_WIFI_CONNECT), "Join a WiFi network", UIIcon::Wifi,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<WifiConnectActivity>(r, m); }},
            AppCategoryActivity::SectionHeader("SECURITY & CRYPTO"),
            {"Authenticator", "TOTP 2FA codes (offline)", UIIcon::Settings,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<TotpActivity>(r, m); }, false,
             []() -> bool { return Storage.exists("/biscuit/totp.dat"); }},
            {"TOTP QR", "Show 2FA code as scannable QR", UIIcon::Image,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<QrTotpActivity>(r, m); }, false,
             []() -> bool { return Storage.exists("/biscuit/totp.dat"); }},
            {tr(STR_PASSWORD_MANAGER), "Encrypted credentials on SD", UIIcon::Settings,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<PasswordManagerActivity>(r, m); }},
            {"Medical Card", "Emergency medical info on screen", UIIcon::Text,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<MedicalCardActivity>(r, m); }},
            {"Stego Notes", "Hide text in BMP images", UIIcon::Image,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SteganographyActivity>(r, m); }},
            AppCategoryActivity::SectionHeader("NETWORK"),
            {tr(STR_WIFI_SCANNER), "APs, signal, channels", UIIcon::Wifi,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<WifiScannerActivity>(r, m); }},
            {tr(STR_HOST_SCANNER), "Find devices on local network", UIIcon::Wifi,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<HostScannerActivity>(r, m); }},
            {tr(STR_PING_TOOL), "Ping a host or IP address", UIIcon::Wifi,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<PingActivity>(r, m); }},
            {tr(STR_DNS_LOOKUP), "Resolve domain names", UIIcon::Wifi,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<DnsLookupActivity>(r, m); }},
            {"HTTP Client", "Send GET/POST requests", UIIcon::Transfer,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<HttpClientActivity>(r, m); }},
            {"mDNS Browser", "Discover local services", UIIcon::Wifi,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<MdnsBrowserActivity>(r, m); }},
            AppCategoryActivity::SectionHeader("WEB"),
            {"Weather", "Local forecast, cached offline", UIIcon::File,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<WeatherActivity>(r, m); }},
            {"Wikipedia", "Search and download articles", UIIcon::Book,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<WikipediaActivity>(r, m); }},
            {"DuckDuckGo", "Search the web", UIIcon::Wifi,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<DuckDuckGoActivity>(r, m); }},
            AppCategoryActivity::SectionHeader("PRODUCTIVITY"),
            {"Clock", "NTP clock / stopwatch / pomodoro", UIIcon::Recent,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<ClockActivity>(r, m); }},
            {"Calendar", "Sync events from phone via Gadgetbridge", UIIcon::Text,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<CalendarActivity>(r, m); }},
            {"Notifications", "Mirror phone notifications via Gadgetbridge", UIIcon::Text,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<NotificationsActivity>(r, m); }},
            {"Calculator", "Basic calculator", UIIcon::File,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<CalculatorActivity>(r, m); }},
            {tr(STR_QR_GENERATOR), "Generate QR codes from text", UIIcon::Image,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<QrGeneratorActivity>(r, m); }},
            {tr(STR_MORSE_CODE), "Encode/decode morse", UIIcon::Text,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<MorseCodeActivity>(r, m); }},
            {tr(STR_UNIT_CONVERTER), "Convert between units", UIIcon::File,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<UnitConverterActivity>(r, m); }},
            {"Cipher Tools", "ROT13, Caesar, Vigenere, XOR", UIIcon::Text,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<CipherActivity>(r, m); }},
            {"OTP Generator", "One-time pad random numbers", UIIcon::Text,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<OtpGeneratorActivity>(r, m); }},
            AppCategoryActivity::SectionHeader("TRACKING & LOGGING"),
            {"Event Logger", "Timestamped notes with location", UIIcon::Text,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<EventLoggerActivity>(r, m); }},
            {"Flashcards", "Study decks from SD (CSV)", UIIcon::Book,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<FlashcardActivity>(r, m); }},
            {"Habit Tracker", "Daily habits with streaks", UIIcon::Recent,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<HabitTrackerActivity>(r, m); }, false,
             []() -> bool { return Storage.exists("/biscuit/habits.dat"); }},
            {"Automation", "Geofence triggers + scheduled tasks", UIIcon::Recent,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<AutomationActivity>(r, m); }},
            {"Breadcrumb Trail", "Retrace your path via WiFi", UIIcon::Wifi,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<BreadcrumbTrailActivity>(r, m); }},
            {"Vehicle Finder", "Find parked car via WiFi", UIIcon::Wifi,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<VehicleFinderActivity>(r, m); }},
            {"Transit Alert", "Alert when nearing your stop", UIIcon::Recent,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<TransitAlertActivity>(r, m); }},
            AppCategoryActivity::SectionHeader("CREATIVE"),
            {tr(STR_ETCH_A_SKETCH), "Draw on the e-ink screen", UIIcon::Image,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<EtchASketchActivity>(r, m); }},
            {"Barcode Generator", "Code 128 / Code 39 / EAN-13", UIIcon::Image,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<BarcodeActivity>(r, m); }},
            {"Key Copier", "Draw key profiles from bitting codes", UIIcon::Settings,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<KeyCopierActivity>(r, m); }},
            {"WiFi QR Share", "Share WiFi credentials as QR", UIIcon::Wifi,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<WifiCredsActivity>(r, m); }},
            {"File Browser", "Browse files on SD card", UIIcon::Folder,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SdFileBrowserActivity>(r, m); }},
            {"Countdown", "Big countdown timer", UIIcon::Recent,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<CountdownActivity>(r, m); }},
        };
        app = std::make_unique<AppCategoryActivity>(renderer, mappedInput, "Tools", std::move(e), false, 4);
        break;
      }
      case 5: {
        // GAMES
        std::vector<AppCategoryActivity::AppEntry> e = {
            {"Casino", "Slots, blackjack, roulette + lootbox", UIIcon::File,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<CasinoActivity>(r, m); }, false,
             []() -> bool { return Storage.exists("/biscuit/casino.dat"); }},
            {tr(STR_MINESWEEPER), "Classic minesweeper", UIIcon::File,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<MinesweeperActivity>(r, m); }},
            {tr(STR_SUDOKU), "Number puzzle", UIIcon::File,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SudokuActivity>(r, m); }},
            {tr(STR_CHESS), "Play against the device", UIIcon::File,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<ChessActivity>(r, m); }},
            {tr(STR_SNAKE), "Classic snake game", UIIcon::File,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SnakeActivity>(r, m); }},
            {tr(STR_TETRIS), "Block stacking", UIIcon::File,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<TetrisActivity>(r, m); }},
            {"Maze", "Navigate random mazes", UIIcon::File,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<MazeActivity>(r, m); }},
            {tr(STR_DICE_ROLLER), "Roll dice with animation", UIIcon::File,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<DiceRollerActivity>(r, m); }},
            {tr(STR_GAME_OF_LIFE), "Conway's cellular automaton", UIIcon::File,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<GameOfLifeActivity>(r, m); }},
            {tr(STR_VORONOI), "Generate Voronoi patterns", UIIcon::File,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<VoronoiActivity>(r, m); }},
            {"Matrix Rain", "The Matrix digital rain effect", UIIcon::File,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<MatrixRainActivity>(r, m); }},
        };
        app = std::make_unique<AppCategoryActivity>(renderer, mappedInput, tr(STR_GAMES), std::move(e), false, 5);
        break;
      }
      case 6: {
        // READER
        std::vector<AppCategoryActivity::AppEntry> e = {
            {"Open Book", "Browse and open an ebook", UIIcon::Book,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<FileBrowserActivity>(r, m); }},
            {"Recent Books", "Continue where you left off", UIIcon::Recent,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<RecentBooksActivity>(r, m); }},
            {"OPDS Browser", "Download books from OPDS servers", UIIcon::Library,
             [](GfxRenderer& r, MappedInputManager& m) -> std::unique_ptr<Activity> {
               const auto& servers = OPDS_STORE.getServers();
               if (servers.size() == 1) {
                 return std::make_unique<OpdsBookBrowserActivity>(r, m, servers[0]);
               }
               return std::make_unique<OpdsServerListActivity>(r, m, true);
             }},
            {"Reading Stats", "Pages read, streaks, progress", UIIcon::Book,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<ReadingStatsActivity>(r, m); }},
            {"Browse Files", "File manager for SD card", UIIcon::Folder,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<FileBrowserActivity>(r, m); }},
        };
        app = std::make_unique<AppCategoryActivity>(renderer, mappedInput, "Reader", std::move(e), false, 6);
        break;
      }
      case 7: {
        // SETTINGS — promoted to main tile
        std::vector<AppCategoryActivity::AppEntry> e = {
            AppCategoryActivity::SectionHeader("PREFERENCES"),
            {"Settings", "Display, reader, controls, system", UIIcon::Settings,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<SettingsActivity>(r, m); }},
            AppCategoryActivity::SectionHeader("FILE TRANSFER"),
            {"WiFi Transfer", "Upload/download via WiFi", UIIcon::Transfer,
             [](GfxRenderer& r, MappedInputManager& m) {
               return std::make_unique<NetworkModeSelectionActivity>(r, m);
             }},
            AppCategoryActivity::SectionHeader("SYSTEM"),
            {"Task Manager", "View heap, uptime, activity stack", UIIcon::Settings,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<TaskManagerActivity>(r, m); }},
            {"Battery", "Battery level + history graph", UIIcon::File,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<BatteryMonitorActivity>(r, m); }},
            {"Device Info", "Chip, flash, RAM, firmware info", UIIcon::Settings,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<DeviceInfoActivity>(r, m); }},
            {"Background", "Radio state, SD, active timers", UIIcon::Settings,
             [](GfxRenderer& r, MappedInputManager& m) { return std::make_unique<BackgroundManagerActivity>(r, m); }},
        };
        app = std::make_unique<AppCategoryActivity>(renderer, mappedInput, "Settings", std::move(e), false, 7);
        break;
      }
    }
    if (app) activityManager.pushActivity(std::move(app));
  }

  // Back button ignored on main screen — use Power button to sleep
}

void AppsMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  // === RADAR MODE ===
  if (SETTINGS.uiTheme == CrossPointSettings::UI_THEME::RADAR) {
    char radioBuf[48];
    char sysBuf[32];
    snprintf(radioBuf, sizeof(radioBuf), "wifi:%s  ble:OFF", wifiConnected ? "ON " : "OFF");
    snprintf(sysBuf, sizeof(sysBuf), "heap:%luK", (unsigned long)(freeHeap / 1024));
    RadarHomeStatus status{radioBuf, sysBuf, static_cast<int>(batteryPercent)};
    RadarHomeRenderer::draw(renderer, kRadarNodes, selectorIndex, status);
    const auto labels = mappedInput.mapLabels("", tr(STR_SELECT), "<", ">");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // === STATUS BAR (top 40px) ===
  drawStatusBar();

  // === TILE GRID ===
  constexpr int statusBarH = 40;
  constexpr int buttonHintsH = 40;
  constexpr int sidePad = 14;
  constexpr int tileGap = 12;               // airier modern spacing between rounded tiles
  constexpr int gridTop = statusBarH + 16;  // breathing room below the separator line
  const int gridBottom = pageHeight - buttonHintsH - 2;
  const int gridHeight = gridBottom - gridTop;

  const int tileW = (pageWidth - sidePad * 2 - tileGap) / COLS;
  const int tileH = (gridHeight - tileGap * (ROWS - 1)) / ROWS;

  for (int i = 0; i < ITEM_COUNT; i++) {
    int row = i / COLS;
    int col = i % COLS;
    int x = sidePad + col * (tileW + tileGap);
    int y = gridTop + row * (tileH + tileGap);
    drawTile(i, x, y, tileW, tileH, i == selectorIndex);
  }

  // === BUTTON HINTS ===
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "<", ">");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void AppsMenuActivity::refreshSystemInfo() {
  freeHeap = esp_get_free_heap_size();
  batteryPercent = (uint8_t)powerManager.getBatteryPercentage();
  wifiConnected = (WiFi.status() == WL_CONNECTED);
  lastInfoRefresh = millis();

  clockAvailable = halClock.isAvailable();
  if (clockAvailable) {
    halClock.formatTime(clockStr, sizeof(clockStr), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat != 0);
  }
}

void AppsMenuActivity::loadLastUsed() {
  for (int i = 0; i < ITEM_COUNT; i++) {
    lastUsedName[i][0] = '\0';
    char path[40];
    snprintf(path, sizeof(path), "/biscuit/lastused_%d.txt", i);
    HalFile file;
    if (Storage.openFileForRead("APPS", path, file)) {
      int len = file.read((uint8_t*)lastUsedName[i], 31);
      if (len > 0) {
        lastUsedName[i][len] = '\0';
        // Strip trailing newline
        if (len > 0 && lastUsedName[i][len - 1] == '\n') {
          lastUsedName[i][len - 1] = '\0';
        }
      }
      file.close();
    }
  }
}

void AppsMenuActivity::drawStatusBar() const {
  const auto pageWidth = renderer.getScreenWidth();
  constexpr int pad = 14;

  // Left: branding
  renderer.drawText(UI_12_FONT_ID, pad, 10, "panda", true, EpdFontFamily::BOLD);

  // Right side: build right-to-left to avoid overlap

  int rightX = pageWidth - pad;

  // Battery (rightmost) — drawBatteryRight draws the icon at rect.x and the
  // percentage text further left of that, so reserve space for both (icon +
  // spacing + percentage text width) before positioning anything else.
  constexpr int batteryIconW = 15;
  constexpr int batteryPercentSpacing = 4;
  char battPctStr[8];
  snprintf(battPctStr, sizeof(battPctStr), "%u%%", (unsigned)batteryPercent);
  const int battPctW = renderer.getTextWidth(SMALL_FONT_ID, battPctStr);
  GUI.drawBatteryRight(renderer, Rect{rightX - batteryIconW, 14, batteryIconW, 12});
  rightX -= batteryIconW + batteryPercentSpacing + battPctW + 10;

  // WiFi dot — filled when connected, hollow ring when not (rounded to a true
  // circle rather than a hard-edged square, matching the tile grid below)
  constexpr int dotSize = 7;
  if (wifiConnected) {
    renderer.fillRoundedRect(rightX - dotSize, 15, dotSize, dotSize, dotSize / 2, Color::Black);
  } else {
    renderer.drawRoundedRect(rightX - dotSize, 15, dotSize, dotSize, 1, dotSize / 2, true);
  }
  rightX -= dotSize + 10;

  // Clock (X3 only — DS3231 RTC; hidden entirely on X4 rather than shown broken).
  // Value is cached from the last refreshSystemInfo() call and only redrawn when
  // something else already triggers a repaint — never a repaint source itself.
  if (clockAvailable && clockStr[0] != '\0') {
    int clockW = renderer.getTextWidth(SMALL_FONT_ID, clockStr);
    renderer.drawText(SMALL_FONT_ID, rightX - clockW, 14, clockStr);
  }

  // Separator line
  renderer.drawLine(pad, 38, pageWidth - pad, 38, true);
}

void AppsMenuActivity::drawTile(int index, int x, int y, int w, int h, bool selected) const {
  // Rounded tile corners for a softer, more modern grid; radius is clamped
  // so it never overwhelms a narrow/short tile.
  int radius = 10;
  if (h / 3 < radius) radius = h / 3;
  if (w / 3 < radius) radius = w / 3;

  if (selected) {
    // Dithered gray fill instead of a hard black invert — matches the
    // selection style LyraTheme already uses for tab bars/lists elsewhere,
    // so text stays legible without inverting it. A heavier outline keeps
    // the focused tile unambiguous at a glance for button-only navigation.
    renderer.fillRoundedRect(x, y, w, h, radius, Color::LightGray);
    renderer.drawRoundedRect(x, y, w, h, 2, radius, true);
  } else {
    renderer.drawRoundedRect(x, y, w, h, 1, radius, true);
  }

  constexpr int pad = 12;
  // Icon width (32px) + spacing before the name/subtitle text column, matching
  // the icon-left convention LyraTheme::drawButtonMenu uses for Home's menu rows.
  constexpr int iconSize = 32;
  constexpr int textX = 42;  // = iconSize + hPaddingInSelection(8) + 2

  // --- Zone 1: Top — icon + category name + subtitle ---
  int nameY = y + pad;
  const char* name = "";
  const char* subtitle = "";
  int appCount = 0;
  const uint8_t* icon = nullptr;

  switch (index) {
    case 0:
      name = "RECON";
      subtitle = "Scan & monitor";
      appCount = 14;
      icon = RadarIcon;
      break;
    case 1:
      name = "OFFENSE";
      subtitle = "Scan/profile/test";
      appCount = 21;
      icon = CrosshairIcon;
      break;
    case 2:
      name = "DEFENSE";
      subtitle = "Ghost & protect";
      appCount = 12;
      icon = ShieldIcon;
      break;
    case 3:
      name = "COMMS";
      subtitle = "Chat & share";
      appCount = 5;
      icon = RadioIcon;
      break;
    case 4:
      name = "TOOLS";
      subtitle = "Utilities";
      appCount = 37;
      icon = WrenchIcon;
      break;
    case 5:
      name = "GAMES";
      subtitle = "Entertainment";
      appCount = 11;
      icon = Gamepad2Icon;
      break;
    case 6:
      name = "READER";
      subtitle = "Books & OPDS";
      appCount = 5;
      icon = BookIcon;
      break;
    case 7:
      name = "SETTINGS";
      subtitle = "System & config";
      appCount = 7;
      icon = Settings2Icon;
      break;
  }

  if (icon != nullptr) {
    renderer.drawIcon(icon, x + pad, y + pad + 2, iconSize);
  }
  renderer.drawText(UI_12_FONT_ID, x + pad + textX, nameY, name, true, EpdFontFamily::BOLD);
  nameY += renderer.getLineHeight(UI_12_FONT_ID) + 2;
  renderer.drawText(SMALL_FONT_ID, x + pad + textX, nameY, subtitle, true);

  // --- Zone 2: Bottom-right — app count (skip for modules with 0) ---
  int countY = y + h - pad - renderer.getLineHeight(SMALL_FONT_ID);
  if (appCount > 0) {
    char countStr[16];
    snprintf(countStr, sizeof(countStr), "%d apps", appCount);
    int countW = renderer.getTextWidth(SMALL_FONT_ID, countStr);
    renderer.drawText(SMALL_FONT_ID, x + w - pad - countW, countY, countStr, true);
  }

  // --- Badge indicator (top-right corner of tile) ---
  int badge = 0;
  bool showBang = false;
  switch (index) {
    case 0:
      badge = badgeRecon;
      break;  // recon — device alerts
    case 2:
      showBang = (badgeSecurity < 0);
      break;  // defense — PIN not set
    default:
      break;
  }

  if (badge > 0 || showBang) {
    constexpr int badgeSize = 18;
    int badgeX = x + w - badgeSize - 6;
    int badgeY = y + 6;
    // Circular badge chip — always black-filled with white text now that
    // tiles no longer invert to solid black on selection, so it reads
    // clearly against either a white or dithered-gray tile background.
    renderer.fillRoundedRect(badgeX, badgeY, badgeSize, badgeSize, badgeSize / 2, Color::Black);
    // Draw badge text
    char badgeStr[4];
    if (showBang) {
      snprintf(badgeStr, sizeof(badgeStr), "!");
    } else {
      snprintf(badgeStr, sizeof(badgeStr), "%d", badge);
    }
    int bw = renderer.getTextWidth(SMALL_FONT_ID, badgeStr);
    renderer.drawText(SMALL_FONT_ID, badgeX + badgeSize / 2 - bw / 2, badgeY + 3, badgeStr, false);
  }

  // --- Zone 3: Bottom-left — live status (selected tile only) ---
  if (selected) {
    char statusStr[48] = "";
    switch (index) {
      case 0:  // RECON
        snprintf(statusStr, sizeof(statusStr), wifiConnected ? "WiFi: ready" : "WiFi: off");
        break;
      case 1:  // OFFENSE
        snprintf(statusStr, sizeof(statusStr), wifiConnected ? "WiFi: connected" : "WiFi: off");
        break;
      case 2:  // DEFENSE
        snprintf(statusStr, sizeof(statusStr), "RF: %s", wifiConnected ? "active" : "silent");
        break;
      case 5:  // GAMES
      case 6:  // READER
        if (lastUsedName[index][0] != '\0') {
          snprintf(statusStr, sizeof(statusStr), "Last: %s", lastUsedName[index]);
        }
        break;
      default:
        break;
    }
    if (statusStr[0] != '\0') {
      int statusY = countY - renderer.getLineHeight(SMALL_FONT_ID) - 4;
      renderer.drawText(SMALL_FONT_ID, x + pad, statusY, statusStr, true);
    }
  }
}
