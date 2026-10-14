/*
 * SFGE Panel — Headless hackable wall terminal
 * ESP32-S3-WROOM-1 N8R8
 *
 * Boot: Check SD card → copy /SFGE/ to LittleFS → run from flash
 * Config: /SFGE/config.json on LittleFS
 * WiFi AP + HTTP API + NFC card slot
 *
 * SD Card: GPIO15=CS, GPIO16=MOSI, GPIO17=CLK, GPIO18=MISO
 * NFC:     GPIO14=SDA, GPIO13=SCL (I2C bus 1 — same as the terminal)
 */

#include <WiFi.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <LittleFS.h>
#include <Adafruit_PN532.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "sfge_mesh.h"
#include "sfge_lights.h"         // addressable RGB strip (/SFGE/lights.txt)
#include "sfge_proximity.h"      // RSSI gate for interactions
#include "sfge_ota.h"            // firmware-from-SD updates
#include "sfge_scenario.h"       // GM-selected scenario -> content folder
#include "sfge_test_configs.h"   // boot-time test provisioning (see header to disable)

#define S Serial

// ═══════════════════════════════════════
//  BUZZER — passive piezo on GPIO 4
// ═══════════════════════════════════════
#define BUZZER_PIN 4
inline void buzzerTone(int freq, int ms) { tone(BUZZER_PIN, freq, ms); }

// ═══════════════════════════════════════
//  INTERACTION FEEDBACK — lights + buzzer
//  HTTP handlers run on the async task, so they only set pendingFx;
//  loop() applies it (NeoPixel writes + tone stay on the main task).
// ═══════════════════════════════════════
enum PropFx : uint8_t { FX_NONE = 0, FX_ACTIVITY, FX_SUCCESS, FX_FAIL, FX_SIGNAL };
volatile uint8_t pendingFx = FX_NONE;

void applyPendingFx() {
    uint8_t fx = pendingFx;
    if (fx == FX_NONE) return;
    pendingFx = FX_NONE;
    switch (fx) {
    case FX_ACTIVITY:
        sfge_lights::effectActivity();
        buzzerTone(1200, 40);
        break;
    case FX_SUCCESS:
        sfge_lights::effectSuccess();
        buzzerTone(523, 80); delay(90); buzzerTone(784, 80); delay(90); buzzerTone(1046, 140);
        break;
    case FX_FAIL:
        sfge_lights::effectFail();
        buzzerTone(220, 250); delay(260); buzzerTone(160, 300);
        break;
    case FX_SIGNAL:
        sfge_lights::effectSignal();
        buzzerTone(880, 60); delay(70); buzzerTone(1320, 60); delay(70); buzzerTone(1760, 120);
        break;
    }
}

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
//  and copy all /SFGE/ files to LittleFS.
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

    bool mounted = false;   // retried: warm reboots can wedge one attempt
    for (int t = 0; t < 3 && !mounted; t++) {
        if (t) { SD.end(); delay(250); S.printf("[SD] mount retry %d...\n", t); }
        mounted = SD.begin(SD_CS, sdSPI, 4000000);   // 4MHz for reliability
    }
    if (!mounted) {
        S.println("[SD] No SD card — running from flash");
        sdSPI.end();
        return false;
    }

    // Firmware update staged on the card? Flash it before provisioning.
    sfge_ota::checkAndFlash(SD);

    // Check if /SFGE/ exists on SD
    if (!SD.exists("/SFGE/config.json")) {
        S.println("[SD] No /SFGE/config.json on SD — skipping");
        SD.end();
        return false;
    }

    S.println("[SD] SD card detected — provisioning to flash...");

    // Wipe existing /SFGE/ on flash
    if (LittleFS.exists("/SFGE")) {
        S.println("[SD] Erasing old flash data...");
        deleteRecursive(LittleFS, "/SFGE");
        LittleFS.rmdir("/SFGE");
    }

    // Copy /SFGE/ from SD to LittleFS
    S.println("[SD] Copying files:");
    copyDir(SD, "/SFGE", LittleFS, "/SFGE");

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
    char ssid[33]          = "SFGE_PANEL_01";
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
    // Alarm state (tripped by consecutive hostile failures)
    int  alarm_after_fails = 2;
    int  alarm_timeout_s   = 90;
    int  alarm_siren_ms    = 350;
    bool timer_visual      = false;  // strip red-shifts as a GM countdown runs out
    bool sabotage_enabled  = false;  // hostile players get a SABOTAGE choice
    // AUX port (the spare second-NFC pins, SDA/A=38 SCL/B=39):
    //   "none"    — unused (default)
    //   "buttons" — two momentary buttons to GND; each press fires its event
    //   "nfc2"    — second PN532: a code-cylinder slot like the terminal's
    char aux_mode[12] = "none";
    struct { char event[32]; char name[32]; } aux_btn[2];
    int  button_cooldown_ms = 1500;
    bool startup_song      = true;   // boot jingle on the buzzer (opt-out)
};
PanelConfig cfg;

// ── Sabotage state: prop is OFFLINE until someone wins the repair game ──
bool sabotaged = false;
bool repairBypass = false;           // next minigame_result is the repair game
volatile bool sabDirty = false;      // lights change applied from loop()

void setSabotaged(bool on, const char *byCallsign) {
    if (on == sabotaged) return;
    sabotaged = on;
    sabDirty = true;
    char eventId[40];
    if (on) {
        snprintf(eventId, sizeof(eventId), "sabotaged:%s", cfg.id);
        sfge::gmTriggerEvent(eventId, "Prop sabotaged", 2, byCallsign ? byCallsign : "");
    } else {
        snprintf(eventId, sizeof(eventId), "prop_repaired:%s", cfg.id);
        sfge::gmTriggerEvent(eventId, "Prop repaired", 0, byCallsign ? byCallsign : "");
    }
    S.printf("[SAB] %s by %s\n", on ? "SABOTAGED" : "REPAIRED", byCallsign ? byCallsign : "GM");
}

