/*
 * SWTS Proximity — RSSI gate for props (panel / droid / terminal)
 *
 * A prop rejects /api/interact requests unless the connected datapad's
 * received signal is strong enough, so hacks can only be started or
 * continued while physically near the prop. Enforcement is server-side
 * (a modified datapad can't bypass it); the datapad mirrors the check
 * for UX only.
 *
 * A prop hosts one interacting datapad at a time, so we read the
 * strongest station in the softAP station list rather than mapping the
 * HTTP client's IP to a MAC (best-effort, and correct in the one-player
 * session model).
 *
 * Hysteresis: the gate opens at `rssiStart` and only closes when the
 * signal falls below `rssiDrop` (default 12 dB lower), so a player at
 * the boundary doesn't flap.
 *
 * Rough indoor calibration on ESP32-S3: -45 dBm ~ 1 m, -60 ~ 3 m,
 * -70+ ~ across the room. Bodies swing readings by +/-10 dB — tune
 * per prop in config.json.
 */
#pragma once
#include <Arduino.h>
#include <esp_wifi.h>

namespace swts_prox {

inline bool enabled   = false;
inline int  rssiStart = -60;
inline int  rssiDrop  = -72;
inline int  graceMs   = 2000;
inline bool gateOpen  = false;

// Parse behavior.* keys (call from the prop's loadConfig with the doc)
template <typename TDoc>
inline void loadConfig(TDoc &doc) {
    enabled   = doc["behavior"]["proximity_enabled"]    | enabled;
    rssiStart = doc["behavior"]["proximity_rssi_start"] | rssiStart;
    rssiDrop  = doc["behavior"]["proximity_rssi_drop"]  | rssiDrop;
    graceMs   = doc["behavior"]["proximity_grace_ms"]   | graceMs;
    int txDbm = doc["behavior"]["ap_tx_power_dbm"]      | 0;
    if (txDbm > 0) esp_wifi_set_max_tx_power((int8_t)(txDbm * 4));  // units of 0.25 dBm
    if (enabled)
        Serial.printf("[PROX] gate on: start %d dBm, drop %d dBm\n", rssiStart, rssiDrop);
}

// Strongest connected station's RSSI; -127 when nobody is connected
inline int bestStationRssi() {
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK || list.num == 0) return -127;
    int best = -127;
    for (int i = 0; i < list.num; i++)
        if (list.sta[i].rssi > best) best = list.sta[i].rssi;
    return best;
}

// True when the requester is close enough (with hysteresis)
inline bool check(int *outRssi = nullptr) {
    if (!enabled) { if (outRssi) *outRssi = 0; return true; }
    int r = bestStationRssi();
    if (outRssi) *outRssi = r;
    if (!gateOpen) { if (r >= rssiStart) gateOpen = true; }
    else if (r < rssiDrop) gateOpen = false;
    return gateOpen;
}

} // namespace swts_prox
