#pragma once
// Wi-Fi networks nearby, for the panel's network list. The device only looks when told to.
#include <cJSON.h>

// Starts a scan in the background unless one is running already. Empty if one is now running, else why not.
const char* wifi_scan_start();
// {"scanning", "networks": [{"ssid", "rssi", "secure"}]}: those of the last finished scan.
// The caller owns the object.
cJSON* wifi_scan_json();
