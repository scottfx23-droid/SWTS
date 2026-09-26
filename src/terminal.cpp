/*
 * SWTS Terminal — Headless NFC interaction kiosk
 * ESP32-S3 (same hardware family as the datapad, no display)
 *
 * Hardware:
 *   PN532 NFC:  GPIO 14 = SDA, GPIO 13 = SCL   (I2C bus 1)
 *   SD card:    GPIO 15 = CS, 16 = MOSI, 17 = CLK, 18 = MISO  (HSPI)
 *   Buzzer:     GPIO  4
 *
 * Boot flow:
 *   - Read /SWTS/config.json from SD (terminal id, name, accept/deny behavior)
 *   - Start WiFi AP on the mesh channel so players can connect their datapad
 *   - Start ESPNOW mesh
 *   - Continuously scan for NFC datacards
 *
 * On a successful datacard scan:
 *   - Play "access granted" buzzer pattern
 *   - Broadcast MSG_NFC_SCAN to GM (logged in dashboard)
 *   - Broadcast MSG_EVENT keyed to "nfc:<TERMINAL_ID>:<CARD_TEXT>"
 *     (player datapads use this to fire comms / advance missions)
 *   - If a player is connected via WiFi, push a targeted MSG_COMM
 *
 * On a cargo crate scan (NDEF starts with CARGO_):
 *   - Play "access denied" buzzer pattern
 *   - Do not broadcast (wrong reader; cargo goes to player's CARGO INTEL screen)
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <Adafruit_PN532.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include "swts_mesh.h"
#include "swts_test_configs.h"   // boot-time test provisioning (see header to disable)

#define S Serial

// ═══════════════════════════════════════
//  PINS
// ═══════════════════════════════════════
#define NFC_SDA    14
#define NFC_SCL    13

#define SD_CS      15
#define SD_MOSI    16
#define SD_CLK     17
#define SD_MISO    18

#define BUZZER_PIN  4

// ═══════════════════════════════════════
//  BUZZER PATTERNS
// ═══════════════════════════════════════
inline void buzzerTone(int freq, int ms) { tone(BUZZER_PIN, freq, ms); }

void buzzGranted() {
    buzzerTone(880, 80);
    delay(90);
    buzzerTone(1320, 100);
}
void buzzDenied() {
    buzzerTone(300, 150);
    delay(160);
    buzzerTone(200, 200);
}
void buzzScanIdle() {
    buzzerTone(2000, 15);   // quiet tick, useful for testing presence
}

// ═══════════════════════════════════════
//  TERMINAL CONFIG (loaded from /SWTS/config.json)
// ═══════════════════════════════════════
struct TerminalConfig {
    char id[16]            = "TERM_01";
    char name[40]          = "Imperial Terminal";
    char ssid[24]          = "SWTS_TERM_01";
    char faction[12]       = "empire";   // "empire" or "rebel" — for display
    int  cooldown_ms       = 1500;        // ignore the same card within this window
};
TerminalConfig cfg;

bool loadConfig() {
    File f = SD.open("/SWTS/config.json", FILE_READ);
    if (!f) { S.println("[CFG] /SWTS/config.json not found, using defaults"); return false; }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) { S.printf("[CFG] parse error: %s\n", err.c_str()); return false; }

    strlcpy(cfg.id,      doc["prop"]["id"]              | cfg.id,      sizeof(cfg.id));
    strlcpy(cfg.name,    doc["prop"]["name"]            | cfg.name,    sizeof(cfg.name));
    strlcpy(cfg.ssid,    doc["prop"]["ssid"]            | cfg.ssid,    sizeof(cfg.ssid));
    strlcpy(cfg.faction, doc["prop"]["faction"]         | cfg.faction, sizeof(cfg.faction));
    cfg.cooldown_ms    = doc["behavior"]["scan_cooldown_ms"] | cfg.cooldown_ms;
    S.printf("[CFG] id=%s ssid=%s faction=%s\n", cfg.id, cfg.ssid, cfg.faction);
    return true;
}

// ═══════════════════════════════════════
//  NFC (PN532 on Wire1)
// ═══════════════════════════════════════
TwoWire nfcI2C = TwoWire(1);
Adafruit_PN532 nfc(-1, -1, &nfcI2C);
bool nfcOk = false;

// Read first NDEF text record off an NTAG into ndefText. Same algorithm as the datapad.
char ndefText[128] = "";

void readNdefText() {
    ndefText[0] = 0;
    uint8_t buf[128];
    int pos = 0;

    for (int page = 4; page < 36 && pos < 120; page++) {
        uint8_t data[4];
        if (!nfc.ntag2xx_ReadPage(page, data)) break;
        for (int i = 0; i < 4 && pos < 127; i++) buf[pos++] = data[i];
    }

    int i = 0;
    while (i < pos) {
        uint8_t tlvType = buf[i];
        if (tlvType == 0x00) { i++; continue; }
        if (tlvType == 0xFE) break;
        if (i + 1 >= pos) break;
        uint8_t tlvLen = buf[i + 1];
        int dataStart = i + 2;

        if (tlvType == 0x03 && tlvLen > 0) {
            int r = dataStart;
            if (r + 3 >= pos) break;
            uint8_t typeLen = buf[r + 1];
            uint8_t payLen  = buf[r + 2];
            int typeOff = r + 3;
            if (typeLen == 1 && typeOff < pos && buf[typeOff] == 0x54) {
                int payOff  = typeOff + typeLen;
                if (payOff >= pos) break;
                uint8_t langLen = buf[payOff] & 0x3F;
                int textOff = payOff + 1 + langLen;
                int textLen = payLen - 1 - langLen;
                if (textOff + textLen <= pos && textLen > 0 && textLen < 120) {
                    memcpy(ndefText, &buf[textOff], textLen);
                    ndefText[textLen] = 0;
                    return;
                }
            }
            break;
        }
        i = dataStart + tlvLen;
    }
}

inline bool isCargoTag(const char *text) {
    if (!text) return false;
    return (strncasecmp(text, "CARGO_", 6) == 0) || (strncasecmp(text, "CARGO-", 6) == 0);
}

// Most-recently-accepted card (used by both NFC scan handler and HTTP dialogue).
char lastCardText[64] = "";
unsigned long lastCardTime = 0;

// ═══════════════════════════════════════
//  HTTP SERVER — datapad calls /api/info on connect, /api/interact for greeting
// ═══════════════════════════════════════
AsyncWebServer server(80);
char connectedCallsign[16] = "";

void handleInfo(AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["prop_id"]  = cfg.id;
    doc["name"]     = cfg.name;
    doc["type"]     = "terminal";
    doc["faction"]  = cfg.faction;
    JsonObject feat = doc["features"].to<JsonObject>();
    feat["has_nfc"]      = true;
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

    // Universal greeting — the player presents a datacard physically at the terminal
    // hardware, then disconnects when done.
    JsonObject l1 = lines.add<JsonObject>();
    l1["text"]  = "TERMINAL ACTIVE";
    l1["style"] = "system";

    JsonObject l2 = lines.add<JsonObject>();
    if (lastCardText[0]) {
        char b[80]; snprintf(b, sizeof(b), "Last accepted: %s", lastCardText);
        l2["text"]  = b;
        l2["style"] = "info";
    } else {
        l2["text"]  = "Present datacard to reader port.";
        l2["style"] = "info";
    }

    JsonObject c1 = choices.add<JsonObject>();
    c1["label"]       = "[Disconnect]";
    c1["next_action"] = nullptr;

    String out;
    serializeJson(resp, out);
    req->send(200, "application/json", out);
}

void startHttpServer() {
    server.on("/api/info", HTTP_GET, handleInfo);
    server.on("/api/interact", HTTP_POST,
        [](AsyncWebServerRequest *r){},            // empty request handler (body comes via body cb)
        nullptr,
        handleInteract);
    server.begin();
    S.println("[HTTP] server up on :80");
}

// ═══════════════════════════════════════
//  MESH HANDLER — listen for GM overrides + sync
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
    // Other types ignored; the terminal is mostly a sender.
}

// ═══════════════════════════════════════
//  CARD SCAN ACTION
// ═══════════════════════════════════════
void onCardScanned(uint8_t *uid, uint8_t len) {
    // Cooldown per-tag so a card held on the reader doesn't spam events.
    if (strcmp(lastCardText, ndefText) == 0 && millis() - lastCardTime < (unsigned long)cfg.cooldown_ms) return;
    strlcpy(lastCardText, ndefText, sizeof(lastCardText));
    lastCardTime = millis();

    if (isCargoTag(ndefText)) {
        S.printf("[SCAN] cargo crate %s rejected (wrong reader)\n", ndefText);
        buzzDenied();
        return;
    }

    // Build a UID string for logging
    char uidStr[24];
    if (len == 4) snprintf(uidStr, sizeof(uidStr), "%02X:%02X:%02X:%02X", uid[0], uid[1], uid[2], uid[3]);
    else snprintf(uidStr, sizeof(uidStr), "%02X:%02X:%02X:%02X:%02X:%02X:%02X",
                  uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6]);
    S.printf("[SCAN] accepted '%s' uid=%s\n", ndefText, uidStr);

    buzzGranted();

    // ── Broadcast to GM (NFC scan log) ──
    swts::sendNfcScan(uidStr, ndefText, "datacard");

    // ── Broadcast an event keyed to this terminal + card.
    //    Player datapads' comms.json can subscribe via trigger: "nfc:<TERMID>:<CARDID>"
    //    Mission systems can also key off this. ──
    char eventId[40];
    snprintf(eventId, sizeof(eventId), "nfc:%s:%s", cfg.id, ndefText);
    char eventName[40];
    snprintf(eventName, sizeof(eventName), "%s scanned", ndefText);
    swts::gmTriggerEvent(eventId, eventName, /*severity*/ 0, cfg.name);

    // ── Push a comm to whoever is connected to this AP ──
    //    We don't know which datapad is connected by mesh id; broadcast a targeted-by-text comm.
    //    Each datapad checks the target field; empty = everyone, named = only matching callsign.
    //    To keep it simple, broadcast as "all" but mark FROM with this terminal name so the
    //    receiving datapad's UI shows context. ──
    char commId[20];
    snprintf(commId, sizeof(commId), "term_%s_%lu", cfg.id, millis() % 100000);
    char body[120];
    snprintf(body, sizeof(body), "Datacard %s accepted by %s. Action logged.", ndefText, cfg.name);
    swts::gmPushComm(commId, "" /*broadcast*/, cfg.name, "ACCESS GRANTED", body);
}