// Per-connection state: has this player sliced yet?
bool playerSliced = false;

bool loadConfig() {
    File f = LittleFS.open(sfge_scn::path("config.json"), "r");
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

    cfg.alarm_after_fails = doc["behavior"]["alarm_after_fails"]     | cfg.alarm_after_fails;
    cfg.alarm_timeout_s   = doc["behavior"]["alarm_timeout_s"]       | cfg.alarm_timeout_s;
    cfg.alarm_siren_ms    = doc["behavior"]["alarm_siren_period_ms"] | cfg.alarm_siren_ms;
    cfg.timer_visual      = doc["behavior"]["timer_visual"]          | cfg.timer_visual;
    cfg.sabotage_enabled  = doc["behavior"]["sabotage_enabled"]      | cfg.sabotage_enabled;

    // AUX port: "buttons" turns the spare NFC2 pins into two momentary
    // buttons, "nfc2" into a code-cylinder reader
    strlcpy(cfg.aux_mode, doc["hardware"]["aux_mode"] | cfg.aux_mode, sizeof(cfg.aux_mode));
    cfg.button_cooldown_ms = doc["behavior"]["button_cooldown_ms"] | cfg.button_cooldown_ms;
    cfg.startup_song       = doc["behavior"]["startup_song"]       | cfg.startup_song;
    JsonArray btns = doc["behavior"]["buttons"].as<JsonArray>();
    for (int b = 0; b < 2; b++) {
        cfg.aux_btn[b].event[0] = cfg.aux_btn[b].name[0] = 0;
        if (!btns.isNull() && b < (int)btns.size()) {
            strlcpy(cfg.aux_btn[b].event, btns[b]["event"] | "", sizeof(cfg.aux_btn[b].event));
            strlcpy(cfg.aux_btn[b].name,  btns[b]["name"]  | "", sizeof(cfg.aux_btn[b].name));
        }
    }
    sfge_prox::loadConfig(doc);

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
    String path = String(sfge_scn::root) + "/" + filename;
    File f = LittleFS.open(path, "r");
    if (!f) {
        // Try the lore directory
        File dir = LittleFS.open(sfge_scn::path("lore"));
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

// Forward decls — the generic NDEF reader + token handler live with the
// AUX section below (they share code with the second reader)
void readNdefFrom(Adafruit_PN532 &rdr, char *out, size_t outSz);
void onPanelCardScanned();
char panelNdefText[128] = "";

// Probe the primary reader (wakeup + firmware version). Called at boot and,
// while no reader has been found, every 10 s from loop() — so plugging in
// or fixing a reader is picked up live without a reboot. If the normal pin
// order finds nothing, the SWAPPED order (SDA=13/SCL=14) is tried too: a
// mirrored harness then still works, with a loud note in the log.
void panelNfcProbe(bool verbose) {
    for (int order = 0; order < 2 && !nfcOk; order++) {
        if (order == 1) { I2C_NFC.end(); I2C_NFC.begin(13, 14, 100000); delay(30); }
        nfc.begin();
        for (int tries = 0; tries < 2 && !nfcOk; tries++) {
            uint32_t fw = nfc.getFirmwareVersion();
            if (fw) {
                nfcOk = true;
                nfc.SAMConfig();
                S.printf("NFC: READY (fw=0x%08X)%s\n", (unsigned)fw,
                         order ? "  ** SDA/SCL ARE SWAPPED ON THIS HARNESS — "
                                 "running swapped; fix when convenient **" : "");
            } else if (tries == 0) {
                delay(100);   // one more chance after a wakeup beat
            }
        }
    }
    if (!nfcOk) {
        // Back to the standard order for the next probe round
        I2C_NFC.end();
        I2C_NFC.begin(14, 13, 100000);
        if (verbose) S.println("NFC: probe failed (both pin orders)");
    }
}

bool pollNfc() {
    uint8_t uid[7]; uint8_t len;
    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 50)) {
        if (len == 4)
            snprintf(lastNfcUid, sizeof(lastNfcUid), "%02X:%02X:%02X:%02X", uid[0], uid[1], uid[2], uid[3]);
        else
            snprintf(lastNfcUid, sizeof(lastNfcUid), "%02X:%02X:%02X:%02X:%02X:%02X:%02X", uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6]);
        lastNfcTime = millis();
        cardPresent = true;
        // Read the token and let the game react (DATACARD_##, keys, ...)
        readNdefFrom(nfc, panelNdefText, sizeof(panelNdefText));
        if (panelNdefText[0]) onPanelCardScanned();
        return true;
    }
    if (millis() - lastNfcTime > 2000) cardPresent = false;
    return false;
}

// ═══════════════════════════════════════
//  AUX PORT — the spare second-NFC pins (A/SDA=38, B/SCL=39)
//  hardware.aux_mode picks what lives there:
//    "buttons" — two momentary buttons to GND (INPUT_PULLUP). Each press
//                fires its configured game event over the mesh, with light
//                + buzzer feedback. Big dramatic physical buttons.
//    "nfc2"    — a second PN532: a code-cylinder slot exactly like the
//                terminal's (CODE_* tokens only, fires cyl:<ID>:<TOKEN>).
// ═══════════════════════════════════════
#define AUX_PIN_A 38
#define AUX_PIN_B 39

TwoWire I2C_NFC2 = TwoWire(0);
Adafruit_PN532 nfc2(-1, -1, &I2C_NFC2);
bool nfc2Ok = false;
char auxNdefText[128] = "";

