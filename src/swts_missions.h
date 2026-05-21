/*
 * SWTS Missions — Load mission definitions from /SWTS/missions/*.json
 * Tracks active missions, step advancement, objective checking
 */
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

namespace swts {

struct MsnStep {
    char step_id[8];
    char title[48];
    char description[80];
    char objective_type[16];    // "nfc_scan" or "prop_interact"

    // Target
    char target_nfc_id[32];     // for nfc_scan
    char target_prop_id[16];    // for prop_interact
    char target_ssid[24];       // SSID prefix match
    char target_display[32];    // human-readable name

    // Minigame
    bool has_minigame;
    char minigame_type[12];
    int  minigame_diff;
    int  minigame_time;
    int  minigame_attempts;

    // Hint
    int  hint_delay;
    char hint_text[80];

    // Rewards on complete
    char message[80];
    int  xp;
    int  credits;
    int  faction_rep;
    char add_items[4][24];     int add_count;
    char remove_items[4][24];  int rem_count;
    char unlock_msn[4][24];    int unlock_count;

    // On fail
    char fail_msg[60];
    char fail_action[12];
};

struct MsnDef {
    char id[24];
    char title[32];
    char subtitle[80];
    char faction[12];
    int  difficulty;

    // Prerequisites
    char prereq_msn[4][24];    int prereq_count;

    // Trigger
    char trigger_type[16];      // "nfc_scan", "mission_unlock"
    char trigger_nfc_id[32];
    char trigger_unlock_by[24];

    // Briefing
    char brief_title[32];
    char brief_lines[6][80];   int brief_count;

    // Steps
    MsnStep steps[6];
    int num_steps;

