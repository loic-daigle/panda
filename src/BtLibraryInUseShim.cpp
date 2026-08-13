// Weak fallback definition of `_btLibraryInUse` for the BLE-controller-only build.
//
// The arduino-esp32 core header cores/esp32/esp32-hal-bt-mem.h emits a global
// constructor (`_setBtLibraryInUse`) that references `_btLibraryInUse` whenever BLE
// hardware is present, but the core only DEFINES that symbol (in esp32-hal-bt.c)
// when an IDF host stack -- CONFIG_BLUEDROID_ENABLED or CONFIG_NIMBLE_ENABLED -- is
// enabled. panda runs the ESP-IDF BLE *controller* only, with NimBLE-Arduino
// supplying the host for both the peripheral-mode offense tools and the BLE HID
// host (CONFIG_BT_NIMBLE_ENABLED=n / CONFIG_BT_CONTROLLER_ONLY=y in
// platformio.ini), so the core never defines it and both the core lib and
// NimBLEDevice.cpp.o fail the final firmware link with "undefined reference to
// `_btLibraryInUse'" (same failure crosspoint-reader's upstream feat-bluetooth
// branch hit and fixed the same way -- see its commit bcb8a74f).
//
// This weak definition lives in our own always-linked source and resolves the
// reference in firmware.elf regardless of the framework header's state. It is
// `weak`, so if a host stack is ever enabled (giving the core its own strong
// definition), that strong symbol wins with no clash.
extern "C" __attribute__((weak)) bool _btLibraryInUse = false;
