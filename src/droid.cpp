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

#define S Serial

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
    S.printf("[CFG] %s id=%s ssid=%s faction=%s\n", cfg.name, cfg.id, cfg.ssid, cfg.faction);
    return true;
}

// ═══════════════════════════════════════
//  HTTP SERVER — datapad calls /api/info on connect, /api/interact for dialogue
// ═══════════════════════════════════════
AsyncWebServer server(80);
char connectedCallsign[16] = "";
bool intelDelivered = false;     // becomes true after a player picks the "deliver_intel" choice

void handleInfo(AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["prop_id"]  = cfg.id;
    doc["name"]     = cfg.name;
    doc["type"]     = "droid";
    doc["faction"]  = cfg.faction;
    JsonObject feat = doc["features"].to<JsonObject>();
    feat["has_nfc"]      = false;
    feat["has_minigame"] = false;
    feat["minigame_type"]= "";
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

    JsonDocument resp;
    resp["type"] = "dialogue";
    JsonObject speaker = resp["speaker"].to<JsonObject>();
    speaker["name"]    = cfg.name;
    speaker["faction"] = cfg.faction;

    JsonArray lines   = resp["lines"].to<JsonArray>();
    JsonArray choices = resp["choices"].to<JsonArray>();

    if (strcmp(action, "deliver_intel") == 0) {
        // ── Player is handing off the decrypted intel — completes the mission ──
        intelDelivered = true;
        astromechChirp();
        delay(120);
        buzzHandoff();

        // Broadcast to GM dashboard + any subscribed comms triggers
        char eventId[40];
        snprintf(eventId, sizeof(eventId), "droid_handoff:%s", cfg.id);
        swts::gmTriggerEvent(eventId, "Intel delivered to R5-D8", /*severity*/ 0, callsign);

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
    astromechChirp();

    JsonObject l1 = lines.add<JsonObject>();
    l1["text"] = "BEEP-BWEEP-WHIRR-CLICK. (Translation: About time you showed up.)";
    l1["style"] = "droid";

    JsonObject l2 = lines.add<JsonObject>();
    if (intelDelivered) {
        l2["text"] = "Coordinates already uploaded. Get to the extraction point.";
    } else {
        l2["text"] = "Did you decrypt the package at the Repair Shop? If yes, hand it off. If not, get there first.";
    }
    l2["style"] = "system";

    if (!intelDelivered) {
        JsonObject c1 = choices.add<JsonObject>();
        c1["label"]       = "Hand off intel";
        c1["next_action"] = "deliver_intel";
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
        loadConfig();
    } else {
        S.println("[SD] No SD card / mount failed — using built-in defaults");
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
