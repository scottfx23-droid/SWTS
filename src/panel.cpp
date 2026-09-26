/*
 * SWTS Panel — Headless hackable wall terminal
 * ESP32-S3-WROOM-1 N8R8
 *
 * Boot: Check SD card → copy /SWTS/ to LittleFS → run from flash
 * Config: /SWTS/config.json on LittleFS
 * WiFi AP + HTTP API + NFC card slot
 *
 * SD Card: GPIO15=CS, GPIO16=MOSI, GPIO17=CLK, GPIO18=MISO
 * NFC:     GPIO38=SDA, GPIO39=SCL (I2C bus 1)
 */

#include <WiFi.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <LittleFS.h>
#include <Adafruit_PN532.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "swts_mesh.h"
#include "swts_test_configs.h"   // boot-time test provisioning (see header to disable)

#define S Serial

// ═══════════════════════════════════════
//  SD CARD PINS
// ═══════════════════════════════════════
#define SD_CS   15
#define SD_MOSI 16
#define SD_CLK  17
#define SD_MISO 18
SPIClass sdSPI(HSPI);

// ═══════════════════════════════════════
//  SD → LittleFS PROVISIONING
//  On boot: if SD present, wipe flash
//  and copy all /SWTS/ files to LittleFS.
//  Then run entirely from flash.
// ═══════════════════════════════════════

void deleteRecursive(fs::FS &fs, const char *path) {
    File dir = fs.open(path);
    if (!dir || !dir.isDirectory()) return;
    File f = dir.openNextFile();
    while (f) {
        String fpath = String(path) + "/" + f.name();
        if (f.isDirectory()) {
            f.close();
            deleteRecursive(fs, fpath.c_str());
            fs.rmdir(fpath.c_str());
        } else {
            f.close();
            fs.remove(fpath.c_str());
        }
        f = dir.openNextFile();
    }
    dir.close();
}

void copyFile(fs::FS &src, const char *srcPath, fs::FS &dst, const char *dstPath) {
    File in = src.open(srcPath, FILE_READ);
    if (!in) return;
    File out = dst.open(dstPath, FILE_WRITE);
    if (!out) { in.close(); return; }
    uint8_t buf[512];
    while (in.available()) {
        size_t n = in.read(buf, sizeof(buf));
        out.write(buf, n);
    }
    out.close();
    in.close();
}

void copyDir(fs::FS &src, const char *srcDir, fs::FS &dst, const char *dstDir) {
    dst.mkdir(dstDir);
    File dir = src.open(srcDir);
    if (!dir || !dir.isDirectory()) return;
    File f = dir.openNextFile();
    while (f) {
        String srcPath = String(srcDir) + "/" + f.name();
        String dstPath = String(dstDir) + "/" + f.name();
        if (f.isDirectory()) {
            f.close();
            copyDir(src, srcPath.c_str(), dst, dstPath.c_str());
        } else {
            size_t sz = f.size();
            f.close();
            copyFile(src, srcPath.c_str(), dst, dstPath.c_str());
            S.printf("  %s (%d bytes)\n", dstPath.c_str(), sz);
        }
        f = dir.openNextFile();
    }
    dir.close();
}

bool provisionFromSD() {
    S.println("[SD] Checking for SD card...");
    sdSPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);

    if (!SD.begin(SD_CS, sdSPI, 4000000)) {  // 4MHz, lower speed for reliability
        S.println("[SD] No SD card — running from flash");
        sdSPI.end();
        return false;
    }

    // Check if /SWTS/ exists on SD
    if (!SD.exists("/SWTS/config.json")) {
        S.println("[SD] No /SWTS/config.json on SD — skipping");
        SD.end();
        return false;
    }

    S.println("[SD] SD card detected — provisioning to flash...");

    // Wipe existing /SWTS/ on flash
    if (LittleFS.exists("/SWTS")) {
        S.println("[SD] Erasing old flash data...");
        deleteRecursive(LittleFS, "/SWTS");
        LittleFS.rmdir("/SWTS");
    }

    // Copy /SWTS/ from SD to LittleFS
    S.println("[SD] Copying files:");
    copyDir(SD, "/SWTS", LittleFS, "/SWTS");

    SD.end();
    S.println("[SD] Provisioning complete — SD card can be removed");
    SD.end();
    sdSPI.end();
    return true;
}

