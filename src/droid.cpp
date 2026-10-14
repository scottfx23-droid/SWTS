/*
 * SFGE Droid — Headless astromech prop
 * ESP32-S3 (same hardware family as the datapad, no display, no NFC)
 *
 * Hardware:
 *   SD card:    GPIO 15 = CS, 16 = MOSI, 17 = CLK, 18 = MISO  (HSPI)
 *   Buzzer:     GPIO  4
 *   (Optional) Eye LED on GPIO 5 — driven HIGH = on
 *
 * Behavior:
 *   - WiFi AP "SFGE_DROID_<ID>" on the mesh channel
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
#include <Adafruit_PN532.h>
#include "sfge_mesh.h"
#include "sfge_lights.h"         // addressable RGB strip (/SFGE/lights.txt)
#include "sfge_proximity.h"      // RSSI gate for interactions
#include "sfge_ota.h"            // firmware-from-SD updates
#include "sfge_scenario.h"       // GM-selected scenario -> content folder
#include "sfge_test_configs.h"   // boot-time test provisioning (see header to disable)

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
#define SERVO_PIN   5    // head servo signal (the old eye-LED pin; the eye
                         // is now an addressable pixel on the RGB strip)
#define NFC_SDA    14    // optional card reader — same NFC1 pins as every prop
#define NFC_SCL    13

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
//  DROID CONFIG (loaded from /SFGE/config.json)
// ═══════════════════════════════════════
struct DroidConfig {
    char id[16]         = "DROID_R5D8";
    char name[40]       = "R5-D8";
    char ssid[24]       = "SFGE_DROID_R5";
    char faction[12]    = "rebel";
    bool has_nfc        = false;   // probe NFC1 (SDA14/SCL13) at boot
    // Restraining bolt: an NFC tag parked on the SECOND reader (38/39).
    // While it's detected the droid is restrained and refuses datacards;
    // physically pulling the bolt off frees it (fires bolt_removed:<id>).
    bool bolt_enabled   = false;
    int  bolt_grace_ms  = 3000;    // reader must be empty this long = removed
    bool startup_song   = true;    // astromech wake-up chatter (opt-out)
    bool boot_tune      = false;   // play a melodic tune instead of chatter
    int  boot_shakes    = 0;       // emphatic head shakes after the boot sound
    bool has_minigame   = true;    // card-less default: core must be purged
    char minigame_type[16] = "purge";
    int  purge_targets  = 12;   // corrupted blocks to clear
    int  purge_time_s   = 35;   // time limit in seconds
    // Mood system
    bool mood_enabled   = false;
    int  mood_decay_s   = 120;
    uint32_t mood_hostile  = 0xFF2000;
    uint32_t mood_neutral  = 0xFFAA00;
    uint32_t mood_friendly = 0x2060FF;
    int  eye_pixel      = 0;    // which strip pixel is the eye (-1 = none)
    // Head servo
    bool servo_enabled  = false;
    int  servo_pin      = SERVO_PIN;
    int  servo_min_us   = 600;
    int  servo_max_us   = 2400;
    int  servo_min_deg  = 25;   // travel limits so the head can't overrotate
    int  servo_max_deg  = 155;
    int  servo_idle_s   = 8;    // average seconds between idle head turns
};
DroidConfig cfg;

// ── Sabotage state: droid is OFFLINE until someone wins Hot Wire ──
bool sabotaged = false;
bool repairBypass = false;
volatile bool sabDirty = false;

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

// ═══════════════════════════════════════
//  HEAD SERVO — LEDC PWM, 50 Hz. Idle wander plus reaction moves:
//  a look-at-you on greet, a happy wiggle on wins, a slow shake on fails.
// ═══════════════════════════════════════
#define SERVO_LEDC_CH 2
float headDeg = 90, headTarget = 90;
unsigned long nextHeadMove = 0;
int   headWiggles = 0;         // remaining wiggle half-cycles
float headWiggleBase = 90;
bool  headShake = false;       // wiggle style: fast happy vs slow shake

void servoWriteDeg(float deg) {
    if (deg < cfg.servo_min_deg) deg = cfg.servo_min_deg;
    if (deg > cfg.servo_max_deg) deg = cfg.servo_max_deg;
    uint32_t us = cfg.servo_min_us +
                  (uint32_t)((deg / 180.0f) * (cfg.servo_max_us - cfg.servo_min_us));
    ledcWrite(SERVO_LEDC_CH, (uint32_t)((uint64_t)us * 65536 / 20000));   // 20 ms frame
}

void servoInit() {
    if (!cfg.servo_enabled) return;
    ledcSetup(SERVO_LEDC_CH, 50, 16);
    ledcAttachPin(cfg.servo_pin, SERVO_LEDC_CH);
    headDeg = headTarget = 90;
    servoWriteDeg(headDeg);
    S.printf("[SERVO] head on GPIO %d (%d-%d deg)\n",
             cfg.servo_pin, cfg.servo_min_deg, cfg.servo_max_deg);
}

// Queue a reaction: happy wiggle (fast) or disapproving shake (slow)
void headReact(bool happy) {
    if (!cfg.servo_enabled) return;
    headWiggleBase = 90;
    headTarget = 90;
    headWiggles = happy ? 6 : 4;
    headShake = !happy;
}

void servoService() {
    if (!cfg.servo_enabled) return;
    static unsigned long lastStep = 0;
    unsigned long now = millis();
    if (now - lastStep < 20) return;   // 50 Hz update
    lastStep = now;

    if (headWiggles > 0) {
        // Reaction wiggle: snap between offsets around center
        static unsigned long lastWiggle = 0;
        int period = headShake ? 420 : 160;
        if (now - lastWiggle > (unsigned long)period) {
            lastWiggle = now;
            int amp = headShake ? 22 : 14;
            headTarget = headWiggleBase + ((headWiggles % 2) ? amp : -amp);
            headWiggles--;
            if (headWiggles == 0) headTarget = headWiggleBase;
        }
    } else if (now > nextHeadMove) {
        // Idle wander: occasional slow look-around
        nextHeadMove = now + (unsigned long)cfg.servo_idle_s * 500UL +
                       esp_random() % ((unsigned long)cfg.servo_idle_s * 1000UL);
        headTarget = cfg.servo_min_deg +
                     (esp_random() % (cfg.servo_max_deg - cfg.servo_min_deg));
    }

    // Ease toward the target (speed scales with distance, capped)
    float d = headTarget - headDeg;
    float step = d * 0.12f;
    if (step > 3.0f) step = 3.0f;
    if (step < -3.0f) step = -3.0f;
    if (fabsf(d) > 0.5f) {
        headDeg += step;
        servoWriteDeg(headDeg);
    }
}

// ═══════════════════════════════════════
//  MOOD — one signed int, persisted to /SFGE/droid_state.json
//  Interactions move it; it decays toward 0. Drives the eye color
//  (strip pixel 0), the greeting tone, and Core Purge difficulty.
// ═══════════════════════════════════════
int  mood = 0;
bool moodDirty = false;
unsigned long moodLastDecay = 0;
bool sdMounted = false;

int moodBand() {   // -1 hostile, 0 neutral, +1 friendly
    if (mood <= -2) return -1;
    if (mood >= 2)  return +1;
    return 0;
}

void moodApplyEye() {
    if (!cfg.mood_enabled || cfg.eye_pixel < 0) return;
    uint32_t c = (moodBand() < 0) ? cfg.mood_hostile
               : (moodBand() > 0) ? cfg.mood_friendly : cfg.mood_neutral;
    sfge_lights::setPixelOverride(cfg.eye_pixel, c);
}

void moodAdd(int delta) {
    if (!cfg.mood_enabled) return;
    mood += delta;
    if (mood > 5)  mood = 5;
    if (mood < -5) mood = -5;
    moodDirty = true;
    S.printf("[MOOD] %+d -> %d\n", delta, mood);
}

void moodSave() {
    if (!sdMounted) return;
    File f = SD.open("/SFGE/droid_state.json", FILE_WRITE);
    if (!f) return;
    JsonDocument doc;
    doc["mood"] = mood;
    serializeJson(doc, f);
    f.close();
}

void moodLoad() {
    File f = SD.open("/SFGE/droid_state.json", FILE_READ);
    if (!f) return;
    JsonDocument doc;
    if (!deserializeJson(doc, f)) mood = doc["mood"] | 0;
    f.close();
    if (mood > 5) mood = 5;
    if (mood < -5) mood = -5;
}

bool applyConfigDoc(JsonDocument &doc);

bool loadConfig() {
    File f = SD.open(sfge_scn::path("config.json"), FILE_READ);
    if (!f) { S.println("[CFG] no /SFGE/config.json, using defaults"); return false; }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) { S.printf("[CFG] parse error: %s\n", err.c_str()); return false; }
    return applyConfigDoc(doc);
}

bool applyConfigDoc(JsonDocument &doc) {
    strlcpy(cfg.id,      doc["prop"]["id"]      | cfg.id,      sizeof(cfg.id));
    strlcpy(cfg.name,    doc["prop"]["name"]    | cfg.name,    sizeof(cfg.name));
    strlcpy(cfg.ssid,    doc["prop"]["ssid"]    | cfg.ssid,    sizeof(cfg.ssid));
    strlcpy(cfg.faction, doc["prop"]["faction"] | cfg.faction, sizeof(cfg.faction));

    cfg.has_nfc = doc["hardware"]["has_nfc_reader"] | cfg.has_nfc;
    cfg.startup_song = doc["behavior"]["startup_song"] | cfg.startup_song;
    cfg.boot_tune    = doc["behavior"]["startup_tune"] | cfg.boot_tune;
    cfg.boot_shakes  = doc["behavior"]["startup_head_shakes"] | cfg.boot_shakes;
    cfg.bolt_enabled  = doc["behavior"]["restraining_bolt"]["enabled"]  | cfg.bolt_enabled;
    cfg.bolt_grace_ms = doc["behavior"]["restraining_bolt"]["grace_ms"] | cfg.bolt_grace_ms;

    // NOTE: as<const char*>(), not `| nullptr` — the latter always yields null
    const char* mg = doc["behavior"]["minigame_default"].as<const char*>();
    cfg.has_minigame = (mg != nullptr && mg[0] != '\0');
    if (cfg.has_minigame) strlcpy(cfg.minigame_type, mg, sizeof(cfg.minigame_type));
    cfg.purge_targets = doc["behavior"]["purge_targets"] | cfg.purge_targets;
    cfg.purge_time_s  = doc["behavior"]["purge_time_s"]  | cfg.purge_time_s;

    cfg.mood_enabled = doc["behavior"]["mood_enabled"] | cfg.mood_enabled;
    cfg.mood_decay_s = doc["behavior"]["mood_decay_s"] | cfg.mood_decay_s;
    const char *mc;
    mc = doc["behavior"]["mood_colors"]["hostile"].as<const char*>();
    if (mc) cfg.mood_hostile = (uint32_t)strtoul(mc, nullptr, 16);
    mc = doc["behavior"]["mood_colors"]["neutral"].as<const char*>();
    if (mc) cfg.mood_neutral = (uint32_t)strtoul(mc, nullptr, 16);
    mc = doc["behavior"]["mood_colors"]["friendly"].as<const char*>();
    if (mc) cfg.mood_friendly = (uint32_t)strtoul(mc, nullptr, 16);
    cfg.eye_pixel = doc["behavior"]["eye_pixel"] | cfg.eye_pixel;

    cfg.servo_enabled = doc["behavior"]["servo"]["enabled"]  | cfg.servo_enabled;
    cfg.servo_pin     = doc["behavior"]["servo"]["pin"]      | cfg.servo_pin;
    cfg.servo_min_us  = doc["behavior"]["servo"]["min_us"]   | cfg.servo_min_us;
    cfg.servo_max_us  = doc["behavior"]["servo"]["max_us"]   | cfg.servo_max_us;
    cfg.servo_min_deg = doc["behavior"]["servo"]["min_deg"]  | cfg.servo_min_deg;
    cfg.servo_max_deg = doc["behavior"]["servo"]["max_deg"]  | cfg.servo_max_deg;
    cfg.servo_idle_s  = doc["behavior"]["servo"]["idle_s"]   | cfg.servo_idle_s;

    sfge_prox::loadConfig(doc);

    S.printf("[CFG] %s id=%s ssid=%s faction=%s minigame=%s\n",
             cfg.name, cfg.id, cfg.ssid, cfg.faction,
             cfg.has_minigame ? cfg.minigame_type : "none");
    return true;
}

// ═══════════════════════════════════════
//  NFC — optional card reader on NFC1 (SDA=14, SCL=13), the standard prop
//  pins. Feed the droid a card: it chirps, wiggles, cheers up a little and
//  broadcasts nfc:<DROID_ID>:<TOKEN> — a mission step / comm trigger like
//  any terminal scan. Enable with hardware.has_nfc_reader; a missing
//  reader just logs and the droid runs card-less as before.
// ═══════════════════════════════════════
TwoWire I2C_NFC = TwoWire(1);
Adafruit_PN532 nfc(-1, -1, &I2C_NFC);
bool nfcOk = false;
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
            uint8_t payLen = buf[r + 2];
            int typeOff = r + 3;
            if (typeLen == 1 && typeOff < pos && buf[typeOff] == 0x54) {
                int payOff = typeOff + typeLen;
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

// ── Restraining bolt (second reader, SDA=38/SCL=39) ──
// The bolt prop holds an NFC tag against the reader. Seen recently = bolted.
TwoWire I2C_NFC2 = TwoWire(0);
Adafruit_PN532 nfc2(-1, -1, &I2C_NFC2);
bool nfc2Ok = false;
int  boltState = -1;               // -1 unknown, 1 bolted, 0 free
unsigned long lastBoltSeen = 0;
unsigned long boltBootTime = 0;

// Called every loop pass: poll the bolt reader and run the state machine.
void boltService() {
    if (!nfc2Ok) return;
    unsigned long now = millis();

    static unsigned long lastBoltPoll = 0;
    if (now - lastBoltPoll > 400) {
        lastBoltPoll = now;
        uint8_t uid[7]; uint8_t len;
        if (nfc2.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 40))
            lastBoltSeen = now;
    }

    int newState;
    if (lastBoltSeen && now - lastBoltSeen <= (unsigned long)cfg.bolt_grace_ms)
        newState = 1;
    else if (lastBoltSeen || now - boltBootTime > (unsigned long)cfg.bolt_grace_ms * 2)
        newState = 0;
    else
        return;   // still deciding the initial state

    if (newState == boltState) return;
    int prev = boltState;
    boltState = newState;
    if (prev == -1) {   // initial determination — no fanfare, no events
        S.printf("[BOLT] initial state: %s\n", boltState ? "BOLTED" : "free");
        return;
    }

    char eventId[40];
    if (boltState == 0) {
        // Freed! The droid is thrilled about it.
        S.println("[BOLT] REMOVED — unit unrestrained");
        snprintf(eventId, sizeof(eventId), "bolt_removed:%s", cfg.id);
        sfge::gmTriggerEvent(eventId, "Restraining bolt removed", 1, cfg.name);
        if (cfg.mood_enabled) { moodAdd(+2); moodApplyEye(); }
        sfge_lights::effectSuccess();
        astromechChirp();
        delay(100);
        for (int f = 900; f <= 2100; f += 300) { buzzerTone(f, 45); delay(55); }
        headReact(true);
    } else {
        // Re-bolted — resigned electronic sigh
        S.println("[BOLT] ATTACHED — unit restrained");
        snprintf(eventId, sizeof(eventId), "bolt_attached:%s", cfg.id);
        sfge::gmTriggerEvent(eventId, "Restraining bolt attached", 0, cfg.name);
        if (cfg.mood_enabled) { moodAdd(-1); moodApplyEye(); }
        buzzerTone(320, 200); delay(220); buzzerTone(200, 320);
        headReact(false);
    }
}

void onDroidCardScanned() {
    static char lastCard[64] = "";
    static unsigned long lastCardTime = 0;
    if (strcmp(lastCard, ndefText) == 0 && millis() - lastCardTime < 1500) return;
    strlcpy(lastCard, ndefText, sizeof(lastCard));
    lastCardTime = millis();

    // Restrained droids refuse uploads — pull the bolt off first
    if (cfg.bolt_enabled && boltState == 1) {
        S.printf("[BOLT] restrained — refusing card '%s'\n", ndefText);
        buzzerTone(260, 220); delay(250); buzzerTone(170, 320);   // muzzled womp
        sfge_lights::effectFail();
        headReact(false);
        return;
    }

    S.printf("[NFC] card '%s' fed to the droid\n", ndefText);
    astromechChirp();
    sfge_lights::effectActivity();
    headReact(true);
    if (cfg.mood_enabled) { moodAdd(+1); moodApplyEye(); }   // he likes snacks

    sfge::sendNfcScan(ndefText, ndefText, "droid");
    char eventId[48], eventName[40];
    snprintf(eventId, sizeof(eventId), "nfc:%s:%s", cfg.id, ndefText);
    snprintf(eventName, sizeof(eventName), "Card fed to %s", cfg.name);
    sfge::gmTriggerEvent(eventId, eventName, 0, cfg.id);
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
    feat["has_nfc"]      = nfcOk;
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
    const char *pfac     = reqDoc["player"]["faction"] | "";
    strlcpy(connectedCallsign, callsign, sizeof(connectedCallsign));
    S.printf("[HTTP] %s by %s\n", action, callsign);

    // ── Proximity gate ──
    int stationRssi = 0;
    if (!sfge_prox::check(&stationRssi)) {
        char body[64];
        snprintf(body, sizeof(body), "{\"error\":\"too_far\",\"rssi\":%d}", stationRssi);
        req->send(403, "application/json", body);
        return;
    }

    // Faction relation: -1 friendly / 0 neutral / +1 hostile
    int frel = 0;
    if (pfac[0] && cfg.faction[0] && strcasecmp(cfg.faction, "neutral") != 0)
        frel = (strcasecmp(cfg.faction, pfac) == 0) ? -1 : +1;
    // A SIGNAL JAMMER makes a hostile player read as neutral (consumed)
    bool usedJammer = false;
    if (frel > 0) {
        for (const char *it : reqDoc["player"]["inventory"].as<JsonArray>()) {
            if (it && strcasecmp(it, "JAMMER") == 0) { frel = 0; usedJammer = true; break; }
        }
    }

    // ── Sabotage gate: a sabotaged droid only talks about getting fixed ──
    if (sabotaged && strcmp(action, "repair") != 0 &&
        !(repairBypass && strcmp(action, "minigame_result") == 0)) {
        JsonDocument sb;
        sb["type"] = "dialogue";
        JsonObject sp = sb["speaker"].to<JsonObject>();
        sp["name"] = cfg.name;
        JsonArray lines = sb["lines"].to<JsonArray>();
        JsonObject l1 = lines.add<JsonObject>();
        l1["text"] = "bzzt... zzt... (A service panel hangs open. Loose wires spark weakly.)";
        l1["style"] = "narration";
        JsonArray chs = sb["choices"].to<JsonArray>();
        JsonObject c1 = chs.add<JsonObject>();
        c1["label"] = "Repair the wiring";
        c1["next_action"] = "repair";
        JsonObject c2 = chs.add<JsonObject>();
        c2["label"] = "[Disconnect]";
        c2["next_action"] = nullptr;
        String out; serializeJson(sb, out);
        req->send(200, "application/json", out);
        return;
    }
    if (strcmp(action, "repair") == 0) {
        JsonDocument rp;
        if (!sabotaged) {
            rp["type"] = "dialogue";
            JsonObject sp = rp["speaker"].to<JsonObject>();
            sp["name"] = cfg.name;
            JsonArray lines = rp["lines"].to<JsonArray>();
            JsonObject l1 = lines.add<JsonObject>();
            l1["text"] = "BWEEP? (Translation: I'm fine. Hands off.)";
            l1["style"] = "droid";
            JsonArray chs = rp["choices"].to<JsonArray>();
            JsonObject c1 = chs.add<JsonObject>();
            c1["label"] = "[Disconnect]";
            c1["next_action"] = nullptr;
        } else {
            repairBypass = true;
            rp["type"] = "minigame_start";
            rp["game"] = "wires";
            rp["wires"] = frel < 0 ? 4 : 5;
            rp["time_limit"] = 25;
        }
        String out; serializeJson(rp, out);
        req->send(200, "application/json", out);
        return;
    }

    if (strcmp(action, "apologize") == 0) {
        // Costs 50 CR (the datapad deducts on seeing "cost"), soothes the droid
        moodAdd(+2);
        moodApplyEye();
        astromechChirp();
        JsonDocument ap;
        ap["type"] = "dialogue";
        ap["cost"] = 50;
        JsonObject sp = ap["speaker"].to<JsonObject>();
        sp["name"] = cfg.name;
        JsonArray lines = ap["lines"].to<JsonArray>();
        JsonObject l1 = lines.add<JsonObject>();
        l1["text"] = "BWOOP... beep. (Translation: Fine. Apology accepted. Credits appreciated.)";
        l1["style"] = "droid";
        JsonArray chs = ap["choices"].to<JsonArray>();
        JsonObject c1 = chs.add<JsonObject>();
        c1["label"] = "[Disconnect]";
        c1["next_action"] = nullptr;
        String out; serializeJson(ap, out);
        req->send(200, "application/json", out);
        return;
    }

    if (strcmp(action, "start_purge") == 0) {
        // A deeply annoyed droid refuses to open his core at all
        if (cfg.mood_enabled && mood <= -4) {
            JsonDocument rf;
            rf["type"] = "dialogue";
            JsonObject sp = rf["speaker"].to<JsonObject>();
            sp["name"] = cfg.name;
            JsonArray lines = rf["lines"].to<JsonArray>();
            JsonObject l1 = lines.add<JsonObject>();
            l1["text"] = "BZZZT! WHIRR-CLUNK!! (Translation: Absolutely not. You lot have done enough damage.)";
            l1["style"] = "droid";
            JsonArray chs = rf["choices"].to<JsonArray>();
            JsonObject c1 = chs.add<JsonObject>();
            c1["label"] = "Apologize (50 CR)";
            c1["next_action"] = "apologize";
            JsonObject c2 = chs.add<JsonObject>();
            c2["label"] = "[Disconnect]";
            c2["next_action"] = nullptr;
            String out; serializeJson(rf, out);
            req->send(200, "application/json", out);
            return;
        }

        // ── Core Purge minigame start — datapad runs the game on its screen ──
        // Faction relation tunes it (own faction gentler, enemy harder), then
        // mood shifts targets: a happy droid opens up, an angry one fights back.
        int targets = cfg.purge_targets;
        int timeS   = cfg.purge_time_s;
        if (frel < 0)      { targets -= 4; timeS += 10; }
        else if (frel > 0) { targets += 4; timeS -= 5;  }
        if (cfg.mood_enabled) targets -= mood;
        if (targets < 4)  targets = 4;
        if (timeS < 10)   timeS = 10;

        pendingFx = FX_ACTIVITY;
        astromechChirp();
        JsonDocument mg;
        mg["type"]       = "minigame_start";
        mg["game"]       = "purge";
        mg["targets"]    = targets;
        mg["time_limit"] = timeS;
        if (usedJammer) mg["consumed_item"] = "JAMMER";
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
        if (repairBypass) {
            // Hot Wire repair after a sabotage — fixing him cheers him up
            repairBypass = false;
            if (won) {
                setSabotaged(false, callsign);
                moodAdd(+2);
                moodApplyEye();
                pendingFx = FX_SUCCESS;
                char eventId[40];
                snprintf(eventId, sizeof(eventId), "prop_repaired:%s", cfg.id);
                mr["game_event"] = eventId;
                mr["message"] = "SERVOS RESPONDING -- DROID BACK ONLINE";
                JsonObject rewards = mr["rewards"].to<JsonObject>();
                rewards["xp"] = 30;
            } else {
                pendingFx = FX_FAIL;
                mr["message"] = "STILL SHORTED -- TRY AGAIN";
                mr["retry"] = true;
            }
            String out; serializeJson(mr, out);
            req->send(200, "application/json", out);
            return;
        }
        // Mood: hostile players wiping his core hurts; any failure annoys
        if (won && frel > 0) moodAdd(-3);
        else if (!won)       moodAdd(-1);
        moodApplyEye();
        if (won) {
            coreSliced = true;
            pendingFx = FX_SUCCESS;
            buzzHandoff();
            mr["message"]  = "MEMORY CORE STABILIZED -- UPLOAD CHANNEL OPEN";
            mr["unlocked"] = true;
            char eventId[40];
            snprintf(eventId, sizeof(eventId), "droid_slice:%s", cfg.id);
            sfge::gmTriggerEvent(eventId, "Memory core sliced on R5-D8", /*severity*/ 0, callsign);
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
        moodAdd(+2);
        moodApplyEye();
        pendingFx = FX_SIGNAL;
        astromechChirp();
        delay(120);
        buzzHandoff();

        // Broadcast to GM dashboard + any subscribed comms triggers
        char eventId[40];
        snprintf(eventId, sizeof(eventId), "droid_handoff:%s", cfg.id);
        sfge::gmTriggerEvent(eventId, "Intel delivered to R5-D8", /*severity*/ 0, callsign);
        // Mission hook — carried in the HTTP response so the delivering
        // datapad advances even if it misses the mesh broadcast
        resp["game_event"] = eventId;

        // Push a comm to the player so they get visual confirmation
        char commId[20];
        snprintf(commId, sizeof(commId), "r5_thanks_%lu", millis() % 100000);
        sfge::gmPushComm(commId, callsign, cfg.name, "INTEL RECEIVED",
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

    // Friendly faces cheer him up (rate-limited so it can't be farmed)
    static unsigned long lastMoodGreet = 0;
    if (cfg.mood_enabled && frel < 0 && millis() - lastMoodGreet > 60000) {
        lastMoodGreet = millis();
        moodAdd(+1);
        moodApplyEye();
    }

    // Greeting tone follows his mood: try dialogue/greet_<band>.json on the
    // card, else use the built-in line for that band
    {
        const char *fn = !cfg.mood_enabled ? nullptr
                       : moodBand() < 0 ? sfge_scn::path("dialogue/greet_hostile.json")
                       : moodBand() > 0 ? sfge_scn::path("dialogue/greet_friendly.json")
                                        : sfge_scn::path("dialogue/greet_neutral.json");
        bool fromFile = false;
        if (fn && sdMounted) {
            File gf = SD.open(fn, FILE_READ);
            if (gf) {
                JsonDocument gd;
                if (!deserializeJson(gd, gf)) {
                    for (JsonObject ln : gd["lines"].as<JsonArray>()) {
                        JsonObject l = lines.add<JsonObject>();
                        l["text"]  = ln["text"]  | "";
                        l["style"] = ln["style"] | "droid";
                        fromFile = true;
                    }
                }
                gf.close();
            }
        }
        if (!fromFile) {
            JsonObject l1 = lines.add<JsonObject>();
            l1["text"] = !cfg.mood_enabled ? "BEEP-BWEEP-WHIRR-CLICK. (Translation: About time you showed up.)"
                       : moodBand() < 0 ? "GRZZT. BWOP. (Translation: Oh. It's you people again. What.)"
                       : moodBand() > 0 ? "BWEEE-DEET-DEET! (Translation: Hey! Good to see a friendly face!)"
                                        : "BEEP-BWEEP-WHIRR-CLICK. (Translation: About time you showed up.)";
            l1["style"] = "droid";
        }
    }

    // Restrained? Say so — the bolt is the visible puzzle
    if (cfg.bolt_enabled && boltState == 1) {
        JsonObject lb = lines.add<JsonObject>();
        lb["text"] = "A restraining bolt is clamped to its chassis. The droid's "
                     "eye keeps flicking toward it, then at you. Meaningfully.";
        lb["style"] = "narration";
        JsonObject lb2 = lines.add<JsonObject>();
        lb2["text"] = "RESTRAINING BOLT ENGAGED // DATA UPLINK LOCKED";
        lb2["style"] = "system";
    }

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
        if (cfg.mood_enabled && mood <= -4 && !coreSliced) {
            // Too angry to cooperate — only an apology moves things forward
            JsonObject c1 = choices.add<JsonObject>();
            c1["label"]       = "Apologize (50 CR)";
            c1["next_action"] = "apologize";
        } else if (cfg.has_minigame && !coreSliced) {
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
void onMeshMsg(const sfge::MeshHeader *hdr, const uint8_t *payload, int len) {
    if (hdr->type == sfge::MSG_SYNC_REQUEST) {
        sfge::sendPing(millis() / 1000);
        return;
    }
    if (hdr->type == sfge::MSG_RESET) {
        S.println("[MESH] RESET — restarting");
        delay(300);
        ESP.restart();
    }
    if (hdr->type == sfge::MSG_NEW_GAME) {
        // Session wipe: forget the mood and reboot fresh
        S.println("[MESH] NEW GAME — resetting");
        if (sdMounted) SD.remove("/SFGE/droid_state.json");
        delay(300 + (esp_random() % 400));
        ESP.restart();
    }
    if (hdr->type == sfge::MSG_OVERRIDE && len >= (int)sizeof(sfge::MeshOverride)) {
        const sfge::MeshOverride *o = (const sfge::MeshOverride *)payload;
        if (strcmp(o->target_prop, cfg.id) == 0 || strcmp(o->target_prop, "ALL") == 0) {
            if (strcmp(o->command, "sabotage") == 0)    setSabotaged(true, "GM");
            else if (strcmp(o->command, "repair") == 0) setSabotaged(false, "GM");
            else if (strcmp(o->command, "reset") == 0)  { delay(200); ESP.restart(); }
        }
        return;
    }
    if (hdr->type == sfge::MSG_SCENARIO && len >= (int)sizeof(sfge::MeshScenario)) {
        const sfge::MeshScenario *sc = (const sfge::MeshScenario *)payload;
        if (sdMounted && sfge_scn::applyScenario(SD, sc->id)) {
            delay(400 + (esp_random() % 500));
            ESP.restart();
        }
    }
    if (hdr->type == sfge::MSG_EVENT && len >= (int)sizeof(sfge::MeshEvent)) {
        // Victory finale: every prop in the room strobes
        const sfge::MeshEvent *e = (const sfge::MeshEvent *)payload;
        extern volatile bool pendingCelebrate;
        extern volatile uint32_t pendingCelebrateColor;
        if (sfge::isCelebrationEvent(e->event_id)) {
            pendingCelebrateColor = sfge::celebrationColor(e->payload);
            pendingCelebrate = true;
        }
    }
}

// ═══════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════
SPIClass sdSPI(HSPI);

// Servo + both NFC readers, driven by whatever config is active (card or
// forced embedded demo). Shared by the SD and no-SD boot paths.
void initDroidPeripherals() {
    servoInit();

    // Optional card reader on the standard NFC1 pins
    if (cfg.has_nfc) {
        I2C_NFC.begin(NFC_SDA, NFC_SCL, 100000);
        nfc.begin();
        uint32_t fw = nfc.getFirmwareVersion();
        if (fw) {
            nfcOk = true;
            nfc.SAMConfig();
            S.printf("[NFC] card reader ready (fw=0x%08X)\n", (unsigned)fw);
        } else {
            S.println("[NFC] has_nfc_reader set but no PN532 on 14/13 — card-less");
        }
    }

    // Restraining bolt reader on the AUX pins (38/39)
    if (cfg.bolt_enabled) {
        I2C_NFC2.begin(38, 39, 100000);
        nfc2.begin();
        uint32_t fw2 = nfc2.getFirmwareVersion();
        if (fw2) {
            nfc2Ok = true;
            nfc2.SAMConfig();
            boltBootTime = millis();
            S.printf("[BOLT] bolt reader ready (fw=0x%08X)\n", (unsigned)fw2);
        } else {
            S.println("[BOLT] enabled but no PN532 on 38/39 — droid runs free");
        }
    }
}

void setup() {
    S.begin(115200);
    delay(200);
    S.println("\n================================");
    S.println("  SFGE DROID — starting");
    S.println("================================");

    randomSeed(esp_random());

    pinMode(BUZZER_PIN, OUTPUT);

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

    // SD optional; falls back to defaults if no card. Mount is retried —
    // warm reboots can leave the card mid-state for one begin() attempt.
    sdSPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
    bool sdOk = false;
    for (int t = 0; t < 3 && !sdOk; t++) {
        if (t) { SD.end(); delay(250); S.printf("[SD] mount retry %d...\n", t); }
        sdOk = SD.begin(SD_CS, sdSPI, 4000000);
    }
    if (sdOk) {
        S.printf("[SD] Mounted %lluMB\n", SD.cardSize() / (1024 * 1024));
        // Firmware update staged on the card? Flash it before anything else.
        sfge_ota::checkAndFlash(SD);
        // TESTING: write embedded gameplay configs to the card (no-op when
        // SFGE_WRITE_TEST_CONFIGS is commented out in sfge_test_configs.h)
        sfge_test::writeTestConfigs(SD);
        sfge_scn::begin(SD);     // GM-selected scenario -> content folder
        loadConfig();
        if (!sfge_lights::load(SD, sfge_scn::path("lights.txt")))
            if (!sfge_lights::load(SD))   // scenario without lights.txt -> default
                sfge_lights::blank();     // no config at all -> strip OFF
        sdMounted = true;
        moodLoad();
        moodApplyEye();
        S.printf("[MOOD] enabled=%d mood=%d\n", cfg.mood_enabled, mood);
        initDroidPeripherals();
    } else {
        S.println("[SD] No SD card / mount failed — insert configured card");
        SD.end();
        sdSPI.end();

        // TESTING fallback: with no readable card, force the embedded demo
        // config from flash — same strings the test provisioning would have
        // written to the card. Compiled OUT at go-live (the embedded files
        // vanish with SFGE_WRITE_TEST_CONFIGS), restoring the strict
        // cards-configure-props rule.
        const char *ec = sfge_test::embeddedFile("/SFGE/config.json");
        if (ec) {
            JsonDocument doc;
            if (!deserializeJson(doc, ec)) {
                applyConfigDoc(doc);
                S.printf("[TEST] FORCED embedded demo config: %s (%s) — no SD!\n",
                         cfg.name, cfg.id);
            }
            const char *el = sfge_test::embeddedFile("/SFGE/lights.txt");
            if (el) sfge_lights::loadBuiltin(el);
            else    sfge_lights::blank();
            moodApplyEye();
            initDroidPeripherals();
        } else {
            sfge_lights::blank();   // clear the strip's random power-on state
        }
    }

    startHttpServer();

    S.println("[BOOT] mesh init...");
    sfge::meshInit(cfg.id, sfge::ROLE_PANEL, onMeshMsg);

    // Boot sound — astromech wake-up chatter, or (behavior.startup_tune) a
    // jaunty little melody for droids with more personality
    if (cfg.startup_song) {
        if (cfg.boot_tune) {
            static const struct { int f, on, gap; } TUNE[] = {
                { 659, 120,  30 }, { 784, 120,  30 }, { 880, 160,  40 },
                { 784, 100,  20 }, { 659, 120,  30 }, { 523, 160,  50 },
                { 587, 110,  25 }, { 659, 110,  25 }, { 784, 240, 120 },
                {1047, 300,   0 },
            };
            for (auto &n : TUNE) { buzzerTone(n.f, n.on); delay(n.on + n.gap); }
        } else {
            buzzerTone(220, 180); delay(220);
            for (int f = 400; f <= 1400; f += 200) { buzzerTone(f, 40); delay(48); }
            delay(90);
            astromechChirp();
            delay(120);
            buzzerTone(2200, 60); delay(70); buzzerTone(1800, 90);
        }

        // Head: either the configured emphatic shakes, or the default wiggle
        if (cfg.servo_enabled && cfg.boot_shakes > 0) {
            for (int s2 = 0; s2 < cfg.boot_shakes; s2++) {
                for (float d = 90; d >= 62; d -= 2.0f) { servoWriteDeg(d); delay(9); }
                for (float d = 62; d <= 118; d += 2.0f) { servoWriteDeg(d); delay(9); }
                for (float d = 118; d >= 90; d -= 2.0f) { servoWriteDeg(d); delay(9); }
            }
            headDeg = headTarget = 90;   // hand a clean center back to servoService
            servoWriteDeg(90);
        } else if (cfg.servo_enabled) {
            headReact(true);   // wake-up wiggle
        }
    }

    S.println("[DROID] Ready — waiting for operatives");
}

// ═══════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════
volatile bool pendingCelebrate = false;
volatile uint32_t pendingCelebrateColor = 0;

void loop() {
    sfge::eventRetryService();   // re-fire gameplay events until datapads/GM confirm

    // Sabotage lights: broken-circuit flicker until repaired
    if (sabDirty) {
        sabDirty = false;
        sfge_lights::setSabotage(sabotaged);
        if (sabotaged) { buzzerTone(200, 300); headReact(false); }
        else           { buzzerTone(880, 80); delay(90); buzzerTone(1320, 120); headReact(true); }
    }

    // Restraining bolt: poll the second reader + run the state machine
    if (!sabotaged) boltService();

    // Card reader: feed the droid a card (dead while sabotaged)
    if (nfcOk && !sabotaged) {
        static unsigned long lastNfcPoll = 0;
        if (millis() - lastNfcPoll > 250) {
            lastNfcPoll = millis();
            uint8_t uid[7]; uint8_t uidLen;
            if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 50)) {
                readNdefText();
                if (ndefText[0]) onDroidCardScanned();
            }
        }
    }

    // Victory celebration (game_complete / faction victory heard on the mesh)
    if (pendingCelebrate) {
        pendingCelebrate = false;
        sfge_lights::celebrate(12000, pendingCelebrateColor);
        headReact(true);                     // happy wiggle
        buzzerTone(1047, 120); delay(140);   // C6-E6-G6 fanfare
        buzzerTone(1319, 120); delay(140);
        buzzerTone(1568, 350);
    }

    // Lights: idle pattern + any interaction effect queued by HTTP handlers
    if (pendingFx != FX_NONE) {
        uint8_t fx = pendingFx;
        pendingFx = FX_NONE;
        switch (fx) {
        case FX_ACTIVITY: sfge_lights::effectActivity(); headReact(true);  break;
        case FX_SUCCESS:  sfge_lights::effectSuccess();  headReact(true);  break;
        case FX_FAIL:     sfge_lights::effectFail();     headReact(false); break;
        case FX_SIGNAL:   sfge_lights::effectSignal();   headReact(true);  break;
        }
    }
    sfge_lights::update();
    servoService();

    // Random idle chatter every 30-60s
    static unsigned long nextChirp = 0;
    if (millis() > nextChirp) {
        astromechChirp();
        nextChirp = millis() + 30000 + random(0, 30000);
    }

    // Mesh heartbeat every 5s (+ prop status with mood)
    static unsigned long lastBeat = 0;
    if (millis() - lastBeat > 5000) {
        lastBeat = millis();
        sfge::sendPing(millis() / 1000);
        sfge::sendPropStatus(millis() / 1000, cfg.mood_enabled ? mood : 0, false, "", sabotaged);
        S.printf("[MESH] ping seq sent (uptime %lus, mood %d)\n", millis() / 1000, mood);
    }

    // Mood decay toward 0 + debounced persistence
    if (cfg.mood_enabled) {
        if (mood != 0 && millis() - moodLastDecay > (unsigned long)cfg.mood_decay_s * 1000UL) {
            moodLastDecay = millis();
            mood += (mood > 0) ? -1 : +1;
            moodDirty = true;
            moodApplyEye();
        }
        static unsigned long lastMoodSave = 0;
        if (moodDirty && millis() - lastMoodSave > 3000) {
            lastMoodSave = millis();
            moodDirty = false;
            moodSave();
        }
    }

    delay(50);
}
