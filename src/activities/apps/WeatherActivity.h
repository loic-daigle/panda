#pragma once

#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Weather app: fetches current conditions for a user-selected city from the
// open-meteo API (no API key required) and caches the last successful
// fetch to SD so the screen still shows something when offline.
//
// State machine: Init -> SelectCity -> Loading -> ShowWeather. The network
// fetch runs on a background FreeRTOS task (see runBackgroundFetch()) so the
// render/input loop never blocks on WiFi/HTTP; the task hands its result
// back to loop() via the pendingUpdateWeather flag.
class WeatherActivity final : public Activity {
 public:
  explicit WeatherActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Weather", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // Runs on the background fetch task spawned by performFetch(). Public so
  // the free-function task trampoline (in the .cpp) can call it through a
  // raw `this` pointer handed to xTaskCreate.
  void runBackgroundFetch();

 private:
  enum class WeatherState { Init, SelectCity, Loading, ShowWeather };

  WeatherState state = WeatherState::Init;
  ButtonNavigator buttonNavigator;

  int selectedCityIndex = 0;
  bool weatherLoaded = false;
  double temp = 0.0;
  double windspeed = 0.0;
  int weatherCode = 0;
  std::string timeStr;
  std::string cityName;
  std::string errorMessage;

  // Written by the background fetch task, consumed by loop() on the main
  // task once pendingUpdateWeather is observed set. Not synchronized beyond
  // the flag handoff (the fetch task is fully joined/deleted before the next
  // fetch starts), matching the source implementation's approach.
  double fetchedTemp = 0.0;
  double fetchedWindspeed = 0.0;
  int fetchedWeatherCode = 0;
  std::string fetchedTimeStr;
  bool backgroundFetchSuccess = false;
  volatile bool pendingUpdateWeather = false;
  void* fetchTaskHandle = nullptr;

  void performFetch();
  void cancelFetchTask();
};