// ═══════════════════════════════════════
//  CONFIG — loaded from LittleFS
// ═══════════════════════════════════════
struct PanelConfig {
    char id[16]            = "PANEL_01";
    char ssid[33]          = "SWTS_PANEL_01";
    char name[32]          = "Outpost Data Terminal";
    char faction[12]       = "NEUTRAL";
    int  wifi_channel      = 6;
    bool has_nfc           = true;
    bool requires_auth     = false;
    bool has_minigame      = true;
    char minigame_type[16] = "slice";   // "slice" or "simon"
    int  minigame_diff     = 2;
    int  simon_rounds      = 3;         // Simon Says rounds (when minigame_type == "simon")
    bool slice_first       = true;   // require slice before showing menu
    char auth_cards[4][32] = {};
    int  num_auth_cards    = 0;
};
PanelConfig cfg;

// Per-connection state: has this player sliced yet?
bool playerSliced = false;

bool loadConfig() {
    File f = LittleFS.open("/SWTS/config.json", "r");
    if (!f) {
        S.println("[CFG] No config on flash — using defaults");
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        S.printf("[CFG] Parse error: %s\n", err.c_str());
        return false;
    }

    strlcpy(cfg.id, doc["prop"]["id"] | cfg.id, sizeof(cfg.id));
    strlcpy(cfg.ssid, doc["prop"]["ssid"] | cfg.ssid, sizeof(cfg.ssid));
    strlcpy(cfg.name, doc["prop"]["name"] | cfg.name, sizeof(cfg.name));
    strlcpy(cfg.faction, doc["prop"]["faction"] | cfg.faction, sizeof(cfg.faction));
    cfg.wifi_channel = doc["prop"]["wifi_channel"] | cfg.wifi_channel;
    cfg.has_nfc = doc["hardware"]["has_nfc_reader"] | cfg.has_nfc;
    cfg.requires_auth = doc["behavior"]["requires_auth_card"] | cfg.requires_auth;

    // NOTE: `| nullptr` doesn't work as a default here (ArduinoJson deduces
    // nullptr_t and always returns null) — use as<const char*>() instead,
    // which returns nullptr for a missing/non-string value.
    const char* mg = doc["behavior"]["minigame_default"].as<const char*>();
    cfg.has_minigame = (mg != nullptr && mg[0] != '\0');
    if (cfg.has_minigame) strlcpy(cfg.minigame_type, mg, sizeof(cfg.minigame_type));
    cfg.simon_rounds = doc["behavior"]["simon_rounds"] | cfg.simon_rounds;

    // Slice-before-menu defaults to "has a minigame"; config may override
    cfg.slice_first = doc["behavior"]["slice_first"] | cfg.has_minigame;

    JsonArray auth = doc["behavior"]["auth_card_ids"].as<JsonArray>();
    cfg.num_auth_cards = 0;
    for (JsonVariant v : auth) {
        if (cfg.num_auth_cards < 4)
            strlcpy(cfg.auth_cards[cfg.num_auth_cards++], v.as<const char*>(), 32);
    }

    S.printf("[CFG] Loaded: %s (%s) faction=%s minigame=%s slice_first=%d\n",
             cfg.name, cfg.id, cfg.faction,
             cfg.has_minigame ? cfg.minigame_type : "none", cfg.slice_first);
    return true;
}

// Load a lore file from flash
String loadLoreFile(const char* filename) {
    String path = String("/SWTS/") + filename;
    File f = LittleFS.open(path, "r");
    if (!f) {
        // Try the lore directory
        File dir = LittleFS.open("/SWTS/lore");
        if (dir && dir.isDirectory()) {
            f = dir.openNextFile();
            if (!f) { dir.close(); return "No data available."; }
            dir.close();
        } else {
            return "No data available.";
        }
    }
    String content = f.readString();
    f.close();
    return content;
}

