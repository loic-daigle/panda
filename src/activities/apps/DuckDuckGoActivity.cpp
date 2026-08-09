#include "DuckDuckGoActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Txt.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/reader/TxtReaderActivity.h"
#include "activities/util/DownloadWatchdog.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/util/WifiConnectHelper.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/RadioManager.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace {

// All persistent DuckDuckGo data lives under /biscuit/ per this device's SD
// card convention.
constexpr const char* kBaseDir = "/biscuit/duckduckgo";
constexpr const char* kWebsitesDir = "/biscuit/duckduckgo/websites";
constexpr const char* kDownloadTmpPath = "/biscuit/duckduckgo/download.tmp";

std::string urlDecode(const std::string& src) {
  std::string ret;
  int ii;
  for (size_t i = 0; i < src.length(); i++) {
    if (src[i] == '%') {
      if (i + 2 < src.length()) {
        sscanf(src.substr(i + 1, 2).c_str(), "%x", &ii);
        ret += static_cast<char>(ii);
        i += 2;
      }
    } else if (src[i] == '+') {
      ret += ' ';
    } else {
      ret += src[i];
    }
  }
  return ret;
}

std::string decodeHtmlEntities(const std::string& input) {
  std::string output;
  output.reserve(input.length());
  for (size_t i = 0; i < input.length();) {
    if (input[i] == '&') {
      size_t semi = input.find(';', i);
      if (semi != std::string::npos && semi - i < 10) {
        std::string entity = input.substr(i + 1, semi - i - 1);
        if (entity == "amp") { output += '&'; i = semi + 1; continue; }
        if (entity == "quot") { output += '"'; i = semi + 1; continue; }
        if (entity == "lt") { output += '<'; i = semi + 1; continue; }
        if (entity == "gt") { output += '>'; i = semi + 1; continue; }
        if (entity == "apos") { output += '\''; i = semi + 1; continue; }
        if (entity == "nbsp") { output += ' '; i = semi + 1; continue; }
        if (entity == "#x27" || entity == "#39") { output += '\''; i = semi + 1; continue; }
      }
    }
    output += input[i];
    i++;
  }
  return output;
}

std::string cleanHtmlText(const std::string& input) {
  std::string decoded = decodeHtmlEntities(input);
  std::string output;
  bool lastWasSpace = true;
  for (char c : decoded) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!lastWasSpace) {
        output += ' ';
        lastWasSpace = true;
      }
    } else {
      output += c;
      lastWasSpace = false;
    }
  }
  if (!output.empty() && output.back() == ' ') {
    output.pop_back();
  }
  return output;
}

std::string extractHref(const std::string& attribs) {
  size_t pos = attribs.find("href");
  if (pos == std::string::npos) {
    pos = attribs.find("HREF");
  }
  if (pos == std::string::npos) {
    return "";
  }

  size_t eqPos = attribs.find('=', pos);
  if (eqPos == std::string::npos) {
    return "";
  }

  size_t valPos = eqPos + 1;
  while (valPos < attribs.length() && (attribs[valPos] == ' ' || attribs[valPos] == '\t')) {
    valPos++;
  }

  if (valPos >= attribs.length()) {
    return "";
  }

  char quote = attribs[valPos];
  if (quote == '"' || quote == '\'') {
    size_t endQuote = attribs.find(quote, valPos + 1);
    if (endQuote != std::string::npos) {
      return attribs.substr(valPos + 1, endQuote - (valPos + 1));
    }
    return attribs.substr(valPos + 1);
  }
  size_t space = attribs.find_first_of(" \t", valPos);
  if (space != std::string::npos) {
    return attribs.substr(valPos, space - valPos);
  }
  return attribs.substr(valPos);
}