// Read the first NDEF text record off an NTAG2xx (same walk as the terminal)
// from whichever reader is passed in.
void readNdefFrom(Adafruit_PN532 &rdr, char *out, size_t outSz) {
    out[0] = 0;
    uint8_t buf[128];
    int pos = 0;
    for (int page = 4; page < 36 && pos < 120; page++) {
        uint8_t data[4];
        if (!rdr.ntag2xx_ReadPage(page, data)) break;
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
            uint8_t payLen = buf[r + 2];
            int typeOff = r + 3;
            if (typeLen == 1 && typeOff < pos && buf[typeOff] == 0x54) {
                int payOff = typeOff + typeLen;
                if (payOff >= pos) break;
                uint8_t langLen = buf[payOff] & 0x3F;
                int textOff = payOff + 1 + langLen;
                int textLen = payLen - 1 - langLen;
                if (textOff + textLen <= pos && textLen > 0 &&
                    textLen < (int)outSz - 1) {
                    memcpy(out, &buf[textOff], textLen);
                    out[textLen] = 0;
                    return;
                }
            }
            break;
        }
        i = dataStart + tlvLen;
    }
}

// ── Primary-reader card scans ──
// Any non-cargo token is accepted: feedback, GM scan log, and a broadcast
// nfc:<PANEL_ID>:<TOKEN> event (mission step target / comm trigger) — the
// same convention as terminal and droid scans. DATACARD_## slots right in.
void onPanelCardScanned() {
    static char lastCard[64] = "";
    static unsigned long lastCardTime2 = 0;
    if (strcmp(lastCard, panelNdefText) == 0 && millis() - lastCardTime2 < 1500) return;
    strlcpy(lastCard, panelNdefText, sizeof(lastCard));
    lastCardTime2 = millis();

    extern bool sabotaged;
    if (sabotaged) return;   // a broken panel reads nothing

    if (strncasecmp(panelNdefText, "CARGO_", 6) == 0 ||
        strncasecmp(panelNdefText, "CARGO-", 6) == 0) {
        S.printf("[NFC] cargo crate %s rejected (wrong reader)\n", panelNdefText);
        buzzerTone(300, 150);
        sfge_lights::effectFail();
        return;
    }

    S.printf("[NFC] accepted '%s'\n", panelNdefText);
    buzzerTone(880, 60);
    sfge_lights::effectSuccess();
    sfge::sendNfcScan(lastNfcUid, panelNdefText, "datacard");

    char eventId[48], eventName[40];
    snprintf(eventId, sizeof(eventId), "nfc:%s:%s", cfg.id, panelNdefText);
    snprintf(eventName, sizeof(eventName), "%s scanned", panelNdefText);
    sfge::gmTriggerEvent(eventId, eventName, /*severity*/ 0, cfg.name);
}

// Momentary button state (debounced edges, per-button cooldown)
struct AuxBtnState { bool down; unsigned long lastChange; unsigned long lastFire; };
AuxBtnState auxBtnState[2] = {};

void auxInit() {
    if (strcasecmp(cfg.aux_mode, "buttons") == 0) {
        pinMode(AUX_PIN_A, INPUT_PULLUP);
        pinMode(AUX_PIN_B, INPUT_PULLUP);
        S.printf("[AUX] button panel: A(38)='%s'  B(39)='%s'\n",
                 cfg.aux_btn[0].event, cfg.aux_btn[1].event);
    } else if (strcasecmp(cfg.aux_mode, "nfc2") == 0) {
        I2C_NFC2.begin(AUX_PIN_A, AUX_PIN_B, 100000);
        nfc2.begin();
        uint32_t fw2 = nfc2.getFirmwareVersion();
        if (fw2) {
            nfc2Ok = true;
            nfc2.SAMConfig();
            S.printf("[AUX] cylinder reader ready (fw=0x%08X)\n", (unsigned)fw2);
        } else {
            S.println("[AUX] nfc2 configured but no PN532 on 38/39");
        }
    }
}

// Called every loop pass. Sabotage keeps the AUX port dead too — a broken
// panel shouldn't take button presses or cylinder scans.
void auxService() {
    extern bool sabotaged;
    if (sabotaged) return;

    if (strcasecmp(cfg.aux_mode, "buttons") == 0) {
        const uint8_t pins[2] = { AUX_PIN_A, AUX_PIN_B };
        unsigned long now = millis();
        for (int b = 0; b < 2; b++) {
            bool raw = (digitalRead(pins[b]) == LOW);   // pressed = LOW (pullup)
            AuxBtnState &st = auxBtnState[b];
            if (raw != st.down && now - st.lastChange > 30) {
                st.lastChange = now;
                st.down = raw;
                if (raw && cfg.aux_btn[b].event[0] &&
                    now - st.lastFire > (unsigned long)cfg.button_cooldown_ms) {
                    st.lastFire = now;
                    S.printf("[AUX] button %c -> %s\n", 'A' + b, cfg.aux_btn[b].event);
                    sfge_lights::effectActivity();
                    buzzerTone(880, 60);
                    sfge::gmTriggerEvent(cfg.aux_btn[b].event,
                                         cfg.aux_btn[b].name[0] ? cfg.aux_btn[b].name
                                                                : "Panel button pressed",
                                         0, cfg.id);
                }
            }
        }
        return;
    }

    if (nfc2Ok) {
        static unsigned long lastCylPoll = 0;
        static char lastCylText[64] = "";
        static unsigned long lastCylTime = 0;
        if (millis() - lastCylPoll > 250) {
            lastCylPoll = millis();
            uint8_t uid2[7]; uint8_t len2;
            if (nfc2.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid2, &len2, 50)) {
                readNdefFrom(nfc2, auxNdefText, sizeof(auxNdefText));
                if (!auxNdefText[0]) return;
                if (strcmp(lastCylText, auxNdefText) == 0 && millis() - lastCylTime < 1500) return;
                strlcpy(lastCylText, auxNdefText, sizeof(lastCylText));
                lastCylTime = millis();
                if (strncasecmp(auxNdefText, "CODE_", 5) != 0) {
                    S.printf("[AUX] rejected '%s' (not a CODE_ cylinder)\n", auxNdefText);
                    buzzerTone(300, 150);
                    sfge_lights::effectFail();
                    return;
                }
                S.printf("[AUX] code cylinder '%s'\n", auxNdefText);
                buzzerTone(880, 60);
                sfge_lights::effectSignal();
                sfge::sendNfcScan(auxNdefText, auxNdefText, "cylinder");
                char eventId[40], eventName[40];
                snprintf(eventId, sizeof(eventId), "cyl:%s:%s", cfg.id, auxNdefText);
                snprintf(eventName, sizeof(eventName), "Code cylinder %s", auxNdefText);
                sfge::gmTriggerEvent(eventId, eventName, 0, cfg.name);
            }
        }
    }
}

