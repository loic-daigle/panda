#include "BleKeyboardPairingActivity.h"

#include <BleKeyboardHost.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/RadioManager.h"

namespace {
constexpr uint32_t SCAN_DURATION_MS = 6000;
}

void BleKeyboardPairingActivity::onEnter() {
  Activity::onEnter();
  RADIO.ensureBleHidHost();
  entries.clear();
  selectedIndex = 0;
  connectingName.clear();
  errorMessage.clear();
  passkey = 0;
  startScan();
}

void BleKeyboardPairingActivity::onExit() {
  BleHid.stopScan();
  RADIO.shutdown();
  Activity::onExit();
}

void BleKeyboardPairingActivity::startScan() {
  rebuildEntries();  // show paired devices immediately, scan fills in the rest
  state = State::SCANNING;
  scanning = true;
  BleHid.startScan(SCAN_DURATION_MS);
  requestUpdate();
}

void BleKeyboardPairingActivity::rebuildEntries() {
  entries.clear();

  for (uint8_t i = 0; i < BleHid.pairedCount(); i++) {
    const auto& p = BleHid.paired(i);
    Entry e;
    e.name = p.name[0] ? p.name : p.addr;
    e.addr = p.addr;
    e.paired = true;
    entries.push_back(std::move(e));
  }

  for (uint8_t i = 0; i < BleHid.deviceCount(); i++) {
    const auto& d = BleHid.device(i);
    if (!d.hid) continue;  // only show devices advertising the HID service
    const bool alreadyPaired =
        std::any_of(entries.begin(), entries.end(), [&](const Entry& e) { return e.addr == d.addr; });
    if (alreadyPaired) continue;

    Entry e;
    e.name = d.hasName ? d.name : d.addr;
    e.addr = d.addr;
    e.rssi = d.rssi;
    e.paired = false;
    entries.push_back(std::move(e));
  }

  std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    if (a.paired != b.paired) return a.paired;
    return a.rssi > b.rssi;
  });

  if (selectedIndex >= entries.size()) {
    selectedIndex = entries.empty() ? 0 : entries.size() - 1;
  }
}

void BleKeyboardPairingActivity::connectToSelected() {
  if (selectedIndex >= entries.size()) return;
  const auto& entry = entries[selectedIndex];
  connectingName = entry.name;
  if (!BleHid.connect(entry.addr.c_str())) {
    errorMessage = tr(STR_ERROR_GENERAL_FAILURE);
    state = State::ERROR;
    requestUpdate();
    return;
  }
  state = State::CONNECTING;
  requestUpdate();
}