// ═══════════════════════════════════════
//  NFC
// ═══════════════════════════════════════
TwoWire I2C_NFC = TwoWire(1);
Adafruit_PN532 nfc(-1, -1, &I2C_NFC);
bool nfcOk = false;
char lastNfcUid[24] = "";
unsigned long lastNfcTime = 0;
bool cardPresent = false;

bool pollNfc() {
    uint8_t uid[7]; uint8_t len;
    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 50)) {
        if (len == 4)
            snprintf(lastNfcUid, sizeof(lastNfcUid), "%02X:%02X:%02X:%02X", uid[0], uid[1], uid[2], uid[3]);
        else
            snprintf(lastNfcUid, sizeof(lastNfcUid), "%02X:%02X:%02X:%02X:%02X:%02X:%02X", uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6]);
        lastNfcTime = millis();
        cardPresent = true;
        return true;
    }
    if (millis() - lastNfcTime > 2000) cardPresent = false;
    return false;
}

// ═══════════════════════════════════════
//  PANEL STATE
// ═══════════════════════════════════════
enum PanelState { STATE_IDLE, STATE_CONNECTED, STATE_ACTIVE, STATE_MINIGAME, STATE_HACKED };
PanelState panelState = STATE_IDLE;
int connectCount = 0;
unsigned long lastActivity = 0;
String connectedCallsign = "";

// ═══════════════════════════════════════
//  HTTP SERVER
// ═══════════════════════════════════════
AsyncWebServer server(80);

