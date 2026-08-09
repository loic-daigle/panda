#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

enum class WikiState { OfflineList, SearchResults, Loading };

// Wikipedia app: searches the Wikipedia API (action=opensearch), then lets
// the user download an article's plain-text extract (action=query&prop=
// extracts&explaintext) and save it to SD for offline reading via
// TxtReaderActivity. The network fetch runs on a background FreeRTOS task so
// the render/input loop never blocks on WiFi/HTTP -- mirrors
// DuckDuckGoActivity / WeatherActivity's structure in this codebase.
class WikipediaActivity final : public Activity {
 public:
  explicit WikipediaActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Wikipedia", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Entry points for the background fetch task (see wikiFetchTaskFunc in the
  // .cpp). Public so the plain-C trampoline function can call them.
  void runBackgroundFetch();
  bool fetchSearchData();
  bool fetchArticleData();

 private:
  WikiState state = WikiState::OfflineList;
  std::vector<std::string> offlineArticles;
  std::vector<std::string> searchResults;

  std::string searchQuery;
  std::string articleToFetch;
  std::string currentArticleTitle;  // title returned by the last successful article fetch
  std::string errorMessage;

  int selectedIndex = 0;
  bool isSearchTask = false;  // which kind of fetch is running while state == Loading

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
  std::atomic<bool> cancelFetch{false};
  std::atomic<bool> pendingUpdateSearch{false};
  std::atomic<bool> pendingUpdateArticle{false};
  std::atomic<bool> backgroundFetchFailed{false};
  std::vector<std::string> bgSearchResults;

  void loadOfflineArticlesList();
  void performBackgroundSearch();
  void performBackgroundFetchArticle();
  void cancelFetchTask();
  void openArticle(const std::string& title);
  std::string articleFilePathFor(const std::string& title) const;
};