void BleKeyboardPairingActivity::loop() {
  // RADIO.pollBleHidHost() drains BleHid.poll() (which refreshes
  // isScanning()/isConnecting()/isConnected() from the live NimBLE state and
  // drives auto-reconnect -- nothing else in this activity pumps it, so every
  // state below would see stale, never-updated flags without this, most
  // visibly the SCANNING state never noticing the scan actually finished) and
  // also holds a HalPowerManager::Lock for as long as a peripheral stays
  // connected, so idle CPU-frequency scaling can't desync the connection.
  RADIO.pollBleHidHost();

  if (state == State::SCANNING) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      BleHid.stopScan();
      finish();
      return;
    }
    if (BleHid.isScanning()) {
      return;
    }
    scanning = false;
    rebuildEntries();
    state = State::LIST;
    requestUpdate();
    return;
  }

  if (state == State::CONNECTING) {
    char failReason[48];
    if (BleHid.takeConnectFailure(failReason, sizeof(failReason))) {
      errorMessage = failReason;
      state = State::ERROR;
      requestUpdate();
      return;
    }
    uint32_t pk = 0;
    if (BleHid.takePairingPasskey(pk)) {
      passkey = pk;
      state = State::PASSKEY;
      requestUpdate();
      return;
    }
    if (BleHid.isConnected()) {
      rebuildEntries();
      state = State::CONNECTED;
      requestUpdate();
      return;
    }
    if (!BleHid.isConnecting()) {
      // Link dropped before it ever reached isConnected() (or isConnecting()
      // was already false when this state was entered) and no explicit
      // failure/passkey event arrived to explain it.
      errorMessage = tr(STR_ERROR_GENERAL_FAILURE);
      state = State::ERROR;
      requestUpdate();
    }
    return;
  }

  if (state == State::PASSKEY) {
    char failReason[48];
    if (BleHid.takeConnectFailure(failReason, sizeof(failReason))) {
      errorMessage = failReason;
      state = State::ERROR;
      requestUpdate();
      return;
    }
    if (BleHid.isConnected()) {
      rebuildEntries();
      state = State::CONNECTED;
      requestUpdate();
      return;
    }
    if (!BleHid.isConnecting()) {
      errorMessage = tr(STR_ERROR_GENERAL_FAILURE);
      state = State::ERROR;
      requestUpdate();
    }
    return;
  }

  if (state == State::CONNECTED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      finish();
    }
    return;
  }

  if (state == State::ERROR) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      startScan();
    }
    return;
  }

  if (state == State::FORGET_PROMPT) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Left) ||
        mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      if (forgetPromptSelection > 0) {
        forgetPromptSelection--;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Right) ||
               mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      if (forgetPromptSelection < 1) {
        forgetPromptSelection++;
        requestUpdate();
      }
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (forgetPromptSelection == 1) {
        BleHid.forget(forgetAddr.c_str());
      }
      rebuildEntries();
      state = State::LIST;
      requestUpdate();
    } else if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      state = State::LIST;
      requestUpdate();
    }
    return;
  }

  // State::LIST
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (!entries.empty()) {
      connectToSelected();
    } else {
      startScan();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    startScan();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    if (!entries.empty() && entries[selectedIndex].paired) {
      forgetAddr = entries[selectedIndex].addr;
      forgetName = entries[selectedIndex].name;
      forgetPromptSelection = 0;
      state = State::FORGET_PROMPT;
      requestUpdate();
      return;
    }
  }

  if (!entries.empty()) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    const int contentTop =
        screen.y + metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
    const int contentHeight = screen.height - contentTop - metrics.verticalSpacing * 2;
    int touchSel = static_cast<int>(selectedIndex);
    const auto listTouch =
        handleListTouch(touchSel, static_cast<int>(entries.size()), contentTop, contentHeight, false);
    if (listTouch != ListTouchResult::None) {
      selectedIndex = static_cast<size_t>(touchSel);
      if (listTouch == ListTouchResult::Activated) connectToSelected();
      return;
    }

    const int pageItems = GUI.getListPageItems(contentHeight, false);
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up) {
      selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, entries.size(), pageItems);
      requestUpdate();
      return;
    }
    if (swipe == MappedInputManager::SwipeDir::Down) {
      selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, entries.size(), pageItems);
      requestUpdate();
      return;
    }
  }

  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, entries.size());
    requestUpdate();
  });
  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, entries.size());
    requestUpdate();
  });
}

void BleKeyboardPairingActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto& theme = UITheme::getInstance();
  auto metrics = theme.getMetrics();
  Rect screen = theme.getScreenSafeArea(renderer, true, false);

  char countStr[48];
  snprintf(countStr, sizeof(countStr), tr(STR_BLE_DEVICES_FOUND), entries.size());
  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_BLUETOOTH_KEYBOARD), (state == State::LIST || state == State::SCANNING) ? countStr : "");

  switch (state) {
    case State::SCANNING:
      renderConnecting(&screen, &metrics);
      break;
    case State::LIST:
      renderList(&screen, &metrics);
      break;
    case State::CONNECTING:
      renderConnecting(&screen, &metrics);
      break;
    case State::PASSKEY:
      renderPasskey(&screen, &metrics);
      break;
    case State::CONNECTED:
      renderConnected(&screen, &metrics);
      break;
    case State::ERROR:
      renderError(&screen, &metrics);
      break;
    case State::FORGET_PROMPT:
      renderForgetPrompt(&screen, &metrics);
      break;
  }

  renderer.displayBuffer();
}

