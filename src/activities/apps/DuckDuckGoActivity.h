#pragma once

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

enum class DDGState {
  OfflineList,
  SearchResults,
  Loading
};

struct DuckLink {
  std::string title;
  std::string url;
};

// DuckDuckGo web search: fetches https://lite.duckduckgo.com/lite/?q=<query>
// (DuckDuckGo's lite HTML results page — no API key required), parses result
// links out of the raw HTML, and lets the user save a result page as plain
// text on the SD card for later offline reading. The network fetch runs on a
// background FreeRTOS task so the render/input loop never blocks on it.
class DuckDuckGoActivity final : public Activity {
 public:
  explicit DuckDuckGoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("DuckDuckGo", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Entry point for the background fetch task (see ddgFetchTaskFunc in the .cpp).
  // Public so the plain-C trampoline function can call it.
  void runBackgroundFetch();
  bool fetchSearchData();

 private:
  DDGState state = DDGState::OfflineList;
  std::vector<std::string> offlineWebsites;
  std::vector<DuckLink> searchResults;

  std::string searchQuery;
  std::string errorMessage;

  int selectedIndex = 0;

  ButtonNavigator buttonNavigator;

  // Loading-screen spinner (BaseTheme::drawSpinner), advanced on a 600ms
  // timer -- same pattern as SweepActivity/ProbeSnifferActivity/CalendarActivity.
  int spinnerFrame = 0;
  unsigned long lastSpinnerUpdate = 0;

  void renderOfflineList() const;
  void renderSearchResults() const;

  // void* (not TaskHandle_t) so this header doesn't need to pull in FreeRTOS
  // task headers; the .cpp casts it to TaskHandle_t where needed.
  void* fetchTaskHandle = nullptr;
  // volatile: written by the background fetch task (runBackgroundFetch(),
  // cancelFetch also read there) and consumed by loop()/onExit() on the main
  // task, matching WeatherActivity's cross-task flag-handoff convention.
  volatile bool cancelFetch = false;
  volatile bool pendingUpdateSearch = false;
  bool backgroundFetchFailed = false;
  std::vector<DuckLink> bgSearchResults;

  void loadOfflineWebsitesList();
  void performBackgroundSearch();
  void downloadActivePost();
  void openOfflinePage(const std::string& filepath);
  std::string websiteFilePathForTitle(const std::string& title) const;
};
