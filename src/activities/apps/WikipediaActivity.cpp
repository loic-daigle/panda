#include "WikipediaActivity.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>

#include "MappedInputManager.h"
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

// All persistent Wikipedia data lives under /biscuit/ per this device's SD
// card convention.
constexpr const char* kBaseDir = "/biscuit/wikipedia";
constexpr const char* kSearchTmpPath = "/biscuit/wikipedia/search.tmp";
constexpr const char* kArticleTmpPath = "/biscuit/wikipedia/article.tmp";
constexpr const char* kArticleTxtTmpPath = "/biscuit/wikipedia/article.txt.tmp";

// Strips a handful of Wikipedia-extract-specific unicode punctuation down to
// plain ASCII so the on-device fonts (which don't carry full unicode
// coverage) render article text cleanly.
std::string cleanUnicode(const std::string& input) {
  std::string output;
  output.reserve(input.size());
  for (size_t i = 0; i < input.size();) {
    unsigned char c = input[i];
    if (c < 0x80) {
      output += static_cast<char>(c);
      i++;
    } else if ((c & 0xE0) == 0xC0) {  // 2 bytes
      if (i + 1 < input.size()) {
        unsigned char c2 = input[i + 1];
        uint16_t codepoint = ((c & 0x1F) << 6) | (c2 & 0x3F);
        if (codepoint == 0x00A0) {  // Non-breaking space
          output += ' ';
        } else {
          output += input.substr(i, 2);
        }
      }
      i += 2;
    } else if ((c & 0xF0) == 0xE0) {  // 3 bytes
      if (i + 2 < input.size()) {
        unsigned char c2 = input[i + 1];
        unsigned char c3 = input[i + 2];
        uint16_t codepoint = ((c & 0x0F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
        if (codepoint == 0x2018 || codepoint == 0x2019) {  // Single quotes
          output += '\'';
        } else if (codepoint == 0x201C || codepoint == 0x201D) {  // Double quotes
          output += '"';
        } else if (codepoint == 0x2013 || codepoint == 0x2014) {  // En/em dash
          output += '-';
        } else if (codepoint == 0x200B) {  // Zero-width space
          // skip it
        } else {
          output += input.substr(i, 3);
        }
      }
      i += 3;
    } else if ((c & 0xF8) == 0xF0) {  // 4 bytes
      if (i + 3 < input.size()) {
        output += input.substr(i, 4);
      }
      i += 4;
    } else {
      i++;
    }
  }
  return output;
}

std::string sanitizedArticlePath(const std::string& title) {
  return std::string(kBaseDir) + "/" + StringUtils::sanitizeFilename(title, 100) + ".txt";
}

// Streams the `action=query&prop=extracts&explaintext` JSON response straight
// from SD to a plain-text .txt article, converting MediaWiki "== Heading =="
// markers to markdown-style "#" headings as it goes, one JSON string value at
// a time. This avoids ever holding the whole article (which can be hundreds
// of KB) or the whole JSON document in RAM at once -- important on a device
// with 380KB SRAM and no PSRAM.
bool parseAndSaveWikipediaArticle(const std::string& tempJsonPath, std::string& outTitle) {
  HalFile jsonFile;
  if (!Storage.openFileForRead("WIKI", tempJsonPath.c_str(), jsonFile)) {
    return false;
  }

  HalFile txtFile;
  if (!Storage.openFileForWrite("WIKI", kArticleTxtTmpPath, txtFile)) {
    jsonFile.close();
    return false;
  }

  enum class ParserState { Scanning, InString, AfterString, ExpectingColon, ExpectingValue, InValueString };

  ParserState state = ParserState::Scanning;
  std::string currentKey;
  std::string currentValue;
  std::string currentLine;
  bool inEscape = false;
  outTitle.clear();

  auto writeLineToTxt = [&](const std::string& rawLine) {
    std::string line = cleanUnicode(rawLine);
    if (line.empty()) {
      txtFile.write("\n", 1);
      return;
    }

    size_t startEquals = 0;
    while (startEquals < line.size() && line[startEquals] == '=') {
      startEquals++;
    }
    size_t endEquals = 0;
    while (endEquals < line.size() && line[line.size() - 1 - endEquals] == '=') {
      endEquals++;
    }

    if (startEquals >= 2 && startEquals == endEquals && startEquals < line.size()) {
      std::string headingText = line.substr(startEquals, line.size() - 2 * startEquals);
      size_t first = headingText.find_first_not_of(" ");
      size_t last = headingText.find_last_not_of(" ");
      if (first != std::string::npos && last != std::string::npos) {
        headingText = headingText.substr(first, (last - first + 1));
      }
      std::string heading(startEquals, '#');
      std::string formatted = heading + " " + headingText + "\n\n";
      txtFile.write(formatted.data(), formatted.size());
    } else {
      std::string formatted = line + "\n";
      txtFile.write(formatted.data(), formatted.size());
    }
  };

  int c;
  while ((c = jsonFile.read()) != -1) {
    char ch = static_cast<char>(c);

    switch (state) {
      case ParserState::Scanning:
        if (ch == '"') {
          currentKey.clear();
          state = ParserState::InString;
        }
        break;

      case ParserState::InString:
        if (inEscape) {
          currentKey += ch;
          inEscape = false;
        } else if (ch == '\\') {
          inEscape = true;
        } else if (ch == '"') {
          state = ParserState::AfterString;
        } else {
          currentKey += ch;
        }
        break;

      case ParserState::AfterString:
        if (std::isspace(static_cast<unsigned char>(ch))) {
          // ignore
        } else if (ch == ':') {
          state = ParserState::ExpectingColon;
        } else {
          state = ParserState::Scanning;
        }
        break;

      case ParserState::ExpectingColon:
        if (std::isspace(static_cast<unsigned char>(ch))) {
          // ignore
        } else if (ch == '"') {
          currentValue.clear();
          currentLine.clear();
          inEscape = false;
          if (currentKey == "title" || currentKey == "extract") {
            state = ParserState::InValueString;
          } else {
            state = ParserState::ExpectingValue;
          }
        } else {
          state = ParserState::Scanning;
        }
        break;

      case ParserState::ExpectingValue:
        if (inEscape) {
          inEscape = false;
        } else if (ch == '\\') {
          inEscape = true;
        } else if (ch == '"') {
          state = ParserState::Scanning;
        }
        break;

      case ParserState::InValueString:
        if (inEscape) {
          char escChar = 0;
          if (ch == 'n') {
            escChar = '\n';
          } else if (ch == 'r') {
            escChar = '\r';
          } else if (ch == 't') {
            escChar = '\t';
          } else if (ch == '"') {
            escChar = '"';
          } else if (ch == '\\') {
            escChar = '\\';
          } else if (ch == '/') {
            escChar = '/';
          } else if (ch == 'u') {
            uint16_t codepoint = 0;
            bool ok = true;
            for (int k = 0; k < 4; k++) {
              int hexDigit = jsonFile.read();
              if (hexDigit == -1) {
                ok = false;
                break;
              }
              char h = static_cast<char>(hexDigit);
              codepoint <<= 4;
              if (h >= '0' && h <= '9') {
                codepoint |= (h - '0');
              } else if (h >= 'a' && h <= 'f') {
                codepoint |= (h - 'a' + 10);
              } else if (h >= 'A' && h <= 'F') {
                codepoint |= (h - 'A' + 10);
              } else {
                ok = false;
                break;
              }
            }
            if (ok) {
              std::string utf8Str;
              if (codepoint < 0x80) {
                utf8Str += static_cast<char>(codepoint);
              } else if (codepoint < 0x800) {
                utf8Str += static_cast<char>(0xC0 | (codepoint >> 6));
                utf8Str += static_cast<char>(0x80 | (codepoint & 0x3F));
              } else {
                utf8Str += static_cast<char>(0xE0 | (codepoint >> 12));
                utf8Str += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                utf8Str += static_cast<char>(0x80 | (codepoint & 0x3F));
              }

              if (currentKey == "title") {
                currentValue += utf8Str;
              } else if (currentKey == "extract") {
                currentLine += utf8Str;
              }
            }
          } else {
            escChar = ch;
          }

          if (escChar != 0) {
            if (currentKey == "title") {
              currentValue += escChar;
            } else if (currentKey == "extract") {
              if (escChar == '\n') {
                writeLineToTxt(currentLine);
                currentLine.clear();
              } else {
                currentLine += escChar;
              }
            }
          }
          inEscape = false;
        } else if (ch == '\\') {
          inEscape = true;
        } else if (ch == '"') {
          if (currentKey == "title") {
            outTitle = currentValue;
          } else if (currentKey == "extract") {
            if (!currentLine.empty()) {
              writeLineToTxt(currentLine);
              currentLine.clear();
            }
          }
          state = ParserState::Scanning;
        } else {
          if (currentKey == "title") {
            currentValue += ch;
          } else if (currentKey == "extract") {
            if (ch == '\n') {
              writeLineToTxt(currentLine);
              currentLine.clear();
            } else {
              currentLine += ch;
            }
          }
        }
        break;
    }
  }

  jsonFile.close();
  txtFile.close();

  if (outTitle.empty()) {
    Storage.remove(kArticleTxtTmpPath);
    return false;
  }

  const std::string finalPath = sanitizedArticlePath(outTitle);
  HalFile finalFile;
  if (!Storage.openFileForWrite("WIKI", finalPath.c_str(), finalFile)) {
    Storage.remove(kArticleTxtTmpPath);
    return false;
  }

  const std::string titleHeader = "# " + outTitle + "\n\n";
  finalFile.write(titleHeader.data(), titleHeader.size());

  HalFile tempTxtFile;
  if (Storage.openFileForRead("WIKI", kArticleTxtTmpPath, tempTxtFile)) {
    char copyBuf[512];
    int bytesRead;
    while ((bytesRead = tempTxtFile.read(reinterpret_cast<uint8_t*>(copyBuf), sizeof(copyBuf))) > 0) {
      finalFile.write(copyBuf, bytesRead);
    }
    tempTxtFile.close();
  }
  finalFile.close();
  Storage.remove(kArticleTxtTmpPath);

  return true;
}

void wikiFetchTaskFunc(void* param) {
  auto* activity = static_cast<WikipediaActivity*>(param);
  activity->runBackgroundFetch();
  vTaskDelete(nullptr);
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void WikipediaActivity::onEnter() {
  Activity::onEnter();
  RADIO.ensureWifi();

  Storage.ensureDirectoryExists("/biscuit");
  Storage.ensureDirectoryExists(kBaseDir);

  errorMessage.clear();
  currentArticleTitle.clear();
  loadOfflineArticlesList();
  state = WikiState::OfflineList;
  selectedIndex = 0;

  fetchTaskHandle = nullptr;
  cancelFetch = false;
  isSearchTask = false;
  pendingUpdateSearch = false;
  pendingUpdateArticle = false;
  backgroundFetchFailed = false;

  requestUpdate();
}

void WikipediaActivity::onExit() {
  Activity::onExit();
  cancelFetchTask();
  RADIO.shutdown();
}

void WikipediaActivity::cancelFetchTask() {
  if (fetchTaskHandle != nullptr) {
    cancelFetch = true;
    int waitCount = 0;
    while (fetchTaskHandle != nullptr && waitCount < 500) {
      delay(10);
      waitCount++;
    }
    if (fetchTaskHandle != nullptr) {
      LOG_ERR("WIKI", "Task failed to cancel! Forcing kill.");
      TaskHandle_t tempHandle = static_cast<TaskHandle_t>(fetchTaskHandle);
      fetchTaskHandle = nullptr;
      vTaskDelete(tempHandle);
    }
  }
}

void WikipediaActivity::loadOfflineArticlesList() {
  offlineArticles.clear();
  std::vector<String> files = Storage.listFiles(kBaseDir);
  for (const auto& file : files) {
    std::string filename = file.c_str();
    if (filename.length() > 4 && filename.substr(filename.length() - 4) == ".txt") {
      offlineArticles.push_back(filename.substr(0, filename.length() - 4));
    }
  }
  std::sort(offlineArticles.begin(), offlineArticles.end());
}

std::string WikipediaActivity::articleFilePathFor(const std::string& title) const {
  return sanitizedArticlePath(title);
}

void WikipediaActivity::openArticle(const std::string& title) {
  const std::string filepath = articleFilePathFor(title);
  activityManager.pushActivity(std::make_unique<TxtReaderActivity>(renderer, mappedInput, filepath, true));
}

// ---------------------------------------------------------------------------
// Background fetch (runs on its own FreeRTOS task -- see wikiFetchTaskFunc)
// ---------------------------------------------------------------------------

void WikipediaActivity::performBackgroundSearch() {
  if (fetchTaskHandle != nullptr) return;
  state = WikiState::Loading;
  backgroundFetchFailed = false;
  isSearchTask = true;
  cancelFetch = false;
  requestUpdate();

  RADIO.ensureWifi();

  TaskHandle_t handle = nullptr;
  xTaskCreate(wikiFetchTaskFunc, "wiki_fetch", 8192, this, 5, &handle);
  fetchTaskHandle = handle;
}

void WikipediaActivity::performBackgroundFetchArticle() {
  if (fetchTaskHandle != nullptr) return;
  state = WikiState::Loading;
  backgroundFetchFailed = false;
  isSearchTask = false;
  cancelFetch = false;
  requestUpdate();

  RADIO.ensureWifi();

  TaskHandle_t handle = nullptr;
  xTaskCreate(wikiFetchTaskFunc, "wiki_fetch", 8192, this, 5, &handle);
  fetchTaskHandle = handle;
}

void WikipediaActivity::runBackgroundFetch() {
  DownloadWatchdog::start(35000);
  errorMessage.clear();

  bool success = false;
  // WifiConnectHelper auto-connects to a saved network without pushing an
  // interactive activity -- required here since this runs on a background
  // FreeRTOS task, not the render/input task.
  if (WifiConnectHelper::connectToDefaultWifi()) {
    if (!WifiConnectHelper::waitForTimeSync()) {
      errorMessage = "Clock sync failed. HTTPS requests may fail.";
    }
    int retries = 3;
    while (retries-- > 0 && !cancelFetch) {
      success = isSearchTask ? fetchSearchData() : fetchArticleData();
      if (success) break;
      if (retries > 0 && !cancelFetch) {
        delay(1500);
      }
    }
  } else {
    errorMessage = "Could not connect to WiFi.";
  }

  DownloadWatchdog::stop();

  if (cancelFetch) {
    fetchTaskHandle = nullptr;
    return;
  }

  if (DownloadWatchdog::gotTimeout) {
    LOG_ERR("WIKI", "Background fetch timed out!");
    backgroundFetchFailed = true;
    errorMessage = "Request timed out.";
  } else if (!success) {
    backgroundFetchFailed = true;
    if (errorMessage.empty()) {
      errorMessage = isSearchTask ? "Search request failed." : "Failed to download article.";
    }
  } else {
    backgroundFetchFailed = false;
    errorMessage.clear();
  }

  if (isSearchTask) {
    pendingUpdateSearch = true;
  } else {
    pendingUpdateArticle = true;
  }
}

bool WikipediaActivity::fetchSearchData() {
  const std::string url =
      UrlUtils::encodeUnsafeUrlChars("https://en.wikipedia.org/w/api.php?action=opensearch&search=" + searchQuery +
                                     "&limit=10&namespace=0&format=json");

  auto result = HttpDownloader::downloadToFile(url, kSearchTmpPath, nullptr, nullptr);
  if (result != HttpDownloader::OK) {
    errorMessage = (result == HttpDownloader::ABORTED) ? "Search cancelled." : "Search request failed.";
    Storage.remove(kSearchTmpPath);
    return false;
  }

  bool ok = false;
  HalFile file;
  if (Storage.openFileForRead("WIKI", kSearchTmpPath, file)) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();

    if (!err && doc.is<JsonArray>()) {
      JsonArray arr = doc.as<JsonArray>();
      if (arr.size() >= 2) {
        JsonArray titles = arr[1].as<JsonArray>();
        bgSearchResults.clear();
        for (JsonVariant val : titles) {
          bgSearchResults.push_back(val.as<std::string>());
        }
        ok = true;
      }
    }
  }
  Storage.remove(kSearchTmpPath);
  return ok;
}

bool WikipediaActivity::fetchArticleData() {
  const std::string url = UrlUtils::encodeUnsafeUrlChars(
      "https://en.wikipedia.org/w/api.php?action=query&prop=extracts&explaintext=&titles=" + articleToFetch +
      "&format=json&redirects=1");

  auto result = HttpDownloader::downloadToFile(url, kArticleTmpPath, nullptr, nullptr);
  if (result != HttpDownloader::OK) {
    errorMessage = (result == HttpDownloader::ABORTED) ? "Download cancelled." : "Failed to download article.";
    Storage.remove(kArticleTmpPath);
    return false;
  }

  std::string title;
  const bool parsed = parseAndSaveWikipediaArticle(kArticleTmpPath, title);
  Storage.remove(kArticleTmpPath);

  if (parsed && !title.empty()) {
    currentArticleTitle = title;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------

void WikipediaActivity::loop() {
  if (pendingUpdateSearch) {
    pendingUpdateSearch = false;
    fetchTaskHandle = nullptr;
    if (DownloadWatchdog::gotTimeout) {
      LOG_ERR("WIKI", "Watchdog timeout! Returning home.");
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
    state = WikiState::SearchResults;
    requestUpdate();
    return;
  }

  if (pendingUpdateArticle) {
    pendingUpdateArticle = false;
    fetchTaskHandle = nullptr;
    if (DownloadWatchdog::gotTimeout) {
      LOG_ERR("WIKI", "Watchdog timeout! Returning home.");
      activityManager.goHome();
      return;
    }
    state = WikiState::SearchResults;
    if (!backgroundFetchFailed && errorMessage.empty()) {
      openArticle(currentArticleTitle);
    } else {
      requestUpdate();
    }
    return;
  }

  if (state == WikiState::Loading && millis() - lastSpinnerUpdate >= 600) {
    lastSpinnerUpdate = millis();
    spinnerFrame = (spinnerFrame + 1) % 3;
    requestUpdate();
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (state == WikiState::Loading) {
      cancelFetchTask();
      if (!searchResults.empty()) {
        state = WikiState::SearchResults;
      } else {
        state = WikiState::OfflineList;
        loadOfflineArticlesList();
      }
      requestUpdate();
    } else if (state == WikiState::SearchResults) {
      state = WikiState::OfflineList;
      loadOfflineArticlesList();
      selectedIndex = 0;
      requestUpdate();
    } else {
      finish();
    }
    return;
  }

  if (state == WikiState::OfflineList) {
    const int totalItems = static_cast<int>(offlineArticles.size()) + 1;
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
            std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Search Wikipedia", "", 128),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto text = std::get<KeyboardResult>(result.data).text;
                if (!text.empty()) {
                  searchQuery = text;
                  state = WikiState::Loading;
                  requestUpdate();
                  performBackgroundSearch();
                  return;
                }
              }
              requestUpdate();
            });
      } else {
        openArticle(offlineArticles[selectedIndex - 1]);
      }
    }
    return;
  }

  if (state == WikiState::SearchResults) {
    if (!errorMessage.empty()) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        state = WikiState::Loading;
        errorMessage.clear();
        requestUpdate();
        performBackgroundSearch();
      }
      return;
    }

    if (searchResults.empty()) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        startActivityForResult(
            std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Search Wikipedia", searchQuery, 128),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                const auto text = std::get<KeyboardResult>(result.data).text;
                if (!text.empty()) {
                  searchQuery = text;
                  state = WikiState::Loading;
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
      const std::string title = searchResults[selectedIndex];
      const std::string filepath = articleFilePathFor(title);
      if (Storage.exists(filepath.c_str())) {
        openArticle(title);
      } else {
        articleToFetch = title;
        state = WikiState::Loading;
        requestUpdate();
        performBackgroundFetchArticle();
      }
    }
    return;
  }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void WikipediaActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (state == WikiState::OfflineList) {
    char subtitle[32];
    snprintf(subtitle, sizeof(subtitle), "%d saved article%s", static_cast<int>(offlineArticles.size()),
             offlineArticles.size() == 1 ? "" : "s");
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Wikipedia", subtitle);
  } else {
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Wikipedia");
  }

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentBottom = pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing;
  const int contentHeight = contentBottom - contentTop;

  if (state == WikiState::Loading) {
    const int centerY = contentTop + contentHeight / 2;
    renderer.drawCenteredText(UI_12_FONT_ID, centerY - 30,
                              isSearchTask ? "Searching Wikipedia..." : "Downloading article...", true,
                              EpdFontFamily::BOLD);
    GUI.drawSpinner(renderer, pageWidth / 2, centerY + 20, nullptr, spinnerFrame);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), nullptr, nullptr, nullptr);
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  } else if (state == WikiState::OfflineList) {
    renderOfflineList();

  } else if (state == WikiState::SearchResults) {
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

// Rounded "+ Search Wikipedia" pill above a plain scrollable list of saved
// articles -- same visual language as CalendarActivity's agenda list
// (pill action row + custom-drawn rows, selection recomputed & centered in
// the viewport each frame rather than tracked as persisted scroll state).
void WikipediaActivity::renderOfflineList() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int pad = metrics.contentSidePadding;
  const int listTop = metrics.topPadding + metrics.headerHeight + 6;
  const int listBottom = pageHeight - metrics.buttonHintsHeight;
  const int listH = listBottom - listTop;

  const int pillH = 40;
  const int rowH = 36;
  const int itemCount = 1 + static_cast<int>(offlineArticles.size());

  const int totalHeight = pillH + static_cast<int>(offlineArticles.size()) * rowH;
  const int selectedTop = selectedIndex == 0 ? 0 : pillH + (selectedIndex - 1) * rowH;
  const int selectedHeight = selectedIndex == 0 ? pillH : rowH;

  int scrollTop = 0;
  if (totalHeight > listH) {
    scrollTop = selectedTop + selectedHeight / 2 - listH / 2;
    scrollTop = std::max(0, std::min(scrollTop, totalHeight - listH));
  }

  // Row 0: the pill.
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
      renderer.drawText(UI_10_FONT_ID, pad + 14, pillY + 9, "+ Search Wikipedia", !selected, EpdFontFamily::BOLD);
    }
  }

  for (int i = 0; i < static_cast<int>(offlineArticles.size()); i++) {
    const int y = listTop + pillH + i * rowH - scrollTop;
    if (y + rowH < listTop || y > listBottom) continue;

    const bool selected = (i + 1) == selectedIndex;
    if (selected) renderer.fillRect(0, y, pageWidth, rowH, true);

    const int titleMaxW = pageWidth - pad * 2;
    std::string title = renderer.truncatedText(UI_10_FONT_ID, offlineArticles[i].c_str(), titleMaxW);
    renderer.drawText(UI_10_FONT_ID, pad, y + 8, title.c_str(), !selected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

// Search result rows with a small rounded "Cached" tag on the right for
// articles already saved offline.
void WikipediaActivity::renderSearchResults() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int pad = metrics.contentSidePadding;
  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int listBottom = pageHeight - metrics.buttonHintsHeight;
  const int listH = listBottom - listTop;
  const int rowH = 36;

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

    const bool cached = Storage.exists(articleFilePathFor(searchResults[i]).c_str());
    int titleMaxW = pageWidth - pad * 2;

    if (cached) {
      constexpr int tagW = 62, tagH = 22;
      const int tagX = pageWidth - pad - tagW;
      const int tagY = y + (rowH - tagH) / 2;
      titleMaxW -= tagW + 8;
      if (selected) {
        renderer.drawRoundedRect(tagX, tagY, tagW, tagH, 1, 8, false);
        renderer.drawCenteredText(SMALL_FONT_ID, tagY + 5, "Cached", false);
      } else {
        renderer.fillRoundedRect(tagX, tagY, tagW, tagH, 8, Color::Black);
        renderer.drawText(SMALL_FONT_ID, tagX + 8, tagY + 5, "Cached", false);
      }
    }

    std::string title = renderer.truncatedText(UI_10_FONT_ID, searchResults[i].c_str(), titleMaxW);
    renderer.drawText(UI_10_FONT_ID, pad, y + 8, title.c_str(), !selected);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "^", "v");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
