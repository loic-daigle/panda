#pragma once

// Background timeout guard for network fetch tasks: if a download hasn't
// completed within the timeout, force-disconnects WiFi so the blocking
// HTTP call in the fetch task unblocks instead of hanging forever.
namespace DownloadWatchdog {
extern bool gotTimeout;
void start(unsigned long timeoutMs = 15000);
void stop();
}  // namespace DownloadWatchdog