void handleInfo(AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["prop_id"] = cfg.id;
    doc["name"] = cfg.name;
    doc["type"] = "panel";
    doc["faction"] = cfg.faction;

    JsonObject feat = doc["features"].to<JsonObject>();
    feat["has_nfc"] = cfg.has_nfc;
    feat["has_minigame"] = cfg.has_minigame;
    feat["requires_auth"] = cfg.requires_auth;
    feat["minigame_type"] = cfg.minigame_type;
    feat["card_present"] = cardPresent;
    feat["slice_first"] = cfg.slice_first;

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

    const char* action = reqDoc["action"] | "greet";
    const char* callsign = reqDoc["player"]["callsign"] | "UNKNOWN";

    connectedCallsign = callsign;
    lastActivity = millis();
    panelState = STATE_ACTIVE;

    JsonDocument resp;

    // Which slice action the menu offers depends on the configured minigame.
    const char* mgAction = (strcmp(cfg.minigame_type, "simon") == 0)
                               ? "start_simon" : "start_minigame";

    if (strcmp(action, "greet") == 0) {
        resp["type"] = "dialogue";
        JsonObject speaker = resp["speaker"].to<JsonObject>();
        speaker["name"] = cfg.name;

        JsonArray lines = resp["lines"].to<JsonArray>();
        JsonArray choices = resp["choices"].to<JsonArray>();

        if (cfg.slice_first && !playerSliced) {
            // ── LOCKED: must slice first ──
            JsonObject l1 = lines.add<JsonObject>();
            l1["text"] = "SYSTEM LOCKED // SECURITY ACTIVE";
            l1["style"] = "system";

            JsonObject l2 = lines.add<JsonObject>();
            l2["text"] = "SLICE REQUIRED FOR ACCESS";
            l2["style"] = "system";

            JsonObject c1 = choices.add<JsonObject>();
            c1["label"] = "Slice into system";
            c1["next_action"] = mgAction;

            JsonObject c2 = choices.add<JsonObject>();
            c2["label"] = "[Disconnect]";
            c2["next_action"] = nullptr;
        } else {
            // ── UNLOCKED: show full menu ──
            JsonObject l1 = lines.add<JsonObject>();
            l1["text"] = playerSliced ? "SYSTEM BREACHED // ACCESS GRANTED" : "SYSTEM ONLINE // AWAITING INPUT";
            l1["style"] = "system";

            if (cardPresent) {
                JsonObject l2 = lines.add<JsonObject>();
                l2["text"] = "DATACARD DETECTED IN SLOT";
                l2["style"] = "system";
            }

            JsonObject c1 = choices.add<JsonObject>();
            c1["label"] = "Access data logs";
            c1["next_action"] = "read_logs";

            if (cfg.has_minigame && !playerSliced) {
                JsonObject c2 = choices.add<JsonObject>();
                c2["label"] = "Slice into system";
                c2["next_action"] = mgAction;
            }

            if (playerSliced) {
                // Breached boards can transmit on the outpost's open channel —
                // the EXTRACTION mission uses this to signal the shuttle.
                JsonObject cx = choices.add<JsonObject>();
                cx["label"] = "Broadcast extraction signal";
                cx["next_action"] = "send_signal";
            }

            JsonObject c3 = choices.add<JsonObject>();
            c3["label"] = "[Disconnect]";
            c3["next_action"] = nullptr;
        }
    }
    else if (strcmp(action, "start_minigame") == 0) {
        panelState = STATE_MINIGAME;
        resp["type"] = "minigame_start";
        resp["game"] = cfg.minigame_type;
        resp["difficulty"] = cfg.minigame_diff;
        resp["time_limit"] = 50 - cfg.minigame_diff * 5;

        JsonArray zones = resp["target_zones"].to<JsonArray>();
        JsonObject z1 = zones.add<JsonObject>();
        z1["start"] = 0.20; z1["end"] = 0.38; z1["points"] = 100;
        JsonObject z2 = zones.add<JsonObject>();
        z2["start"] = 0.60; z2["end"] = 0.78; z2["points"] = 80;
        if (cfg.minigame_diff >= 3) {
            JsonObject z3 = zones.add<JsonObject>();
            z3["start"] = 0.85; z3["end"] = 0.95; z3["points"] = 120;
        }
    }
    else if (strcmp(action, "start_simon") == 0) {
        // Simon Says (pattern lock) — rounds come from config.
        panelState = STATE_MINIGAME;
        resp["type"] = "minigame_start";
        resp["game"] = "simon";
        resp["rounds"] = cfg.simon_rounds;
    }
    else if (strcmp(action, "minigame_result") == 0) {
        bool won = reqDoc["won"] | false;
        resp["type"] = "minigame_result";
        resp["accepted"] = true;

        if (won) {
            panelState = STATE_HACKED;
            playerSliced = true;
            resp["message"] = "SYSTEM BREACHED -- ACCESS GRANTED";
            resp["unlocked"] = true;
            JsonObject rewards = resp["rewards"].to<JsonObject>();
            rewards["xp"] = 50;
            JsonArray items = rewards["items"].to<JsonArray>();
            char itemId[32];
            snprintf(itemId, sizeof(itemId), "DATA_%s", cfg.id);
            items.add(itemId);
        } else {
            panelState = STATE_ACTIVE;
            resp["message"] = "SLICE FAILED -- SECURITY HOLDING";
            resp["retry"] = true;
        }
    }
    else if (strcmp(action, "send_signal") == 0) {
        resp["type"] = "dialogue";
        JsonObject speaker = resp["speaker"].to<JsonObject>();
        speaker["name"] = cfg.name;
        JsonArray lines = resp["lines"].to<JsonArray>();
        JsonArray choices = resp["choices"].to<JsonArray>();

        if (!playerSliced) {
            // Can't transmit through active security — slice first
            JsonObject l1 = lines.add<JsonObject>();
            l1["text"] = "TRANSMIT BLOCKED // SECURITY ACTIVE";
            l1["style"] = "system";
            JsonObject c1 = choices.add<JsonObject>();
            c1["label"] = "Slice into system";
            c1["next_action"] = mgAction;
        } else {
            // Broadcast the extraction signal — advances the EXTRACTION mission
            char eventId[40];
            snprintf(eventId, sizeof(eventId), "extraction:%s", cfg.id);
            swts::gmTriggerEvent(eventId, "Extraction signal broadcast", 0, callsign);
            resp["game_event"] = eventId;

            JsonObject l1 = lines.add<JsonObject>();
            l1["text"] = "WIDEBAND BURST TRANSMITTED // ALLIANCE CODE 7-7";
            l1["style"] = "system";
            JsonObject l2 = lines.add<JsonObject>();
            l2["text"] = "Signal away. If anyone's listening out there, they know to come get you.";
            l2["style"] = "speech";
        }
        JsonObject c2 = choices.add<JsonObject>();
        c2["label"] = "[Disconnect]";
        c2["next_action"] = nullptr;
    }
    else if (strcmp(action, "read_logs") == 0 && cfg.slice_first && !playerSliced) {
        // Menu shouldn't offer logs before a slice, but enforce it here too
        resp["type"] = "lore";
        resp["title"] = "ACCESS DENIED";
        resp["classification"] = "SECURITY LOCKOUT";
        resp["content"] = "Encryption active.\nSlice into the system first.";
    }
    else if (strcmp(action, "read_logs") == 0) {
        // Load lore from flash
        String content = loadLoreFile("lore/l001.txt");

        // Parse TITLE/CLASS/body
        resp["type"] = "lore";
        int ti = content.indexOf("TITLE:");
        int ci = content.indexOf("CLASS:");
        int bs = content.indexOf("---");
        int be = content.lastIndexOf("---");

        if (ti >= 0) {
            int nl = content.indexOf('\n', ti);
            String t = content.substring(ti + 6, nl); t.trim();
            resp["title"] = t;
        } else {
            resp["title"] = "DATA LOG";
        }
        if (ci >= 0) {
            int nl = content.indexOf('\n', ci);
            String c = content.substring(ci + 6, nl); c.trim();
            resp["classification"] = c;
        } else {
            resp["classification"] = "";
        }
        if (bs >= 0 && be > bs) {
            String body = content.substring(bs + 4, be); body.trim();
            resp["content"] = body;
        } else {
            resp["content"] = content;
        }
    }

    String out;
    serializeJson(resp, out);
    req->send(200, "application/json", out);
    S.printf("[HTTP] %s from %s\n", action, callsign);
}

