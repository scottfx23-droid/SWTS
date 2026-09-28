/*
 * SWTS Droid — Headless astromech prop
 * ESP32-S3 (same hardware family as the datapad, no display, no NFC)
 *
 * Hardware:
 *   SD card:    GPIO 15 = CS, 16 = MOSI, 17 = CLK, 18 = MISO  (HSPI)
 *   Buzzer:     GPIO  4
 *   (Optional) Eye LED on GPIO 5 — driven HIGH = on
 *
 * Behavior:
 *   - WiFi AP "SWTS_DROID_<ID>" on the mesh channel
 *   - HTTP /api/info  → identity for the datapad's prop screen
 *   - HTTP /api/interact → R5 dialogue with a "deliver_intel" action
 *   - Periodic R5 chatter (random astromech beeps) every 30-60 s
 *   - Broadcast MSG_EVENT "droid_handoff:<DROID_ID>" when player delivers intel
 *     so the datapad's comms.json / mission system can react
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include "swts_mesh.h"
#include "swts_lights.h"         // addressable RGB strip (/SWTS/lights.txt)
#include "swts_test_configs.h"   // boot-time test provisioning (see header to disable)

#define S Serial

// Light feedback queued from async HTTP handlers, applied in loop().
// (Sounds stay inline in the handlers — the astromech chirps predate this.)
enum PropFx : uint8_t { FX_NONE = 0, FX_ACTIVITY, FX_SUCCESS, FX_FAIL, FX_SIGNAL };
volatile uint8_t pendingFx = FX_NONE;

// ═══════════════════════════════════════
//  PINS
// ═══════════════════════════════════════
#define SD_CS      15
#define SD_MOSI    16
#define SD_CLK     17
#define SD_MISO    18
#define BUZZER_PIN  4
#define EYE_LED    5     // optional indicator; harmless if no LED wired

// ═══════════════════════════════════════
//  BUZZER — R5-style chatter
// ═══════════════════════════════════════
inline void buzzerTone(int freq, int ms) { tone(BUZZER_PIN, freq, ms); }

// A short random astromech chirp — three quick notes in the high range
void astromechChirp() {
    int base = 1500 + random(0, 1500);
    buzzerTone(base, 60);
    delay(70);
    buzzerTone(base + random(-300, 600), 50);
    delay(60);
    buzzerTone(base + random(-500, 800), 70);
}

void buzzGreeting() {
    // Rising "hi there"
    buzzerTone(1800, 80); delay(90);
    buzzerTone(2400, 80); delay(90);
    buzzerTone(2800, 120);
}

void buzzHandoff() {
    // Satisfied "got it" — descending three
    buzzerTone(2800, 100); delay(110);
    buzzerTone(2200, 100); delay(110);
    buzzerTone(1800, 200);
}

// ═══════════════════════════════════════
//  DROID CONFIG (loaded from /SWTS/config.json)
// ═══════════════════════════════════════
struct DroidConfig {
    char id[16]         = "DROID_R5D8";
    char name[40]       = "R5-D8";
    char ssid[24]       = "SWTS_DROID_R5";
    char faction[12]    = "rebel";
    bool has_minigame   = true;    // card-less default: core must be purged
    char minigame_type[16] = "purge";
    int  purge_targets  = 12;   // corrupted blocks to clear
    int  purge_time_s   = 35;   // time limit in seconds
};
DroidConfig cfg;

bool loadConfig() {
    File f = SD.open("/SWTS/config.json", FILE_READ);
    if (!f) { S.println("[CFG] no /SWTS/config.json, using defaults"); return false; }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) { S.printf("[CFG] parse error: %s\n", err.c_str()); return false; }

    strlcpy(cfg.id,      doc["prop"]["id"]      | cfg.id,      sizeof(cfg.id));
    strlcpy(cfg.name,    doc["prop"]["name"]    | cfg.name,    sizeof(cfg.name));
    strlcpy(cfg.ssid,    doc["prop"]["ssid"]    | cfg.ssid,    sizeof(cfg.ssid));
    strlcpy(cfg.faction, doc["prop"]["faction"] | cfg.faction, sizeof(cfg.faction));

    // NOTE: as<const char*>(), not `| nullptr` — the latter always yields null
    const char* mg = doc["behavior"]["minigame_default"].as<const char*>();
    cfg.has_minigame = (mg != nullptr && mg[0] != '\0');
    if (cfg.has_minigame) strlcpy(cfg.minigame_type, mg, sizeof(cfg.minigame_type));
    cfg.purge_targets = doc["behavior"]["purge_targets"] | cfg.purge_targets;
    cfg.purge_time_s  = doc["behavior"]["purge_time_s"]  | cfg.purge_time_s;

    S.printf("[CFG] %s id=%s ssid=%s faction=%s minigame=%s\n",
             cfg.name, cfg.id, cfg.ssid, cfg.faction,
             cfg.has_minigame ? cfg.minigame_type : "none");
    return true;
}

// ═══════════════════════════════════════
//  HTTP SERVER — datapad calls /api/info on connect, /api/interact for dialogue
// ═══════════════════════════════════════
AsyncWebServer server(80);
char connectedCallsign[16] = "";
bool intelDelivered = false;     // becomes true after a player picks the "deliver_intel" choice
bool coreSliced = false;         // per-connection: has this player purged the memory core?

void handleInfo(AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["prop_id"]  = cfg.id;
    doc["name"]     = cfg.name;
    doc["type"]     = "droid";
    doc["faction"]  = cfg.faction;
    JsonObject feat = doc["features"].to<JsonObject>();
    feat["has_nfc"]      = false;
    feat["has_minigame"] = cfg.has_minigame;
    feat["minigame_type"]= cfg.has_minigame ? cfg.minigame_type : "";
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
}

void handleInteract(AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
    JsonDocument reqDoc;
    if (deserializeJson(reqDoc, data, len)) {
        req->send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }
    const char *action   = reqDoc["action"]            | "greet";
    const char *callsign = reqDoc["player"]["callsign"]| "UNKNOWN";
    strlcpy(connectedCallsign, callsign, sizeof(connectedCallsign));
    S.printf("[HTTP] %s by %s\n", action, callsign);

    if (strcmp(action, "start_purge") == 0) {
        // ── Core Purge minigame start — datapad runs the game on its screen ──
        // Faction relation tunes it: this droid's own faction gets a gentler
        // purge (his core trusts them), the enemy gets more corruption and
        // less time.
        const char *pfac = reqDoc["player"]["faction"] | "";
        int targets = cfg.purge_targets;
        int timeS   = cfg.purge_time_s;
        if (pfac[0] && cfg.faction[0] && strcasecmp(cfg.faction, "neutral") != 0) {
            if (strcasecmp(cfg.faction, pfac) == 0) { targets -= 4; timeS += 10; }
            else                                    { targets += 4; timeS -= 5;  }
        }
        if (targets < 4)  targets = 4;
        if (timeS < 10)   timeS = 10;

        pendingFx = FX_ACTIVITY;
        astromechChirp();
        JsonDocument mg;
        mg["type"]       = "minigame_start";
        mg["game"]       = "purge";
        mg["targets"]    = targets;
        mg["time_limit"] = timeS;
        String out;
        serializeJson(mg, out);
        req->send(200, "application/json", out);
        return;
    }

    if (strcmp(action, "minigame_result") == 0) {
        bool won = reqDoc["won"] | false;
        JsonDocument mr;
        mr["type"]     = "minigame_result";
        mr["accepted"] = true;
        if (won) {
            coreSliced = true;
            pendingFx = FX_SUCCESS;
            buzzHandoff();
            mr["message"]  = "MEMORY CORE STABILIZED -- UPLOAD CHANNEL OPEN";
            mr["unlocked"] = true;
            char eventId[40];
            snprintf(eventId, sizeof(eventId), "droid_slice:%s", cfg.id);
            swts::gmTriggerEvent(eventId, "Memory core sliced on R5-D8", /*severity*/ 0, callsign);
        } else {
            pendingFx = FX_FAIL;
            buzzerTone(300, 250);
            mr["message"] = "PURGE FAILED -- CORE STILL SCRAMBLED";
            mr["retry"]   = true;
        }
        String out;
        serializeJson(mr, out);
        req->send(200, "application/json", out);
        return;
    }

    JsonDocument resp;
    resp["type"] = "dialogue";
    JsonObject speaker = resp["speaker"].to<JsonObject>();
    speaker["name"]    = cfg.name;
    speaker["faction"] = cfg.faction;

    JsonArray lines   = resp["lines"].to<JsonArray>();
    JsonArray choices = resp["choices"].to<JsonArray>();

    if (strcmp(action, "deliver_intel") == 0 && cfg.has_minigame && !coreSliced) {
        // ── Handoff attempted before the core was sliced — refuse ──
        JsonObject l1 = lines.add<JsonObject>();
        l1["text"] = "BZZT-BZZT-CLUNK. (Translation: Can't accept the upload — my memory core is still scrambled.)";
        l1["style"] = "droid";
        JsonObject c1 = choices.add<JsonObject>();
        c1["label"]       = "Purge memory core";
        c1["next_action"] = "start_purge";
        JsonObject c2 = choices.add<JsonObject>();
        c2["label"]       = "[Disconnect]";
        c2["next_action"] = nullptr;
        String out;
        serializeJson(resp, out);
        req->send(200, "application/json", out);
        return;
    }

    if (strcmp(action, "deliver_intel") == 0) {
        // ── Player is handing off the decrypted intel — completes the mission ──
        intelDelivered = true;
        pendingFx = FX_SIGNAL;
        astromechChirp();
        delay(120);
        buzzHandoff();

        // Broadcast to GM dashboard + any subscribed comms triggers
        char eventId[40];
        snprintf(eventId, sizeof(eventId), "droid_handoff:%s", cfg.id);
        swts::gmTriggerEvent(eventId, "Intel delivered to R5-D8", /*severity*/ 0, callsign);
        // Mission hook — carried in the HTTP response so the delivering
        // datapad advances even if it misses the mesh broadcast
        resp["game_event"] = eventId;

        // Push a comm to the player so they get visual confirmation
        char commId[20];
        snprintf(commId, sizeof(commId), "r5_thanks_%lu", millis() % 100000);
        swts::gmPushComm(commId, callsign, cfg.name, "INTEL RECEIVED",
                         "Coordinates logged. Transmitting to Alliance Command. Excellent work, operative.");

        JsonObject l1 = lines.add<JsonObject>();
        l1["text"] = "WHEEEEEEE-WHOOP! (Translation: Got it. Coordinates uploaded.)";
        l1["style"] = "droid";

        JsonObject l2 = lines.add<JsonObject>();
        l2["text"] = "Mission complete, operative. Stay safe out there.";
        l2["style"] = "system";

        JsonObject c1 = choices.add<JsonObject>();
        c1["label"]       = "[Disconnect]";
        c1["next_action"] = nullptr;

        String out;
        serializeJson(resp, out);
        req->send(200, "application/json", out);
        return;
    }

    // Default — greet
    pendingFx = FX_ACTIVITY;
    astromechChirp();

    JsonObject l1 = lines.add<JsonObject>();
    l1["text"] = "BEEP-BWEEP-WHIRR-CLICK. (Translation: About time you showed up.)";
    l1["style"] = "droid";

    JsonObject l2 = lines.add<JsonObject>();
    if (intelDelivered) {
        l2["text"] = "Coordinates already uploaded. Get to the extraction point.";
    } else if (cfg.has_minigame && !coreSliced) {
        l2["text"] = "Imperial ice scrambled my memory core -- I can't accept any upload like this. Purge the corrupted blocks and we're in business.";
    } else {
        l2["text"] = "Did you decrypt the package at the Repair Shop? If yes, hand it off. If not, get there first.";
    }
    l2["style"] = "system";

    if (!intelDelivered) {
        if (cfg.has_minigame && !coreSliced) {
            JsonObject c1 = choices.add<JsonObject>();
            c1["label"]       = "Purge memory core";
            c1["next_action"] = "start_purge";
        } else {
            JsonObject c1 = choices.add<JsonObject>();
            c1["label"]       = "Hand off intel";
            c1["next_action"] = "deliver_intel";
        }
    }
    JsonObject c2 = choices.add<JsonObject>();
    c2["label"]       = "[Disconnect]";
    c2["next_action"] = nullptr;

    String out;
    serializeJson(resp, out);
    req->send(200, "application/json", out);
}