void BleKeyboardPairingActivity::renderList(const Rect* screen, const ThemeMetrics* metrics) const {
  if (entries.empty()) {
    const auto height = renderer.getLineHeight(UI_10_FONT_ID);
    const auto top = screen->y + (screen->height - height) / 2;
    UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top, tr(STR_NO_BLE_DEVICES));
    UITheme::drawCenteredText(renderer, *screen, SMALL_FONT_ID, top + height + 10, tr(STR_PRESS_OK_SCAN));
  } else {
    const int contentTop =
        screen->y + metrics->topPadding + metrics->headerHeight + metrics->tabBarHeight + metrics->verticalSpacing;
    const int contentHeight = screen->height - contentTop - metrics->verticalSpacing * 2;
    GUI.drawList(
        renderer, Rect{screen->x, contentTop, screen->width, contentHeight}, static_cast<int>(entries.size()),
        selectedIndex, [this](int index) { return entries[index].name; }, nullptr, nullptr,
        [this](int index) { return std::string(entries[index].paired ? "+ " : ""); });
  }

  const bool canForget = !entries.empty() && entries[selectedIndex].paired;
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONNECT), canForget ? tr(STR_FORGET_BUTTON) : "", tr(STR_RETRY));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BleKeyboardPairingActivity::renderConnecting(const Rect* screen, const ThemeMetrics* metrics) const {
  constexpr int MAX_STATUS_LINES = 2;
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height) / 2;
  const int statusX = screen->x + metrics->contentSidePadding;
  const int statusWidth = screen->width - metrics->contentSidePadding * 2;

  if (state == State::SCANNING) {
    const Rect statusBounds{statusX, screen->y, statusWidth, screen->height};
    UITheme::drawCenteredWrappedText(renderer, statusBounds, UI_10_FONT_ID, tr(STR_SCANNING), MAX_STATUS_LINES);
  } else {
    const Rect statusBounds{statusX, screen->y, statusWidth, top - metrics->verticalSpacing - screen->y};
    UITheme::drawCenteredWrappedText(renderer, statusBounds, UI_12_FONT_ID, tr(STR_CONNECTING), MAX_STATUS_LINES, true,
                                     EpdFontFamily::BOLD, UITheme::TextVerticalAlignment::BOTTOM);
    std::string nameInfo = std::string(tr(STR_TO_PREFIX)) + connectingName;
    UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top, nameInfo.c_str());
  }

  const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BleKeyboardPairingActivity::renderPasskey(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height * 3) / 2;

  UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top - 20, tr(STR_CONFIRM_PASSKEY), true,
                            EpdFontFamily::BOLD);

  char passkeyStr[16];
  snprintf(passkeyStr, sizeof(passkeyStr), "%06u", static_cast<unsigned>(passkey));
  UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top + 30, passkeyStr);

  const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BleKeyboardPairingActivity::renderConnected(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height * 2) / 2;

  UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top - 20, tr(STR_CONNECTED), true, EpdFontFamily::BOLD);
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 20, connectingName.c_str());

  const auto labels = mappedInput.mapLabels("", tr(STR_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BleKeyboardPairingActivity::renderError(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height * 2) / 2;

  UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top - 20, tr(STR_CONNECTION_FAILED), true,
                            EpdFontFamily::BOLD);
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 20, errorMessage.c_str());

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DONE), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BleKeyboardPairingActivity::renderForgetPrompt(const Rect* screen, const ThemeMetrics* metrics) const {
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = screen->y + (screen->height - height * 3) / 2;

  UITheme::drawCenteredText(renderer, *screen, UI_12_FONT_ID, top - 40, tr(STR_FORGET_DEVICE), true,
                            EpdFontFamily::BOLD);
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top, forgetName.c_str());
  UITheme::drawCenteredText(renderer, *screen, UI_10_FONT_ID, top + 40, tr(STR_FORGET_AND_REMOVE));

  const int buttonY = top + 80;
  constexpr int buttonWidth = 120;
  constexpr int buttonSpacing = 30;
  constexpr int totalWidth = buttonWidth * 2 + buttonSpacing;
  const int startX = screen->x + (screen->width - totalWidth) / 2;

  if (forgetPromptSelection == 0) {
    std::string text = "[" + std::string(tr(STR_CANCEL)) + "]";
    renderer.drawText(UI_10_FONT_ID, startX, buttonY, text.c_str());
  } else {
    renderer.drawText(UI_10_FONT_ID, startX + 4, buttonY, tr(STR_CANCEL));
  }

  if (forgetPromptSelection == 1) {
    std::string text = "[" + std::string(tr(STR_FORGET_BUTTON)) + "]";
    renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing, buttonY, text.c_str());
  } else {
    renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing + 4, buttonY, tr(STR_FORGET_BUTTON));
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