// Parses <a href="...">text</a> results out of the raw DuckDuckGo lite HTML
// results page. Operates on the in-memory string returned by
// HttpDownloader::fetchUrl (the page is small and bounded, so buffering it
// is fine on this device's 380KB SRAM).
bool parseDuckDuckGoResults(const std::string& html, std::vector<DuckLink>& links) {
  links.clear();

  size_t pos = 0;
  const size_t len = html.length();
  auto nextChar = [&]() -> int {
    if (pos >= len) return -1;
    return static_cast<unsigned char>(html[pos++]);
  };

  enum class ParserState { Scanning, InTag, InTagAttribs, InAnchorContent };

  ParserState state = ParserState::Scanning;
  std::string currentTagName;
  std::string currentTagAttribs;
  std::string currentText;

  int c;
  while ((c = nextChar()) != -1) {
    char ch = static_cast<char>(c);

    switch (state) {
      case ParserState::Scanning:
        if (ch == '<') {
          currentTagName.clear();
          currentTagAttribs.clear();
          state = ParserState::InTag;
        }
        break;

      case ParserState::InTag:
        if (ch == '>') {
          std::string lowerTag = currentTagName;
          std::transform(lowerTag.begin(), lowerTag.end(), lowerTag.begin(), ::tolower);
          if (lowerTag == "a") {
            currentText.clear();
            state = ParserState::InAnchorContent;
          } else {
            state = ParserState::Scanning;
          }
        } else if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
          std::string lowerTag = currentTagName;
          std::transform(lowerTag.begin(), lowerTag.end(), lowerTag.begin(), ::tolower);
          if (lowerTag == "a") {
            currentTagAttribs.clear();
            state = ParserState::InTagAttribs;
          } else {
            state = ParserState::Scanning;
          }
        } else {
          currentTagName += ch;
        }
        break;

      case ParserState::InTagAttribs:
        if (ch == '>') {
          currentText.clear();
          state = ParserState::InAnchorContent;
        } else {
          currentTagAttribs += ch;
        }
        break;

      case ParserState::InAnchorContent:
        if (ch == '<') {
          int nextC = nextChar();
          if (nextC == -1) break;
          char nextCh = static_cast<char>(nextC);
          if (nextCh == '/') {
            std::string closeTag;
            int tc;
            while ((tc = nextChar()) != -1) {
              char tch = static_cast<char>(tc);
              if (tch == '>') break;
              closeTag += tch;
            }
            std::transform(closeTag.begin(), closeTag.end(), closeTag.begin(), ::tolower);
            if (closeTag == "a") {
              std::string href = extractHref(currentTagAttribs);
              std::string cleanText = cleanHtmlText(currentText);

              if (!href.empty() && !cleanText.empty()) {
                std::string targetUrl = href;
                size_t uddgPos = targetUrl.find("uddg=");
                if (uddgPos != std::string::npos) {
                  size_t start = uddgPos + 5;
                  size_t end = targetUrl.find('&', start);
                  std::string encoded =
                      (end == std::string::npos) ? targetUrl.substr(start) : targetUrl.substr(start, end - start);
                  targetUrl = urlDecode(encoded);
                }

                if (targetUrl.rfind("http", 0) == 0) {
                  bool dup = false;
                  for (const auto& l : links) {
                    if (l.url == targetUrl) { dup = true; break; }
                  }
                  if (!dup) {
                    links.push_back({cleanText, targetUrl});
                  }
                }
              }
              state = ParserState::Scanning;
            } else {
              currentText += '<';
              currentText += '/';
              currentText += closeTag;
              currentText += '>';
            }
          } else {
            // Nested tag opening, skip until '>'
            int tc;
            while ((tc = nextChar()) != -1) {
              if (static_cast<char>(tc) == '>') break;
            }
          }
        } else {
          currentText += ch;
        }
        break;
    }
  }

  return !links.empty();
}

// Streams a downloaded HTML page from srcPath to a plain-text destPath,
// stripping tags and decoding entities as it goes. There is no HTML renderer
// on this device — downloaded pages are always converted to plain text so
// TxtReaderActivity can display them. Reads/writes a byte at a time to keep
// RAM usage flat regardless of page size.
bool convertHtmlFileToText(const std::string& srcPath, const std::string& destPath) {
  HalFile in;
  if (!Storage.openFileForRead("DDG", srcPath.c_str(), in)) {
    return false;
  }
  HalFile out;
  if (!Storage.openFileForWrite("DDG", destPath.c_str(), out)) {
    in.close();
    return false;
  }

  bool inTag = false;
  bool lastWasSpace = true;
  bool inEntity = false;
  std::string entityBuf;

  auto emit = [&](char ch) {
    if (std::isspace(static_cast<unsigned char>(ch))) {
      if (!lastWasSpace) {
        out.write(static_cast<uint8_t>(' '));
        lastWasSpace = true;
      }
    } else {
      out.write(static_cast<uint8_t>(ch));
      lastWasSpace = false;
    }
  };

  int c;
  while ((c = in.read()) != -1) {
    char ch = static_cast<char>(c);

    if (inEntity) {
      if (ch == ';') {
        if (entityBuf == "amp") emit('&');
        else if (entityBuf == "quot") emit('"');
        else if (entityBuf == "lt") emit('<');
        else if (entityBuf == "gt") emit('>');
        else if (entityBuf == "apos" || entityBuf == "#x27" || entityBuf == "#39") emit('\'');
        else if (entityBuf == "nbsp") emit(' ');
        inEntity = false;
        entityBuf.clear();
        continue;
      }
      entityBuf += ch;
      if (entityBuf.length() > 10) {
        inEntity = false;
        entityBuf.clear();
      }
      continue;
    }

    if (ch == '<') { inTag = true; continue; }
    if (ch == '>') { inTag = false; continue; }
    if (inTag) continue;
    if (ch == '&') { inEntity = true; entityBuf.clear(); continue; }

    emit(ch);
  }

  in.close();
  out.close();
  return true;
}