void handleNfc(AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
    JsonDocument reqDoc;
    if (deserializeJson(reqDoc, data, len)) {
        req->send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }

    const char* tagId = reqDoc["tag_id"] | "";
    JsonDocument resp;
    resp["accepted"] = true;
    resp["message"] = "DATACARD ACKNOWLEDGED";

    if (cfg.requires_auth) {
        bool authed = false;
        for (int i = 0; i < cfg.num_auth_cards; i++)
            if (strcmp(tagId, cfg.auth_cards[i]) == 0) { authed = true; break; }
        resp["accepted"] = authed;
        resp["effect"] = authed ? "unlock" : "denied";
        resp["message"] = authed ? "ACCESS GRANTED" : "CLEARANCE INSUFFICIENT";
        if (authed) panelState = STATE_ACTIVE;
    }

    String out;
    serializeJson(resp, out);
    req->send(200, "application/json", out);
}

// ═══════════════════════════════════════
//  MESH HANDLER (panel — accept GM overrides)
// ═══════════════════════════════════════
bool panelDisabled = false;
int  diffOverride = -1;  // -1 = use config, else override

void onMeshMsg(const swts::MeshHeader *hdr, const uint8_t *payload, int len) {
    switch (hdr->type) {
        case swts::MSG_OVERRIDE: {
            if (len < (int)sizeof(swts::MeshOverride)) return;
            const swts::MeshOverride *o = (const swts::MeshOverride *)payload;
            // Check if it's for us
            if (strcmp(o->target_prop, cfg.id) != 0 && strcmp(o->target_prop, "ALL") != 0) return;
            S.printf("[MESH] Override: %s %s = %s\n", o->target_prop, o->command, o->value);
            if (strcmp(o->command, "offline") == 0) panelDisabled = true;
            else if (strcmp(o->command, "online") == 0) panelDisabled = false;
            else if (strcmp(o->command, "difficulty") == 0) diffOverride = atoi(o->value);
            else if (strcmp(o->command, "reset") == 0) { delay(200); ESP.restart(); }
            break;
        }
        case swts::MSG_RESET: {
            S.println("[MESH] Reset received");
            delay(200);
            ESP.restart();
            break;
        }
        default: break;
    }
}

