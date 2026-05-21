/*
 * SWTS Comms — HTTP client for prop interaction
 * Connect to props via WiFi, send player state, receive dialogue/game/lore
 */
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

namespace swts {

// ── Connected prop info ──
struct PropInfo {
    char prop_id[16];
    char name[32];
    char type[12];
    char faction[12];
    bool has_nfc;
    bool has_minigame;
    char minigame_type[16];
    bool requires_auth;
    bool card_present;
    bool slice_first;
};

// ── Dialogue response data ──
struct DialogueLine {
    char text[100];
    char style[16];   // "system", "speech", "narration"
};

struct DialogueChoice {
    char label[40];
    char next_action[20];
};

struct LoreData {
    char title[40];
    char classification[24];
    char content[512];
};

struct MinigameConfig {
    char game[12];
    int  difficulty;
    int  time_limit;
    // Slice zones
    float zone_start[3];
    float zone_end[3];
    int   zone_points[3];
    int   zone_count;
};

struct MinigameResult {
    bool accepted;
    char message[80];
    bool unlocked;
    int  reward_xp;
};

// ── Global state ──
inline PropInfo         prop;
inline bool             connected = false;
inline DialogueLine     lines[6];     inline int lineCount = 0;
inline DialogueChoice   choices[4];   inline int choiceCount = 0;
inline LoreData         lore;
inline MinigameConfig   gameConfig;
inline MinigameResult   gameResult;

// ── WiFi connection ──
inline bool connectProp(const char *ssid) {
    Serial.printf("[COMM] Connecting to %s...\n", ssid);
    WiFi.begin(ssid);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
        delay(50);
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[COMM] Connected! IP=%s\n", WiFi.localIP().toString().c_str());
        delay(200);
        connected = true;
        return true;
    }
    Serial.printf("[COMM] Failed (status=%d)\n", WiFi.status());
    WiFi.disconnect();
    return false;
}

inline void disconnectProp() {
    WiFi.disconnect();
    connected = false;
    memset(&prop, 0, sizeof(PropInfo));
    Serial.println("[COMM] Disconnected");
}

// ── GET /api/info ──
inline bool fetchInfo() {
    HTTPClient http;
    http.setTimeout(5000);
    http.begin("http://192.168.4.1/api/info");
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[COMM] /api/info failed: %d\n", code);
        http.end();
        return false;
    }

    JsonDocument doc;
    deserializeJson(doc, http.getString());
    http.end();

    strlcpy(prop.prop_id, doc["prop_id"] | "??", sizeof(prop.prop_id));
    strlcpy(prop.name, doc["name"] | "Unknown", sizeof(prop.name));
    strlcpy(prop.type, doc["type"] | "panel", sizeof(prop.type));
    strlcpy(prop.faction, doc["faction"] | "", sizeof(prop.faction));
    prop.has_nfc = doc["features"]["has_nfc"] | false;
    prop.has_minigame = doc["features"]["has_minigame"] | false;
    strlcpy(prop.minigame_type, doc["features"]["minigame_type"] | "", sizeof(prop.minigame_type));
    prop.requires_auth = doc["features"]["requires_auth"] | false;
    prop.card_present = doc["features"]["card_present"] | false;
    prop.slice_first = doc["features"]["slice_first"] | false;

    Serial.printf("[COMM] Prop: %s (%s) game=%d\n", prop.name, prop.prop_id, prop.has_minigame);
    return true;
}