static void ddgFetchTaskFunc(void* param) {
  auto* activity = static_cast<DuckDuckGoActivity*>(param);
  activity->runBackgroundFetch();
  vTaskDelete(nullptr);
}

}  // namespace

void DuckDuckGoActivity::onEnter() {
  Activity::onEnter();

  // Don't force the radio on just to browse the offline page list — WiFi is
  // only powered up lazily, right before a search or download actually needs it
  // (see performBackgroundSearch() / downloadActivePost()).
  Storage.ensureDirectoryExists("/biscuit");
  Storage.ensureDirectoryExists(kBaseDir);
  Storage.ensureDirectoryExists(kWebsitesDir);

  errorMessage.clear();
  loadOfflineWebsitesList();
  state = DDGState::OfflineList;
  selectedIndex = 0;

  fetchTaskHandle = nullptr;
  cancelFetch = false;
  pendingUpdateSearch = false;
  backgroundFetchFailed = false;

  requestUpdate();
}

void DuckDuckGoActivity::onExit() {
  Activity::onExit();
  if (fetchTaskHandle != nullptr) {
    cancelFetch = true;
    int waitCount = 0;
    while (fetchTaskHandle != nullptr && waitCount < 500) {
      delay(10);
      waitCount++;
    }
    if (fetchTaskHandle != nullptr) {
      LOG_ERR("DDG", "Task failed to cancel! Forcing kill.");
      TaskHandle_t tempHandle = static_cast<TaskHandle_t>(fetchTaskHandle);
      fetchTaskHandle = nullptr;
      vTaskDelete(tempHandle);
    }
  }
  // Matches WeatherActivity: clear heap fragmentation accumulated during the
  // WiFi session with a silent restart, but only if WiFi was actually used.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void DuckDuckGoActivity::loadOfflineWebsitesList() {
  offlineWebsites.clear();
  std::vector<String> files = Storage.listFiles(kWebsitesDir);
  for (const auto& file : files) {
    std::string filename = file.c_str();
    if (filename.length() > 4 && filename.substr(filename.length() - 4) == ".txt") {
      offlineWebsites.push_back(filename.substr(0, filename.length() - 4));
    }
  }
  std::sort(offlineWebsites.begin(), offlineWebsites.end());
}

std::string DuckDuckGoActivity::websiteFilePathForTitle(const std::string& title) const {
  std::string sanitized = StringUtils::sanitizeFilename(title, 60);
  if (sanitized.empty()) {
    sanitized = "page";
  }
  return std::string(kWebsitesDir) + "/" + sanitized + ".txt";
}

void DuckDuckGoActivity::openOfflinePage(const std::string& filepath) {
  auto txt = std::unique_ptr<Txt>(new Txt(filepath, "/.crosspoint"));
  if (txt && txt->load()) {
    activityManager.pushActivity(std::make_unique<TxtReaderActivity>(renderer, mappedInput, std::move(txt), 1));
  }
}

void DuckDuckGoActivity::performBackgroundSearch() {
  if (fetchTaskHandle != nullptr) return;
  state = DDGState::Loading;
  backgroundFetchFailed = false;
  pendingUpdateSearch = false;
  cancelFetch = false;
  requestUpdate();

  RADIO.ensureWifi();

  xTaskCreate(ddgFetchTaskFunc, "ddg_fetch", 8192, this, 5, reinterpret_cast<TaskHandle_t*>(&fetchTaskHandle));
}