void setupServer() {
    server.on("/api/info", HTTP_GET, handleInfo);
    server.on("/api/interact", HTTP_POST,
              [](AsyncWebServerRequest *r){}, NULL, handleInteract);
    server.on("/api/nfc", HTTP_POST,
              [](AsyncWebServerRequest *r){}, NULL, handleNfc);
    server.begin();
    S.println("[HTTP] Server ready");
}

// ═══════════════════════════════════════
//  WiFi events
// ═══════════════════════════════════════
void onWiFiEvent(WiFiEvent_t event) {
    if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
        connectCount++;
        lastActivity = millis();
        playerSliced = false;  // new connection = needs to slice again
        if (panelState == STATE_IDLE) panelState = STATE_CONNECTED;
        S.printf("[WiFi] Client connected (%d total)\n", connectCount);
    }
    else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
        if (panelState != STATE_HACKED) panelState = STATE_IDLE;
        playerSliced = false;
        connectedCallsign = "";
        S.println("[WiFi] Client disconnected");
    }
}

// ═══════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════
void setup() {
    S.begin(115200);
    delay(500);
    S.println("\n=== SWTS PANEL ===");

    // Init flash filesystem
    if (!LittleFS.begin(true)) {
        S.println("[FS] LittleFS mount failed!");
    } else {
        S.println("[FS] Flash filesystem ready");
    }

    // Check for SD card — provision to flash if found
    provisionFromSD();

    // TESTING: overwrite flash with embedded gameplay configs (no-op when
    // SWTS_WRITE_TEST_CONFIGS is commented out in swts_test_configs.h)
    swts_test::writeTestConfigs(LittleFS);

    // Load config from flash
    loadConfig();

    S.printf("Panel: %s (%s)\n", cfg.name, cfg.id);

    // NFC
    I2C_NFC.begin(38, 39, 100000);
    delay(50);
    for (uint8_t a = 1; a < 127; a++) {
        I2C_NFC.beginTransmission(a);
        if (I2C_NFC.endTransmission() == 0) {
            nfc.begin();
            uint32_t fw = nfc.getFirmwareVersion();
            if (fw) { nfcOk = true; nfc.SAMConfig(); }
            break;
        }
    }
    S.printf("NFC: %s\n", nfcOk ? "READY" : "NOT FOUND");

    // WiFi AP + STA (STA needed for ESPNOW). Both use MESH_CHANNEL.
    WiFi.onEvent(onWiFiEvent);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(cfg.ssid, NULL, MESH_CHANNEL);  // force mesh channel
    S.printf("WiFi AP: %s (ch %d)\n", cfg.ssid, MESH_CHANNEL);
    S.printf("IP: %s\n", WiFi.softAPIP().toString().c_str());

    setupServer();

    // ── ESPNOW Mesh ──
    swts::meshInit(cfg.id, swts::ROLE_PANEL, onMeshMsg);

    S.println("Ready.");
}

// ═══════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════
void loop() {
    static unsigned long lastPoll = 0;
    if (nfcOk && millis() - lastPoll > 500) {
        lastPoll = millis();
        pollNfc();
    }

    // Auto-reset after 60s idle
    if (panelState != STATE_IDLE && millis() - lastActivity > 60000) {
        panelState = STATE_IDLE;
        playerSliced = false;
        connectedCallsign = "";
    }

    // Heartbeat (serial + mesh)
    static unsigned long lastBeat = 0;
    if (millis() - lastBeat > 10000) {
        lastBeat = millis();
        const char* st[] = {"IDLE","CONN","ACTIVE","SLICE","HACKED"};
        S.printf("[%s] %s | sliced=%d | nfc=%s | card=%s\n",
                 cfg.id, st[panelState], playerSliced,
                 nfcOk ? "ok" : "--", cardPresent ? lastNfcUid : "--");
        swts::sendPing(millis() / 1000);
    }

    delay(10);
}