// ── POST /api/interact ──
// Returns response type: "dialogue", "minigame_start", "minigame_result", "lore", or "" on error
inline String interact(const char *action, const char *callsign, const char *faction = "REBEL") {
    HTTPClient http;
    http.setTimeout(5000);
    http.begin("http://192.168.4.1/api/interact");
    http.addHeader("Content-Type", "application/json");

    JsonDocument req;
    req["action"] = action;
    JsonObject p = req["player"].to<JsonObject>();
    p["callsign"] = callsign;
    p["faction"] = faction;

    String body;
    serializeJson(req, body);
    int code = http.POST(body);

    if (code != 200) {
        Serial.printf("[COMM] interact '%s' failed: %d\n", action, code);
        http.end();
        return "";
    }

    String resp = http.getString();
    http.end();

    // Parse response into structured data
    JsonDocument doc;
    if (deserializeJson(doc, resp)) return "";

    String rtype = doc["type"].as<String>();

    if (rtype == "dialogue") {
        lineCount = 0;
        for (JsonObject l : doc["lines"].as<JsonArray>()) {
            if (lineCount >= 6) break;
            strlcpy(lines[lineCount].text, l["text"] | "", sizeof(lines[0].text));
            strlcpy(lines[lineCount].style, l["style"] | "speech", sizeof(lines[0].style));
            lineCount++;
        }
        choiceCount = 0;
        for (JsonObject c : doc["choices"].as<JsonArray>()) {
            if (choiceCount >= 4) break;
            strlcpy(choices[choiceCount].label, c["label"] | "...", sizeof(choices[0].label));
            String act = c["next_action"].as<String>();
            strlcpy(choices[choiceCount].next_action, act.c_str(), sizeof(choices[0].next_action));
            choiceCount++;
        }
    }
    else if (rtype == "minigame_start") {
        strlcpy(gameConfig.game, doc["game"] | "slice", sizeof(gameConfig.game));
        gameConfig.difficulty = doc["difficulty"] | 2;
        gameConfig.time_limit = doc["time_limit"] | 45;
        gameConfig.zone_count = 0;
        for (JsonObject z : doc["target_zones"].as<JsonArray>()) {
            if (gameConfig.zone_count >= 3) break;
            int i = gameConfig.zone_count++;
            gameConfig.zone_start[i] = z["start"] | 0.0f;
            gameConfig.zone_end[i] = z["end"] | 0.0f;
            gameConfig.zone_points[i] = z["points"] | 50;
        }
    }
    else if (rtype == "minigame_result") {
        gameResult.accepted = doc["accepted"] | false;
        strlcpy(gameResult.message, doc["message"] | "", sizeof(gameResult.message));
        gameResult.unlocked = doc["unlocked"] | false;
        gameResult.reward_xp = doc["rewards"]["xp"] | 0;
    }
    else if (rtype == "lore") {
        strlcpy(lore.title, doc["title"] | "DATA LOG", sizeof(lore.title));
        strlcpy(lore.classification, doc["classification"] | "", sizeof(lore.classification));
        strlcpy(lore.content, doc["content"] | "", sizeof(lore.content));
    }

    return rtype;
}

// ── Convenience wrappers ──
inline String greet(const char *callsign) { return interact("greet", callsign); }
inline String startMinigame(const char *callsign) { return interact("start_minigame", callsign); }
inline String readLogs(const char *callsign) { return interact("read_logs", callsign); }

inline String sendMinigameResult(const char *callsign, bool won, int score) {
    HTTPClient http;
    http.setTimeout(5000);
    http.begin("http://192.168.4.1/api/interact");
    http.addHeader("Content-Type", "application/json");

    JsonDocument req;
    req["action"] = "minigame_result";
    req["won"] = won;
    req["score"] = score;
    JsonObject p = req["player"].to<JsonObject>();
    p["callsign"] = callsign;

    String body; serializeJson(req, body);
    int code = http.POST(body);
    String resp = code == 200 ? http.getString() : "";
    http.end();

    if (resp.length() > 0) {
        JsonDocument doc;
        if (!deserializeJson(doc, resp)) {
            gameResult.accepted = doc["accepted"] | false;
            strlcpy(gameResult.message, doc["message"] | "", sizeof(gameResult.message));
            gameResult.unlocked = doc["unlocked"] | false;
            gameResult.reward_xp = doc["rewards"]["xp"] | 0;
        }
    }
    return resp.length() > 0 ? "minigame_result" : "";
}

} // namespace swts