void DuckDuckGoActivity::runBackgroundFetch() {
  DownloadWatchdog::start(35000);
  errorMessage.clear();

  bool success = false;
  // WifiConnectHelper auto-connects to a saved network without pushing an
  // interactive activity — required here since this runs on a background
  // FreeRTOS task, not the render/input task.
  if (WifiConnectHelper::connectToDefaultWifi()) {
    if (!WifiConnectHelper::waitForTimeSync()) {
      errorMessage = "Clock sync failed. HTTPS requests may fail.";
    }
    int retries = 3;
    while (retries-- > 0 && !cancelFetch) {
      success = fetchSearchData();
      if (success) break;
      if (retries > 0 && !cancelFetch) {
        delay(1500);
      }
    }
  } else {
    errorMessage = "Could not connect to WiFi.";
  }

  DownloadWatchdog::stop();

  if (DownloadWatchdog::gotTimeout) {
    LOG_ERR("DDG", "Background fetch timed out!");
    backgroundFetchFailed = true;
    errorMessage = "Request timed out.";
  } else if (!success) {
    backgroundFetchFailed = true;
    if (errorMessage.empty()) {
      errorMessage = "Search request failed.";
    }
  } else {
    backgroundFetchFailed = false;
    errorMessage.clear();
  }

  if (cancelFetch) {
    fetchTaskHandle = nullptr;
    return;
  }

  pendingUpdateSearch = true;
}

bool DuckDuckGoActivity::fetchSearchData() {
  // lite.duckduckgo.com is a much smaller/simpler response than the html
  // endpoint (direct result URLs, no /l/?uddg= redirect wrapper) — a better
  // fit for this device's 380KB SRAM, and we already only display title+url
  // (no snippet), so nothing is lost.
  std::string url = UrlUtils::encodeUnsafeUrlChars("https://lite.duckduckgo.com/lite/?q=" + searchQuery);

  std::string html;
  if (!HttpDownloader::fetchUrl(url, html)) {
    errorMessage = "Search request failed.";
    return false;
  }

  if (cancelFetch) {
    return false;
  }

  return parseDuckDuckGoResults(html, bgSearchResults);
}

void DuckDuckGoActivity::downloadActivePost() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(searchResults.size())) {
    return;
  }

  std::string downloadUrl = searchResults[selectedIndex].url;
  std::string destPath = websiteFilePathForTitle(searchResults[selectedIndex].title);

  GUI.drawPopup(renderer, "Downloading...");
  RADIO.ensureWifi();

  if (!WifiConnectHelper::connectToDefaultWifi()) {
    GUI.drawPopup(renderer, "WiFi connection failed!");
    delay(1500);
    requestUpdate();
    return;
  }

  bool success = false;
  int retries = 3;
  while (retries > 0) {
    GUI.drawPopup(renderer, "Downloading...");
    auto result = HttpDownloader::downloadToFile(downloadUrl, kDownloadTmpPath, nullptr, nullptr);
    if (result == HttpDownloader::OK) {
      success = true;
      break;
    }
    retries--;
    if (retries > 0) {
      delay(1000);
    }
  }

  if (success) {
    GUI.drawPopup(renderer, "Processing...");
    if (Storage.exists(destPath.c_str())) {
      Storage.remove(destPath.c_str());
    }
    if (convertHtmlFileToText(kDownloadTmpPath, destPath)) {
      Storage.remove(kDownloadTmpPath);
      loadOfflineWebsitesList();
      openOfflinePage(destPath);
      return;
    }
    Storage.remove(kDownloadTmpPath);
    GUI.drawPopup(renderer, "Save failed!");
    delay(1500);
  } else {
    Storage.remove(kDownloadTmpPath);
    GUI.drawPopup(renderer, "Download failed!");
    delay(1500);
  }
  requestUpdate();
}