// ═══════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════
SPIClass sdSPI(HSPI);

void setup() {
    S.begin(115200);
    delay(200);
    S.println("\n================================");
    S.println("  SWTS TERMINAL — starting");
    S.println("================================");

    pinMode(BUZZER_PIN, OUTPUT);

    // SD
    sdSPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
    bool sdOk = SD.begin(SD_CS, sdSPI, 4000000);
    if (sdOk) {
        S.printf("[SD] Mounted %lluMB\n", SD.cardSize() / (1024 * 1024));
        // TESTING: write embedded gameplay configs to the card (no-op when
        // SWTS_WRITE_TEST_CONFIGS is commented out in swts_test_configs.h)
        swts_test::writeTestConfigs(SD);
        loadConfig();
    } else {
        S.println("[SD] Mount failed, using defaults");
    }

    // NFC
    nfcI2C.begin(NFC_SDA, NFC_SCL, 100000);
    nfc.begin();
    uint32_t fw = nfc.getFirmwareVersion();
    if (fw) {
        nfcOk = true;
        nfc.SAMConfig();
        S.printf("[NFC] PN532 ready (fw=0x%08X)\n", (unsigned)fw);
    } else {
        S.println("[NFC] PN532 NOT FOUND — check wiring (SDA=14, SCL=13, I2C mode)");
    }

    // WiFi AP — players connect their datapad to this network to interact.
    // Forced to the mesh channel so ESPNOW broadcasts coexist.
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    WiFi.softAP(cfg.ssid, nullptr, MESH_CHANNEL);
    S.printf("[WIFI] AP '%s' on channel %d, IP=%s\n", cfg.ssid, MESH_CHANNEL, WiFi.softAPIP().toString().c_str());

    // HTTP server so the datapad's /api/info + /api/interact contract works
    startHttpServer();

    // Mesh
    swts::meshInit(cfg.id, swts::ROLE_PANEL, onMeshMsg);

    // Boot tone — two quick beeps so you know the terminal is alive
    buzzerTone(800, 60); delay(80); buzzerTone(1200, 60);

    S.println("[TERMINAL] Ready — waiting for cards");
}

// ═══════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════
void loop() {
    if (nfcOk) {
        uint8_t uid[7];
        uint8_t len;
        if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 80)) {
            readNdefText();
            if (ndefText[0]) onCardScanned(uid, len);
            // small delay before next read so we don't double-fire on the same card
            delay(80);
        }
    }

    // Heartbeat to GM every 10 seconds
    static unsigned long lastBeat = 0;
    if (millis() - lastBeat > 10000) {
        lastBeat = millis();
        swts::sendPing(millis() / 1000);
    }

    delay(10);
}
