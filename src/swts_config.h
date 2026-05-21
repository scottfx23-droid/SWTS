/*
 * SWTS Config — Load device configuration from /SWTS/config.json
 */
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

namespace swts {

struct HomeButton {
    char id[16];
    char label[20];
    char sublabel[24];
    char sublabel_dynamic[16]; // counter name for live updates
};

struct Config {
    // Scenario
    char scenario_name[32]    = "Unknown Scenario";
    char scenario_version[8]  = "1.0";
    char planet[24]           = "Unknown";
    char currency[4]          = "CR";     // "CR" for credits, "RP" for reputation
    char currency_name[16]    = "Credits"; // display name

    // Device
    char device_type[12]      = "datapad";
    int  wifi_scan_interval   = 5000;
    char wifi_scan_prefix[12] = "SWTS_";

    // UI
    HomeButton home_buttons[8];
    int num_home_buttons      = 0;

    // Audio (freq, duration pairs)
    int boot_melody[16];   int boot_melody_len = 0;
    int nfc_tone[8];       int nfc_tone_len = 0;
    int success_tone[8];   int success_tone_len = 0;
    int fail_tone[8];      int fail_tone_len = 0;
    int alert_tone[12];    int alert_tone_len = 0;

    // Buttons
    int btn_red_sw = -1,  btn_red_led = -1;
    int btn_wht_sw = -1,  btn_wht_led = -1;
    int btn_blu_sw = -1,  btn_blu_led = -1;
};

inline Config config;

inline bool loadConfig() {
    File f = LittleFS.open("/SWTS/config.json", "r");
    if (!f) {
        Serial.println("[CFG] No config.json on flash — using defaults");
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, f)) {
        Serial.println("[CFG] Parse error");
        f.close();
        return false;
    }
    f.close();

    // Scenario
    strlcpy(config.scenario_name, doc["scenario"]["name"] | config.scenario_name, sizeof(config.scenario_name));
    strlcpy(config.scenario_version, doc["scenario"]["version"] | config.scenario_version, sizeof(config.scenario_version));
    strlcpy(config.planet, doc["scenario"]["planet"] | config.planet, sizeof(config.planet));
    strlcpy(config.currency, doc["scenario"]["currency"] | config.currency, sizeof(config.currency));
    strlcpy(config.currency_name, doc["scenario"]["currency_name"] | config.currency_name, sizeof(config.currency_name));

    // Device
    strlcpy(config.device_type, doc["device"]["type"] | config.device_type, sizeof(config.device_type));
    config.wifi_scan_interval = doc["device"]["wifi_scan_interval_ms"] | config.wifi_scan_interval;
    strlcpy(config.wifi_scan_prefix, doc["device"]["wifi_scan_prefix"] | config.wifi_scan_prefix, sizeof(config.wifi_scan_prefix));

    // Home buttons
    JsonArray btns = doc["ui"]["home_buttons"].as<JsonArray>();
    config.num_home_buttons = 0;
    for (JsonObject b : btns) {
        if (config.num_home_buttons >= 8) break;
        HomeButton &hb = config.home_buttons[config.num_home_buttons++];
        strlcpy(hb.id, b["id"] | "", sizeof(hb.id));
        strlcpy(hb.label, b["label"] | "", sizeof(hb.label));
        strlcpy(hb.sublabel, b["sublabel"] | "", sizeof(hb.sublabel));
        strlcpy(hb.sublabel_dynamic, b["sublabel_dynamic"] | "", sizeof(hb.sublabel_dynamic));
    }

    // Audio tones
    auto loadTone = [](JsonArray arr, int *out, int &len, int maxLen) {
        len = 0;
        for (JsonVariant v : arr) {
            if (len >= maxLen) break;
            out[len++] = v.as<int>();
        }
    };
    loadTone(doc["audio"]["boot_melody"].as<JsonArray>(), config.boot_melody, config.boot_melody_len, 16);
    loadTone(doc["audio"]["nfc_scan_tone"].as<JsonArray>(), config.nfc_tone, config.nfc_tone_len, 8);
    loadTone(doc["audio"]["success_tone"].as<JsonArray>(), config.success_tone, config.success_tone_len, 8);
    loadTone(doc["audio"]["fail_tone"].as<JsonArray>(), config.fail_tone, config.fail_tone_len, 8);
    loadTone(doc["audio"]["alert_tone"].as<JsonArray>(), config.alert_tone, config.alert_tone_len, 12);

    // Button pins
    config.btn_red_sw  = doc["buttons"]["red"]["switch_pin"] | -1;
    config.btn_red_led = doc["buttons"]["red"]["led_pin"] | -1;
    config.btn_wht_sw  = doc["buttons"]["white"]["switch_pin"] | -1;
    config.btn_wht_led = doc["buttons"]["white"]["led_pin"] | -1;
    config.btn_blu_sw  = doc["buttons"]["blue"]["switch_pin"] | -1;
    config.btn_blu_led = doc["buttons"]["blue"]["led_pin"] | -1;

    Serial.printf("[CFG] Loaded: %s v%s (%d buttons)\n",
                  config.scenario_name, config.scenario_version, config.num_home_buttons);
    return true;
}

} // namespace swts