enum PanelState { STATE_IDLE, STATE_CONNECTED, STATE_ACTIVE, STATE_MINIGAME, STATE_HACKED };
PanelState panelState = STATE_IDLE;
int connectCount = 0;
unsigned long lastActivity = 0;
String connectedCallsign = "";

// ═══════════════════════════════════════
//  ALARM STATE — tripped by consecutive hostile-faction failures
// ═══════════════════════════════════════
bool alarmActive = false;
unsigned long alarmSince = 0;
// GM countdown (drives the timer_visual red-shift)
volatile unsigned long timerEndsAt = 0;   // local millis() when it expires (0 = none)
volatile uint32_t timerTotalS = 0;
int  hostileFails = 0;
bool alarmBypass = false;          // hostile reset flow: 1-round Simon in progress
volatile bool alarmDirty = false;  // loop applies light/siren state changes

void setAlarm(bool on, const char *byFaction, const char *byCallsign) {
    if (on == alarmActive) return;
    alarmActive = on;
    alarmSince = millis();
    alarmDirty = true;
    char eventId[40];
    if (on) {
        hostileFails = 0;
        snprintf(eventId, sizeof(eventId), "alarm:%s", cfg.id);
        sfge::gmTriggerEvent(eventId, "Security alarm tripped", 2,
                             byCallsign ? byCallsign : "", byFaction ? byFaction : "");
        S.println("[ALARM] TRIPPED");
    } else {
        alarmBypass = false;
        snprintf(eventId, sizeof(eventId), "alarm_clear:%s", cfg.id);
        sfge::gmTriggerEvent(eventId, "Alarm cleared", 0, byCallsign ? byCallsign : "");
        S.println("[ALARM] cleared");
    }
}