void startHttpServer() {
    server.on("/api/info", HTTP_GET, handleInfo);
    server.on("/api/interact", HTTP_POST,
        [](AsyncWebServerRequest *r){},
        nullptr,
        handleInteract);
    server.begin();
    S.println("[HTTP] server up on :80");
}

// ═══════════════════════════════════════
//  MESH HANDLER
// ═══════════════════════════════════════
void onMeshMsg(const swts::MeshHeader *hdr, const uint8_t *payload, int len) {
    if (hdr->type == swts::MSG_SYNC_REQUEST) {
        swts::sendPing(millis() / 1000);
        return;
    }
    if (hdr->type == swts::MSG_RESET) {
        S.println("[MESH] RESET — restarting");
        delay(300);
        ESP.restart();
    }
}

// ═══════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════
SPIClass sdSPI(HSPI);

void setup() {
    S.begin(115200);
    delay(200);
    S.println("\n================================");
    S.println("  SWTS DROID — starting");
    S.println("================================");

    randomSeed(esp_random());

    pinMode(BUZZER_PIN, OUTPUT);
    pinMode(EYE_LED, OUTPUT);
    digitalWrite(EYE_LED, HIGH);   // proof of life

    // Boot beep — two rising tones (only audible if buzzer is wired on GPIO 4)
    buzzerTone(1800, 80);
    delay(100);
    buzzerTone(2400, 80);

    // WiFi AP + STA so the player datapad can connect AND ESPNOW works
    S.println("[BOOT] WiFi mode -> AP_STA");
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED ||
            event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
            coreSliced = false;   // each new session must slice the core again
        }
    });
    WiFi.mode(WIFI_AP_STA);
    delay(100);
    S.println("[BOOT] softAP start...");
    bool apOk = WiFi.softAP(cfg.ssid, NULL, MESH_CHANNEL);
    S.printf("[WIFI] AP '%s' ch=%d %s, IP=%s\n",
             cfg.ssid, MESH_CHANNEL, apOk ? "OK" : "FAILED",
             WiFi.softAPIP().toString().c_str());

    // SD optional; falls back to defaults if no card
    sdSPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
    bool sdOk = SD.begin(SD_CS, sdSPI, 4000000);
    if (sdOk) {
        S.printf("[SD] Mounted %lluMB\n", SD.cardSize() / (1024 * 1024));
        // TESTING: write embedded gameplay configs to the card (no-op when
        // SWTS_WRITE_TEST_CONFIGS is commented out in swts_test_configs.h)
        swts_test::writeTestConfigs(SD);
        loadConfig();
        swts_lights::load(SD);   // RGB strip pattern (/SWTS/lights.txt)
    } else {
        S.println("[SD] No SD card / mount failed — insert configured card");
        SD.end();
        sdSPI.end();
    }

    startHttpServer();

    S.println("[BOOT] mesh init...");
    swts::meshInit(cfg.id, swts::ROLE_PANEL, onMeshMsg);
    S.println("[DROID] Ready — waiting for operatives");

    // Boot chirp — three quick rising tones
    buzzGreeting();
    digitalWrite(EYE_LED, HIGH);

    S.println("[DROID] Ready — waiting for operatives");
}

// ═══════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════
void loop() {
    // Lights: idle pattern + any interaction effect queued by HTTP handlers
    if (pendingFx != FX_NONE) {
        uint8_t fx = pendingFx;
        pendingFx = FX_NONE;
        switch (fx) {
        case FX_ACTIVITY: swts_lights::effectActivity(); break;
        case FX_SUCCESS:  swts_lights::effectSuccess();  break;
        case FX_FAIL:     swts_lights::effectFail();     break;
        case FX_SIGNAL:   swts_lights::effectSignal();   break;
        }
    }
    swts_lights::update();

    // Random idle chatter every 30-60s
    static unsigned long nextChirp = 0;
    if (millis() > nextChirp) {
        astromechChirp();
        nextChirp = millis() + 30000 + random(0, 30000);
    }

    // Mesh heartbeat every 5s
    static unsigned long lastBeat = 0;
    if (millis() - lastBeat > 5000) {
        lastBeat = millis();
        swts::sendPing(millis() / 1000);
        S.printf("[MESH] ping seq sent (uptime %lus)\n", millis() / 1000);
    }

    delay(50);
}