    // Completion
    char summary[120];
    char title_awarded[32];
    char lore_unlock[32];
    int  reward_credits;
    int  reward_xp;
};

#define MAX_MSN_DEFS 12
#define MAX_ACTIVE    4

struct ActiveMsn {
    int8_t  def_idx;
    uint8_t step;
    bool    complete;
    unsigned long step_time;
};

inline MsnDef    defs[MAX_MSN_DEFS];
inline int       defCount = 0;
inline ActiveMsn active[MAX_ACTIVE];
inline bool      unlocked[MAX_MSN_DEFS];
inline int       activeCount = 0;

// ── Parse helpers ──

inline void parseStep(JsonObject s, MsnStep &ms) {
    memset(&ms, 0, sizeof(MsnStep));
    strlcpy(ms.step_id, s["step_id"] | "", sizeof(ms.step_id));
    strlcpy(ms.title, s["title"] | "", sizeof(ms.title));
    strlcpy(ms.description, s["description"] | "", sizeof(ms.description));
    strlcpy(ms.objective_type, s["objective_type"] | "", sizeof(ms.objective_type));

    JsonObject tgt = s["target"].as<JsonObject>();
    if (!tgt.isNull()) {
        strlcpy(ms.target_nfc_id, tgt["nfc_tag_id"] | "", sizeof(ms.target_nfc_id));
        strlcpy(ms.target_prop_id, tgt["prop_id"] | "", sizeof(ms.target_prop_id));
        strlcpy(ms.target_ssid, tgt["ssid_pattern"] | "", sizeof(ms.target_ssid));
        strlcpy(ms.target_display, tgt["display_name"] | "", sizeof(ms.target_display));
    }

    JsonObject mg = s["minigame"].as<JsonObject>();
    ms.has_minigame = !mg.isNull();
    if (ms.has_minigame) {
        strlcpy(ms.minigame_type, mg["type"] | "slice", sizeof(ms.minigame_type));
        ms.minigame_diff = mg["difficulty"] | 2;
        ms.minigame_time = mg["time_limit_seconds"] | 45;
        ms.minigame_attempts = mg["max_attempts"] | 3;
    }

    JsonObject hint = s["hint"].as<JsonObject>();
    if (!hint.isNull()) {
        ms.hint_delay = hint["delay_seconds"] | 120;
        strlcpy(ms.hint_text, hint["text"] | "", sizeof(ms.hint_text));
    }

    JsonObject oc = s["on_complete"].as<JsonObject>();
    if (!oc.isNull()) {
        strlcpy(ms.message, oc["message"] | "", sizeof(ms.message));
        ms.xp = oc["award_xp"] | 0;
        ms.credits = oc["award_credits"] | 0;
        ms.faction_rep = oc["faction_rep"] | 0;
        ms.add_count = 0;
        for (JsonVariant v : oc["add_inventory"].as<JsonArray>())
            if (ms.add_count < 4) strlcpy(ms.add_items[ms.add_count++], v.as<const char*>(), 24);
        for (JsonVariant v : oc["award_items"].as<JsonArray>())
            if (ms.add_count < 4) strlcpy(ms.add_items[ms.add_count++], v.as<const char*>(), 24);
        ms.rem_count = 0;
        for (JsonVariant v : oc["remove_inventory"].as<JsonArray>())
            if (ms.rem_count < 4) strlcpy(ms.remove_items[ms.rem_count++], v.as<const char*>(), 24);
        ms.unlock_count = 0;
        for (JsonVariant v : oc["unlock_missions"].as<JsonArray>())
            if (ms.unlock_count < 4) strlcpy(ms.unlock_msn[ms.unlock_count++], v.as<const char*>(), 24);
    }

    JsonObject of = s["on_fail"].as<JsonObject>();
    if (!of.isNull()) {
        strlcpy(ms.fail_msg, of["message"] | "", sizeof(ms.fail_msg));
        strlcpy(ms.fail_action, of["action"] | "", sizeof(ms.fail_action));
    }
}

inline bool parseMission(File &f, MsnDef &m) {
    JsonDocument doc;
    if (deserializeJson(doc, f)) return false;

    memset(&m, 0, sizeof(MsnDef));
    strlcpy(m.id, doc["id"] | "", sizeof(m.id));
    strlcpy(m.title, doc["title"] | "", sizeof(m.title));
    strlcpy(m.subtitle, doc["subtitle"] | "", sizeof(m.subtitle));
    strlcpy(m.faction, doc["faction"] | "", sizeof(m.faction));
    m.difficulty = doc["difficulty"] | 1;

    // Prerequisites
    m.prereq_count = 0;
    for (JsonVariant v : doc["prerequisites"]["required_missions"].as<JsonArray>())
        if (m.prereq_count < 4) strlcpy(m.prereq_msn[m.prereq_count++], v.as<const char*>(), 24);

    // Trigger
    strlcpy(m.trigger_type, doc["triggers"]["type"] | "", sizeof(m.trigger_type));
    strlcpy(m.trigger_nfc_id, doc["triggers"]["nfc_tag_id"] | "", sizeof(m.trigger_nfc_id));
    strlcpy(m.trigger_unlock_by, doc["triggers"]["unlocked_by"] | "", sizeof(m.trigger_unlock_by));

    // Briefing
    strlcpy(m.brief_title, doc["briefing"]["title"] | "", sizeof(m.brief_title));
    m.brief_count = 0;
    for (JsonVariant v : doc["briefing"]["lines"].as<JsonArray>())
        if (m.brief_count < 6) strlcpy(m.brief_lines[m.brief_count++], v.as<const char*>(), 80);

    // Steps
    m.num_steps = 0;
    for (JsonObject s : doc["steps"].as<JsonArray>()) {
        if (m.num_steps >= 6) break;
        parseStep(s, m.steps[m.num_steps++]);
    }

    // Completion
    strlcpy(m.summary, doc["completion"]["summary"] | "", sizeof(m.summary));
    strlcpy(m.title_awarded, doc["completion"]["title_awarded"] | "", sizeof(m.title_awarded));
    strlcpy(m.lore_unlock, doc["completion"]["lore_unlock"] | "", sizeof(m.lore_unlock));

    // Top-level rewards (from last step or completion)
    m.reward_credits = 0; m.reward_xp = 0;
    for (int i = 0; i < m.num_steps; i++) {
        m.reward_credits += m.steps[i].credits;
        m.reward_xp += m.steps[i].xp;
    }

    return true;
}

// ── Load all missions from /SWTS/missions/ ──

inline bool loadMissions() {
    File dir = LittleFS.open("/SWTS/missions");
    if (!dir || !dir.isDirectory()) {
        Serial.println("[MSN] No /SWTS/missions/ directory");
        return false;
    }

    defCount = 0;
    File f = dir.openNextFile();
    while (f && defCount < MAX_MSN_DEFS) {
        if (!f.isDirectory() && String(f.name()).endsWith(".json")) {
            if (parseMission(f, defs[defCount])) {
                Serial.printf("[MSN] Loaded: %s (%d steps)\n", defs[defCount].title, defs[defCount].num_steps);
                defCount++;
            }
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();

    Serial.printf("[MSN] %d missions loaded\n", defCount);
    return defCount > 0;
}

// ── Runtime state ──

inline void initState() {
    for (int i = 0; i < MAX_ACTIVE; i++) active[i].def_idx = -1;
    activeCount = 0;

    // Determine which missions are unlocked
    for (int i = 0; i < defCount; i++) {
        unlocked[i] = (defs[i].prereq_count == 0 &&
                       strcmp(defs[i].trigger_type, "mission_unlock") != 0);
    }
}

inline int findByTag(const char *nfcTagId) {
    for (int i = 0; i < defCount; i++)
        if (strcmp(defs[i].trigger_nfc_id, nfcTagId) == 0) return i;
    return -1;
}

inline int findById(const char *msnId) {
    for (int i = 0; i < defCount; i++)
        if (strcmp(defs[i].id, msnId) == 0) return i;
    return -1;
}

inline int findActiveSlot(int defIdx) {
    for (int i = 0; i < MAX_ACTIVE; i++)
        if (active[i].def_idx == defIdx && !active[i].complete) return i;
    return -1;
}

inline bool canStart(int defIdx) {
    if (defIdx < 0 || defIdx >= defCount) return false;
    if (!unlocked[defIdx]) return false;
    if (findActiveSlot(defIdx) >= 0) return false;
    return true;
}

inline bool startMission(int defIdx) {
    if (!canStart(defIdx)) return false;
    for (int i = 0; i < MAX_ACTIVE; i++) {
        if (active[i].def_idx == -1) {
            active[i].def_idx = defIdx;
            active[i].step = 0;
            active[i].complete = false;
            active[i].step_time = millis();
            activeCount++;
            Serial.printf("[MSN] Started: %s\n", defs[defIdx].title);
            return true;
        }
    }
    return false;
}

// Check if an NFC scan completes any active mission step
// Returns slot index or -1
inline int checkNfcObjective(const char *nfcId) {
    for (int i = 0; i < MAX_ACTIVE; i++) {
        if (active[i].def_idx < 0 || active[i].complete) continue;
        MsnStep &s = defs[active[i].def_idx].steps[active[i].step];
        if (strcmp(s.objective_type, "nfc_scan") == 0 &&
            strcmp(s.target_nfc_id, nfcId) == 0) return i;
    }
    return -1;
}

// Check if connecting to a prop completes any active mission step
inline int checkPropObjective(const char *propId, const char *ssid) {
    for (int i = 0; i < MAX_ACTIVE; i++) {
        if (active[i].def_idx < 0 || active[i].complete) continue;
        MsnStep &s = defs[active[i].def_idx].steps[active[i].step];
        if (strcmp(s.objective_type, "prop_interact") != 0) continue;
        // Match by prop_id (exact) or ssid_pattern (prefix)
        if (strlen(s.target_prop_id) > 0 && strcmp(s.target_prop_id, propId) == 0) return i;
        if (strlen(s.target_ssid) > 0 && strncmp(ssid, s.target_ssid, strlen(s.target_ssid)) == 0) return i;
    }
    return -1;
}

// Advance step — returns true if mission is now complete
inline bool advanceStep(int slot, int &outXp, int &outCredits) {
    if (slot < 0 || slot >= MAX_ACTIVE) return false;
    ActiveMsn &a = active[slot];
    MsnDef &m = defs[a.def_idx];
    MsnStep &s = m.steps[a.step];

    outXp = s.xp;
    outCredits = s.credits;

    // Unlock any missions
    for (int u = 0; u < s.unlock_count; u++) {
        int idx = findById(s.unlock_msn[u]);
        if (idx >= 0) unlocked[idx] = true;
    }

    a.step++;
    a.step_time = millis();

    if (a.step >= m.num_steps) {
        a.complete = true;
        activeCount--;
        Serial.printf("[MSN] COMPLETE: %s\n", m.title);
        return true;
    }
    Serial.printf("[MSN] Step %d/%d: %s\n", a.step + 1, m.num_steps, m.steps[a.step].title);
    return false;
}

inline const MsnStep* getCurrentStep(int slot) {
    if (slot < 0 || active[slot].def_idx < 0) return nullptr;
    return &defs[active[slot].def_idx].steps[active[slot].step];
}

} // namespace swts