// Faction relation of the requesting player to this prop:
// -1 friendly / 0 neutral / +1 hostile
int factionRelation(const char *pfac) {
    if (!pfac[0] || !cfg.faction[0] || strcasecmp(cfg.faction, "neutral") == 0) return 0;
    return (strcasecmp(cfg.faction, pfac) == 0) ? -1 : +1;
}

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

    // ── Proximity gate: reject when the datapad's signal is too weak ──
    int stationRssi = 0;
    if (!sfge_prox::check(&stationRssi)) {
        char body[64];
        snprintf(body, sizeof(body), "{\"error\":\"too_far\",\"rssi\":%d}", stationRssi);
        req->send(403, "application/json", body);
        S.printf("[PROX] rejected %s (rssi %d)\n", callsign, stationRssi);
        return;
    }

    JsonDocument resp;

    // Which slice action the menu offers depends on the configured minigame.
    const char* mgAction = (strcmp(cfg.minigame_type, "simon") == 0)
                               ? "start_simon" : "start_minigame";

    // Faction relation: slicing your own faction's gear is easier, enemy
    // gear is harder. Neutral props (like this cantina board by default)
    // treat everyone the same. -1 friendly / 0 neutral / +1 hostile.
    const char* pfac = reqDoc["player"]["faction"] | "";
    int facMod = factionRelation(pfac);

    // A SIGNAL JAMMER in the player's inventory makes a hostile prop treat
    // them as neutral for this interaction (the datapad consumes the item
    // when the response confirms it was used).
    bool usedJammer = false;
    if (facMod > 0) {
        for (const char *it : reqDoc["player"]["inventory"].as<JsonArray>()) {
            if (it && strcasecmp(it, "JAMMER") == 0) { facMod = 0; usedJammer = true; break; }
        }
    }

    // ── Alarm gate: while tripped, hostile players may only work the
    //    reset flow (reset_alarm + its bypass minigame result) ──
    if (alarmActive && facMod > 0 &&
        strcmp(action, "reset_alarm") != 0 &&
        !(alarmBypass && strcmp(action, "minigame_result") == 0)) {
        req->send(403, "application/json", "{\"error\":\"alarm\"}");
        return;
    }

    // ── Sabotage gate: the system is physically OFFLINE. Everything routes
    //    to the repair flow (Hot Wire) until someone fixes the wiring. ──
    if (sabotaged && strcmp(action, "repair") != 0 &&
        !(repairBypass && strcmp(action, "minigame_result") == 0)) {
        pendingFx = FX_FAIL;
        resp["type"] = "dialogue";
        JsonObject sp = resp["speaker"].to<JsonObject>();
        sp["name"] = cfg.name;
        JsonArray lines = resp["lines"].to<JsonArray>();
        JsonObject l1 = lines.add<JsonObject>();
        l1["text"] = "!! SYSTEM OFFLINE // JUNCTION BOX DAMAGED !!";
        l1["style"] = "system";
        JsonObject l2 = lines.add<JsonObject>();
        l2["text"] = "Scorched wiring hangs from an open service hatch.";
        l2["style"] = "narration";
        JsonArray choices = resp["choices"].to<JsonArray>();
        JsonObject c1 = choices.add<JsonObject>();
        c1["label"] = "Repair the wiring";
        c1["next_action"] = "repair";
        JsonObject c2 = choices.add<JsonObject>();
        c2["label"] = "[Disconnect]";
        c2["next_action"] = nullptr;
        String out; serializeJson(resp, out);
        req->send(200, "application/json", out);
        return;
    }

    // ── Keycard auth (no NFC on panels — the datapad sends the card tokens
    //    it has scanned as player.inventory) ──
    if (cfg.requires_auth && cfg.num_auth_cards > 0 && strcmp(action, "greet") == 0) {
        bool authed = false;
        for (const char *it : reqDoc["player"]["inventory"].as<JsonArray>()) {
            if (!it) continue;
            for (int a = 0; a < cfg.num_auth_cards; a++)
                if (strcasecmp(it, cfg.auth_cards[a]) == 0) { authed = true; break; }
            if (authed) break;
        }
        if (!authed) {
            resp["type"] = "dialogue";
            JsonObject sp = resp["speaker"].to<JsonObject>();
            sp["name"] = cfg.name;
            JsonArray lines = resp["lines"].to<JsonArray>();
            JsonObject l1 = lines.add<JsonObject>();
            l1["text"] = "AUTHORIZATION REQUIRED // KEYCARD NOT DETECTED";
            l1["style"] = "system";
            JsonArray choices = resp["choices"].to<JsonArray>();
            JsonObject c1 = choices.add<JsonObject>();
            c1["label"] = "[Disconnect]";
            c1["next_action"] = nullptr;
            String out; serializeJson(resp, out);
            req->send(200, "application/json", out);
            return;
        }
    }

    if (strcmp(action, "greet") == 0) {
        pendingFx = FX_ACTIVITY;
        resp["type"] = "dialogue";
        JsonObject speaker = resp["speaker"].to<JsonObject>();
        speaker["name"] = cfg.name;

        JsonArray lines = resp["lines"].to<JsonArray>();
        JsonArray choices = resp["choices"].to<JsonArray>();

        if (alarmActive) {
            // Friendly/neutral players see the alarm and can reset it
            // (hostile greets were already rejected above)
            resp["alarm"] = true;
            JsonObject la = lines.add<JsonObject>();
            la["text"] = "!! SECURITY ALARM ACTIVE !!";
            la["style"] = "system";
            JsonObject ca = choices.add<JsonObject>();
            ca["label"] = "Reset alarm (hold)";
            ca["next_action"] = "reset_alarm";
        }

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

            if (cfg.sabotage_enabled && facMod > 0) {
                // Hostile players can take the system offline entirely
                JsonObject cs = choices.add<JsonObject>();
                cs["label"] = "Sabotage junction box";
                cs["next_action"] = "sabotage";
            }

            JsonObject c3 = choices.add<JsonObject>();
            c3["label"] = "[Disconnect]";
            c3["next_action"] = nullptr;
        }
    }
    else if (strcmp(action, "sabotage") == 0) {
        resp["type"] = "dialogue";
        JsonObject sp = resp["speaker"].to<JsonObject>();
        sp["name"] = cfg.name;
        JsonArray lines = resp["lines"].to<JsonArray>();
        JsonArray choices = resp["choices"].to<JsonArray>();
        if (!cfg.sabotage_enabled || facMod <= 0) {
            JsonObject l1 = lines.add<JsonObject>();
            l1["text"] = "ACCESS DENIED";
            l1["style"] = "system";
        } else {
            setSabotaged(true, callsign);
            pendingFx = FX_FAIL;
            char eventId[40];
            snprintf(eventId, sizeof(eventId), "sabotaged:%s", cfg.id);
            resp["game_event"] = eventId;
            JsonObject l1 = lines.add<JsonObject>();
            l1["text"] = "You rip the primary harness loose. Sparks shower the floor.";
            l1["style"] = "narration";
            JsonObject l2 = lines.add<JsonObject>();
            l2["text"] = "!! SYSTEM OFFLINE !!";
            l2["style"] = "system";
        }
        JsonObject c1 = choices.add<JsonObject>();
        c1["label"] = "[Disconnect]";
        c1["next_action"] = nullptr;
    }
    else if (strcmp(action, "repair") == 0) {
        if (!sabotaged) {
            resp["type"] = "dialogue";
            JsonObject sp = resp["speaker"].to<JsonObject>();
            sp["name"] = cfg.name;
            JsonArray lines = resp["lines"].to<JsonArray>();
            JsonObject l1 = lines.add<JsonObject>();
            l1["text"] = "NO FAULT CONDITION PRESENT";
            l1["style"] = "system";
            JsonArray choices = resp["choices"].to<JsonArray>();
            JsonObject c1 = choices.add<JsonObject>();
            c1["label"] = "[Disconnect]";
            c1["next_action"] = nullptr;
        } else {
            repairBypass = true;
            panelState = STATE_MINIGAME;
            pendingFx = FX_ACTIVITY;
            resp["type"] = "minigame_start";
            resp["game"] = "wires";
            resp["wires"] = facMod < 0 ? 4 : 5;   // friendly techs get a simpler box
            resp["time_limit"] = 25;
        }
    }
    else if (strcmp(action, "start_minigame") == 0) {
        panelState = STATE_MINIGAME;
        pendingFx = FX_ACTIVITY;
        // Faction relation shifts the timing-bar difficulty (friendly -1, hostile +1)
        int diff = cfg.minigame_diff + facMod;
        if (diff < 1) diff = 1;
        if (diff > 5) diff = 5;
        resp["type"] = "minigame_start";
        resp["game"] = cfg.minigame_type;
        resp["difficulty"] = diff;
        resp["time_limit"] = 50 - diff * 5;
        if (usedJammer) resp["consumed_item"] = "JAMMER";

        JsonArray zones = resp["target_zones"].to<JsonArray>();
        JsonObject z1 = zones.add<JsonObject>();
        z1["start"] = 0.20; z1["end"] = 0.38; z1["points"] = 100;
        JsonObject z2 = zones.add<JsonObject>();
        z2["start"] = 0.60; z2["end"] = 0.78; z2["points"] = 80;
        if (diff >= 3) {
            JsonObject z3 = zones.add<JsonObject>();
            z3["start"] = 0.85; z3["end"] = 0.95; z3["points"] = 120;
        }
    }
    else if (strcmp(action, "start_simon") == 0) {
        // Simon Says (pattern lock) — rounds from config, adjusted by the
        // player's faction relation (friendly -1, hostile +2)
        panelState = STATE_MINIGAME;
        pendingFx = FX_ACTIVITY;
        int rounds = cfg.simon_rounds + (facMod < 0 ? -1 : facMod > 0 ? 2 : 0);
        if (rounds < 1) rounds = 1;
        resp["type"] = "minigame_start";
        resp["game"] = "simon";
        resp["rounds"] = rounds;
        if (usedJammer) resp["consumed_item"] = "JAMMER";
    }
    else if (strcmp(action, "minigame_result") == 0) {
        bool won = reqDoc["won"] | false;
        const char *result = reqDoc["result"] | (won ? "won" : "lost");
        resp["type"] = "minigame_result";
        resp["accepted"] = true;

        if (repairBypass) {
            // Hot Wire repair after a sabotage — restores the system, no slice
            repairBypass = false;
            if (won) {
                setSabotaged(false, callsign);
                pendingFx = FX_SUCCESS;
                char eventId[40];
                snprintf(eventId, sizeof(eventId), "prop_repaired:%s", cfg.id);
                resp["game_event"] = eventId;
                resp["message"] = "CIRCUIT RESTORED -- SYSTEM ONLINE";
                JsonObject rewards = resp["rewards"].to<JsonObject>();
                rewards["xp"] = 30;
            } else {
                pendingFx = FX_FAIL;
                resp["message"] = "STILL SHORTED -- TRY AGAIN";
                resp["retry"] = true;
            }
        }
        else if (alarmBypass) {
            // Hostile "bypass the klaxon" Simon — clears the alarm, no slice
            alarmBypass = false;
            if (won) {
                setAlarm(false, pfac, callsign);
                pendingFx = FX_SUCCESS;
                resp["message"] = "KLAXON BYPASSED -- ALARM SILENCED";
            } else {
                pendingFx = FX_FAIL;
                resp["message"] = "BYPASS FAILED -- ALARM STILL ACTIVE";
                resp["retry"] = true;
            }
        }
        else if (won) {
            panelState = STATE_HACKED;
            playerSliced = true;
            hostileFails = 0;
            pendingFx = FX_SUCCESS;
            // Announce the breach — missions can key off this event
            char eventId[40];
            snprintf(eventId, sizeof(eventId), "panel_sliced:%s", cfg.id);
            sfge::gmTriggerEvent(eventId, "Panel security breached", 0, callsign);
            resp["game_event"] = eventId;
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
            pendingFx = FX_FAIL;
            resp["message"] = (strcmp(result, "lost_signal") == 0)
                                  ? "LINK LOST -- INTRUSION LOGGED"
                                  : "SLICE FAILED -- SECURITY HOLDING";
            resp["retry"] = true;
            // Consecutive hostile failures (incl. lost signal) trip the alarm
            if (facMod > 0 && cfg.alarm_after_fails > 0) {
                hostileFails++;
                if (!alarmActive && hostileFails >= cfg.alarm_after_fails)
                    setAlarm(true, pfac, callsign);
            }
        }
        if (usedJammer) resp["consumed_item"] = "JAMMER";
    }
    else if (strcmp(action, "reset_alarm") == 0) {
        resp["type"] = "dialogue";
        JsonObject sp = resp["speaker"].to<JsonObject>();
        sp["name"] = cfg.name;
        JsonArray lines = resp["lines"].to<JsonArray>();
        JsonArray choices = resp["choices"].to<JsonArray>();
        if (!alarmActive) {
            JsonObject l1 = lines.add<JsonObject>();
            l1["text"] = "NO ALARM CONDITION PRESENT";
            l1["style"] = "system";
        } else if (facMod > 0) {
            // Hostile players must beat a 1-round Simon to silence it
            alarmBypass = true;
            panelState = STATE_MINIGAME;
            resp["type"] = "minigame_start";
            resp["game"] = "simon";
            resp["rounds"] = 1;
        } else {
            setAlarm(false, pfac, callsign);
            pendingFx = FX_ACTIVITY;
            JsonObject l1 = lines.add<JsonObject>();
            l1["text"] = "ALARM RESET // SECURITY GRID NORMALIZED";
            l1["style"] = "system";
        }
        if (strcmp(resp["type"] | "", "dialogue") == 0) {
            JsonObject c1 = choices.add<JsonObject>();
            c1["label"] = "[Disconnect]";
            c1["next_action"] = nullptr;
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
            sfge::gmTriggerEvent(eventId, "Extraction signal broadcast", 0, callsign);
            resp["game_event"] = eventId;
            pendingFx = FX_SIGNAL;

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

void onMeshMsg(const sfge::MeshHeader *hdr, const uint8_t *payload, int len) {
    switch (hdr->type) {
        case sfge::MSG_OVERRIDE: {
            if (len < (int)sizeof(sfge::MeshOverride)) return;
            const sfge::MeshOverride *o = (const sfge::MeshOverride *)payload;
            // Check if it's for us
            if (strcmp(o->target_prop, cfg.id) != 0 && strcmp(o->target_prop, "ALL") != 0) return;
            S.printf("[MESH] Override: %s %s = %s\n", o->target_prop, o->command, o->value);
            if (strcmp(o->command, "offline") == 0) panelDisabled = true;
            else if (strcmp(o->command, "online") == 0) panelDisabled = false;
            else if (strcmp(o->command, "difficulty") == 0) diffOverride = atoi(o->value);
            else if (strcmp(o->command, "sabotage") == 0) setSabotaged(true, "GM");
            else if (strcmp(o->command, "repair") == 0)   setSabotaged(false, "GM");
            else if (strcmp(o->command, "reset") == 0) { delay(200); ESP.restart(); }
            break;
        }
        case sfge::MSG_RESET: {
            S.println("[MESH] Reset received");
            delay(200);
            ESP.restart();
            break;
        }
        case sfge::MSG_NEW_GAME: {
            // Session wipe: reboot clears slice/alarm/session state
            S.println("[MESH] NEW GAME — resetting");
            delay(300 + (esp_random() % 400));   // stagger fleet reboots
            ESP.restart();
            break;
        }
        case sfge::MSG_SCENARIO: {
            if (len < (int)sizeof(sfge::MeshScenario)) return;
            const sfge::MeshScenario *sc = (const sfge::MeshScenario *)payload;
            if (sfge_scn::applyScenario(LittleFS, sc->id)) {
                delay(400 + (esp_random() % 500));
                ESP.restart();
            }
            break;
        }
        case sfge::MSG_EVENT: {
            // Victory finale: every prop in the room strobes
            if (len < (int)sizeof(sfge::MeshEvent)) return;
            const sfge::MeshEvent *e = (const sfge::MeshEvent *)payload;
            extern volatile bool pendingCelebrate;
            extern volatile uint32_t pendingCelebrateColor;
            if (sfge::isCelebrationEvent(e->event_id)) {
                pendingCelebrateColor = sfge::celebrationColor(e->payload);
                pendingCelebrate = true;
            }
            break;
        }
        case sfge::MSG_ALARM_CLEAR: {
            if (len < (int)sizeof(sfge::MeshAlarmClear)) return;
            const sfge::MeshAlarmClear *a = (const sfge::MeshAlarmClear *)payload;
            if (strcmp(a->target_prop, cfg.id) != 0 && strcmp(a->target_prop, "ALL") != 0) return;
            if (alarmActive) setAlarm(false, "", "GM");
            break;
        }
        case sfge::MSG_TIMER: {
            if (len < (int)sizeof(sfge::MeshTimer)) return;
            const sfge::MeshTimer *t = (const sfge::MeshTimer *)payload;
            extern volatile unsigned long timerEndsAt;
            extern volatile uint32_t timerTotalS;
            if (!t->active) { timerEndsAt = 0; }
            else {
                timerEndsAt = millis() + t->remaining_s * 1000UL;
                timerTotalS = t->total_s;
            }
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
    S.println("\n=== SFGE PANEL ===");

    // Init flash filesystem
    if (!LittleFS.begin(true)) {
        S.println("[FS] LittleFS mount failed!");
    } else {
        S.println("[FS] Flash filesystem ready");
    }

    // Check for SD card — provision to flash if found
    provisionFromSD();

    // TESTING: overwrite flash with embedded gameplay configs (no-op when
    // SFGE_WRITE_TEST_CONFIGS is commented out in sfge_test_configs.h)
    sfge_test::writeTestConfigs(LittleFS);

    // Scenario: marker lives OUTSIDE /SFGE because provisioning wipes it
    sfge_scn::markerPath = "/scn.txt";
    sfge_scn::begin(LittleFS);

    // Load config from flash
    loadConfig();

    // Buzzer + RGB strip
    pinMode(BUZZER_PIN, OUTPUT);
    if (!sfge_lights::load(LittleFS, sfge_scn::path("lights.txt")))
        if (!sfge_lights::load(LittleFS))   // scenario without lights.txt -> default
            sfge_lights::blank();           // no config at all -> strip OFF

    S.printf("Panel: %s (%s)\n", cfg.name, cfg.id);

    // NFC — same pins as the terminal's primary reader (SDA=14, SCL=13)
    // so one wiring harness fits every prop. Gated on hardware.has_nfc_reader:
    // a panel configured scan-free skips the probe (and its log noise)
    // entirely. No address pre-scan: a sleeping PN532 won't ACK a bare probe —
    // begin()/getFirmwareVersion() performs the wakeup. If nothing answers,
    // the panel keeps re-probing every 10 s from loop(), so fixing wiring or
    // mode switches is detected live — no reboot needed.
    if (cfg.has_nfc) {
        I2C_NFC.begin(14, 13, 100000);
        delay(50);
        panelNfcProbe(true);
    } else {
        S.println("NFC: disabled by config (scan-free panel)");
    }
    if (cfg.has_nfc && !nfcOk) {
        // Diagnostic: who (if anyone) is on the bus? A PN532 in I2C mode
        // ACKs at 0x24. NOTE: a PN532 with its mode switches on HSU/SPI
        // ignores I2C entirely and ALSO looks like an empty bus — so
        // "EMPTY" means either wiring/power OR the switches.
        S.println("NFC: NOT FOUND (SDA=14, SCL=13) — scanning bus:");
        int found = 0;
        for (uint8_t a = 1; a < 127; a++) {
            I2C_NFC.beginTransmission(a);
            if (I2C_NFC.endTransmission() == 0) {
                S.printf("NFC:   device ACK at 0x%02X%s\n", a,
                         a == 0x24 ? " (PN532!)" : "");
                found++;
            }
        }
        if (!found) S.println("NFC:   bus EMPTY — check power/GND/SDA14/SCL13 AND the"
                              " mode switches (I2C = SW1 ON, SW2 OFF); re-probing every 10 s");
        else        S.println("NFC:   device present but PN532 not responding — re-probing every 10 s");
    }

    // AUX port (buttons or second NFC on 38/39, per hardware.aux_mode)
    auxInit();

    // WiFi AP + STA (STA needed for ESPNOW). Both use MESH_CHANNEL.
    WiFi.onEvent(onWiFiEvent);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(cfg.ssid, NULL, MESH_CHANNEL);  // force mesh channel
    S.printf("WiFi AP: %s (ch %d)\n", cfg.ssid, MESH_CHANNEL);
    S.printf("IP: %s\n", WiFi.softAPIP().toString().c_str());

    setupServer();

    // ── ESPNOW Mesh ──
    sfge::meshInit(cfg.id, sfge::ROLE_PANEL, onMeshMsg);

    // Boot jingle (behavior.startup_song) — a cheery little power-up ditty
    // so the prop announces it's alive before anyone taps it
    if (cfg.startup_song) {
        static const struct { int f, on, gap; } SONG[] = {
            { 523, 90, 110 }, { 659, 90, 110 }, { 784, 90, 110 },
            { 659, 70,  90 }, { 784, 90, 110 }, {1047, 160, 200 },
            { 784, 70,  90 }, {1047, 220,   0 },
        };
        for (auto &n : SONG) { buzzerTone(n.f, n.on); delay(n.on + n.gap); }
    }

    S.println("Ready.");
}

// ═══════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════
volatile bool pendingCelebrate = false;
volatile uint32_t pendingCelebrateColor = 0;

void loop() {
    sfge::eventRetryService();   // re-fire gameplay events until datapads/GM confirm

    // Lights: idle pattern + any interaction effect queued by HTTP handlers
    applyPendingFx();

    // AUX port: momentary buttons or the code-cylinder reader
    auxService();

    // Victory celebration (game_complete / faction victory heard on the mesh)
    if (pendingCelebrate) {
        pendingCelebrate = false;
        if (!alarmActive) {
            sfge_lights::celebrate(12000, pendingCelebrateColor);
            buzzerTone(1047, 120); delay(140);   // C6-E6-G6 fanfare
            buzzerTone(1319, 120); delay(140);
            buzzerTone(1568, 350);
        }
    }

    // ── Alarm: strobe + siren while tripped; auto-clear on timeout ──
    if (alarmDirty) {
        alarmDirty = false;
        sfge_lights::setAlarm(alarmActive, cfg.alarm_siren_ms);
    }

    // ── Sabotage: broken-circuit lights until repaired ──
    if (sabDirty) {
        sabDirty = false;
        sfge_lights::setSabotage(sabotaged);
        if (sabotaged) { buzzerTone(200, 300); }
        else           { buzzerTone(880, 80); delay(90); buzzerTone(1320, 120); }
    }
    if (alarmActive) {
        static unsigned long lastSiren = 0;
        if (millis() - lastSiren >= (unsigned long)cfg.alarm_siren_ms * 2) {
            lastSiren = millis();
            buzzerTone(1400, cfg.alarm_siren_ms);
        }
        if (cfg.alarm_timeout_s > 0 &&
            millis() - alarmSince > (unsigned long)cfg.alarm_timeout_s * 1000UL) {
            setAlarm(false, "", "timeout");
            alarmDirty = false;
            sfge_lights::setAlarm(false);
        }
    }

    // ── GM countdown: red-shift the idle pattern as it runs out ──
    if (cfg.timer_visual) {
        static unsigned long lastTint = 0;
        if (millis() - lastTint > 1000) {
            lastTint = millis();
            uint8_t pct = 0;
            if (timerEndsAt && timerTotalS > 0) {
                long remainMs = (long)(timerEndsAt - millis());
                if (remainMs < 0) remainMs = 0;
                pct = (uint8_t)(70 - (70 * (remainMs / 1000UL)) / timerTotalS);
            }
            sfge_lights::setTint(pct);
        }
    }

    sfge_lights::update();

    static unsigned long lastPoll = 0;
    if (nfcOk && millis() - lastPoll > 500) {
        lastPoll = millis();
        pollNfc();
    }

    // No reader found yet (but config expects one)? Keep looking —
    // a hot-plugged or just-fixed PN532 comes up without a reboot.
    static unsigned long lastNfcProbe = 0;
    if (cfg.has_nfc && !nfcOk && millis() - lastNfcProbe > 10000) {
        lastNfcProbe = millis();
        panelNfcProbe(false);
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
        S.printf("[%s] %s | sliced=%d | alarm=%d | nfc=%s | card=%s\n",
                 cfg.id, st[panelState], playerSliced, alarmActive,
                 nfcOk ? "ok" : "--", cardPresent ? lastNfcUid : "--");
        sfge::sendPing(millis() / 1000);
        sfge::sendPropStatus(millis() / 1000, 0, alarmActive, "", sabotaged);
    }

    delay(10);
}