void DuckDuckGoActivity::loop() {
  if (pendingUpdateSearch) {
    pendingUpdateSearch = false;
    fetchTaskHandle = nullptr;
    if (DownloadWatchdog::gotTimeout) {
      LOG_ERR("DDG", "Watchdog timeout! Returning home.");
      activityManager.goHome();
      return;
    }
    if (!backgroundFetchFailed) {
      searchResults = std::move(bgSearchResults);
      errorMessage.clear();
    } else {
      searchResults.clear();
    }
    selectedIndex = 0;
    state = DDGState::SearchResults;
    requestUpdate();
    return;
  }

  if (state == DDGState::Loading && millis() - lastSpinnerUpdate >= 600) {
    lastSpinnerUpdate = millis();
    spinnerFrame = (spinnerFrame + 1) % 3;
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (state == DDGState::Loading) {
      if (fetchTaskHandle != nullptr) {
        cancelFetch = true;
        int waitCount = 0;
        while (fetchTaskHandle != nullptr && waitCount < 500) {
          delay(10);
          waitCount++;
        }
        if (fetchTaskHandle != nullptr) {
          LOG_ERR("DDG", "Task failed to cancel! Forcing kill.");
          TaskHandle_t tempHandle = static_cast<TaskHandle_t>(fetchTaskHandle);
          fetchTaskHandle = nullptr;
          vTaskDelete(tempHandle);
        }
      }
      if (!searchResults.empty()) {
        state = DDGState::SearchResults;
      } else {
        state = DDGState::OfflineList;
        loadOfflineWebsitesList();
      }
      requestUpdate();
    } else if (state == DDGState::SearchResults) {
      state = DDGState::OfflineList;
      loadOfflineWebsitesList();
      selectedIndex = 0;
      requestUpdate();
    } else {
      finish();
    }
    return;
  }

  if (state == DDGState::OfflineList) {
    const int totalItems = static_cast<int>(offlineWebsites.size()) + 1;
    buttonNavigator.onNext([this, totalItems] {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, totalItems);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this, totalItems] {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, totalItems);
      requestUpdate();
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (selectedIndex == 0) {
        startActivityForResult(
            std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "DuckDuckGo Search", "", 128),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                auto keyboardResult = std::get_if<KeyboardResult>(&result.data);
                if (keyboardResult && !keyboardResult->text.empty()) {
                  searchQuery = keyboardResult->text;
                  state = DDGState::Loading;
                  requestUpdate();
                  performBackgroundSearch();
                  return;
                }
              }
              requestUpdate();
            });
      } else {
        const std::string& name = offlineWebsites[selectedIndex - 1];
        openOfflinePage(std::string(kWebsitesDir) + "/" + name + ".txt");
      }
    }
    return;
  }

  if (state == DDGState::SearchResults) {
    if (!errorMessage.empty()) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        state = DDGState::Loading;
        errorMessage.clear();
        requestUpdate();
        performBackgroundSearch();
      }
      return;
    }

    if (searchResults.empty()) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        startActivityForResult(
            std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "DuckDuckGo Search", searchQuery, 128),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                auto keyboardResult = std::get_if<KeyboardResult>(&result.data);
                if (keyboardResult && !keyboardResult->text.empty()) {
                  searchQuery = keyboardResult->text;
                  state = DDGState::Loading;
                  requestUpdate();
                  performBackgroundSearch();
                  return;
                }
              }
              requestUpdate();
            });
      }
      return;
    }

    const int totalItems = static_cast<int>(searchResults.size());
    buttonNavigator.onNext([this, totalItems] {
      selectedIndex = ButtonNavigator::nextIndex(selectedIndex, totalItems);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this, totalItems] {
      selectedIndex = ButtonNavigator::previousIndex(selectedIndex, totalItems);
      requestUpdate();
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      std::string filepath = websiteFilePathForTitle(searchResults[selectedIndex].title);
      if (Storage.exists(filepath.c_str())) {
        openOfflinePage(filepath);
      } else {
        downloadActivePost();
      }
    }
    return;
  }
}

void DuckDuckGoActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (state == DDGState::OfflineList) {
    char subtitle[32];
    snprintf(subtitle, sizeof(subtitle), "%d saved page%s", static_cast<int>(offlineWebsites.size()),
              offlineWebsites.size() == 1 ? "" : "s");
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "DuckDuckGo", subtitle);
  } else {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "DuckDuckGo");
  }

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int contentHeight = contentBottom - contentTop;

  if (state == DDGState::Loading) {
    const int centerY = contentTop + contentHeight / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, centerY - 30, "Searching DuckDuckGo...", true, EpdFontFamily::BOLD);
    GUI.drawSpinner(renderer, pageWidth / 2, centerY + 20, nullptr, spinnerFrame);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (state == DDGState::OfflineList) {
    renderOfflineList();

  } else if (state == DDGState::SearchResults) {
    if (!errorMessage.empty()) {
      const int textY = contentTop + contentHeight / 2 - 50;
      renderer.drawCenteredText(UI_12_FONT_ID, textY, errorMessage.c_str(), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(SMALL_FONT_ID, textY + 26, "Configure your network or retry search.");

      const int pillW = 180, pillH = 40;
      const int pillX = (pageWidth - pillW) / 2, pillY = textY + 60;
      renderer.fillRoundedRect(pillX, pillY, pillW, pillH, 10, Color::Black);
      renderer.drawCenteredText(UI_10_FONT_ID, pillY + 10, tr(STR_RETRY), false, EpdFontFamily::BOLD);

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), nullptr, nullptr);
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    } else if (searchResults.empty()) {
      const int textY = contentTop + contentHeight / 2 - 30;
      renderer.drawCenteredText(UI_12_FONT_ID, textY, "No results found.", true, EpdFontFamily::BOLD);

      const int pillW = 200, pillH = 40;
      const int pillX = (pageWidth - pillW) / 2, pillY = textY + 30;
      renderer.fillRoundedRect(pillX, pillY, pillW, pillH, 10, Color::Black);
      renderer.drawCenteredText(UI_10_FONT_ID, pillY + 10, "Search Again", false, EpdFontFamily::BOLD);

      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SEARCH), nullptr, nullptr);
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    } else {
      renderSearchResults();
    }
  }

  renderer.displayBuffer();
}

// Rounded "+ Search DuckDuckGo" pill above a plain scrollable list of saved
// pages -- same visual language as CalendarActivity/WikipediaActivity.
void DuckDuckGoActivity::renderOfflineList() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int pad = metrics.contentSidePadding;
  const int listTop = metrics.topPadding + metrics.headerHeight + 6;
  const int listBottom = pageHeight - metrics.buttonHintsHeight;
  const int listH = listBottom - listTop;

  const int pillH = 40;
  const int rowH = 36;

  const int totalHeight = pillH + static_cast<int>(offlineWebsites.size()) * rowH;
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
      renderer.drawText(UI_10_FONT_ID, pad + 14, pillY + 9, "+ Search DuckDuckGo", !selected, EpdFontFamily::BOLD);
    }
  }

  for (int i = 0; i < static_cast<int>(offlineWebsites.size()); i++) {
    const int y = listTop + pillH + i * rowH - scrollTop;
    if (y + rowH < listTop || y > listBottom) continue;

    const bool selected = (i + 1) == selectedIndex;
    if (selected) renderer.fillRect(0, y, pageWidth, rowH, true);

    std::string title = renderer.truncatedText(UI_10_FONT_ID, offlineWebsites[i].c_str(), pageWidth - pad * 2);
    renderer.drawText(UI_10_FONT_ID, pad, y + 8, title.c_str(), !selected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// Two-line result rows: bold title, then either the URL or a small "Saved
// offline" status line beneath it.
void DuckDuckGoActivity::renderSearchResults() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int pad = metrics.contentSidePadding;
  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listBottom = pageHeight - metrics.buttonHintsHeight;
  const int listH = listBottom - listTop;
  const int rowH = 44;

  const int totalHeight = static_cast<int>(searchResults.size()) * rowH;
  const int selectedTop = selectedIndex * rowH;

  int scrollTop = 0;
  if (totalHeight > listH) {
    scrollTop = selectedTop + rowH / 2 - listH / 2;
    scrollTop = std::max(0, std::min(scrollTop, totalHeight - listH));
  }

  for (int i = 0; i < static_cast<int>(searchResults.size()); i++) {
    const int y = listTop + i * rowH - scrollTop;
    if (y + rowH < listTop || y > listBottom) continue;

    const bool selected = i == selectedIndex;
    if (selected) renderer.fillRect(0, y, pageWidth, rowH, true);

    const int maxW = pageWidth - pad * 2;
    std::string title = renderer.truncatedText(UI_10_FONT_ID, searchResults[i].title.c_str(), maxW);
    renderer.drawText(UI_10_FONT_ID, pad, y + 4, title.c_str(), !selected);

    const std::string path = websiteFilePathForTitle(searchResults[i].title);
    const bool saved = Storage.exists(path.c_str());
    std::string subtitle =
        renderer.truncatedText(SMALL_FONT_ID, saved ? "Saved offline" : searchResults[i].url.c_str(), maxW);
    renderer.drawText(SMALL_FONT_ID, pad, y + 4 + renderer.getLineHeight(UI_10_FONT_ID), subtitle.c_str(), !selected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
