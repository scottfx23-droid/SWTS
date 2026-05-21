/*
 * SWTS Datapad — Mission system (SD-driven)
 * Mission and NFC trigger data are loaded from /SWTS/missions.json and /SWTS/nfc_map.json on boot.
 * No game content is baked into firmware.
 */
#pragma once
#include <Arduino.h>

// ═══════════════════════════════════════
//  DATA STRUCTURES
// ═══════════════════════════════════════

enum ObjType : uint8_t {
    OBJ_SCAN_NFC,       // Scan a specific NFC tag (token match)
    OBJ_CONNECT_PROP,   // Connect to a specific prop via WiFi (SSID prefix)
    OBJ_HAVE_ITEM       // Reserved
};

struct MissionStep {
    char title[40];
    char description[120];
    ObjType obj_type;
    char target[20];       // NFC token or SSID prefix
    int xp_reward;
};

#define MAX_MISSIONS       8
#define MAX_MISSION_STEPS  6

struct MissionDef {
    char id[24];
    char title[24];
    char subtitle[80];
    char faction[16];
    char briefing[320];
    uint8_t difficulty;        // 1-5
    uint8_t num_steps;
    MissionStep steps[MAX_MISSION_STEPS];
    int reward_credits;
    int reward_xp;
    char reward_item[32];      // "" if none
    char unlocks_id[24];       // "" if none
    bool starts_unlocked;
};

extern MissionDef ALL_MISSIONS[MAX_MISSIONS];
extern int NUM_MISSIONS;

// ═══════════════════════════════════════
//  NFC TRIGGER TABLE (also SD-driven)
// ═══════════════════════════════════════
enum TrigAction : uint8_t { TRIG_START, TRIG_STEP };

struct NfcTrigger {
    char match_token[16];
    TrigAction action;
    int8_t   mission_idx;   // index into ALL_MISSIONS
    uint8_t  step_idx;
};

#define MAX_TRIGGERS 16
extern NfcTrigger NFC_TRIGGERS[MAX_TRIGGERS];
extern int NUM_TRIGGERS;

// ═══════════════════════════════════════
//  RUNTIME STATE
// ═══════════════════════════════════════
#define MAX_ACTIVE 4

struct ActiveMission {
    int8_t   def_idx;       // -1 = slot empty
    uint8_t  current_step;  // 0-based
    bool     complete;
};

// ═══════════════════════════════════════
//  TOKEN GENERATOR — fallback when nfc_map.json doesn't have the UID
//  Hashes the UID to one of the runtime tokens
// ═══════════════════════════════════════
inline const char* uidToToken(uint8_t *uid, uint8_t len) {
    if (NUM_TRIGGERS == 0) return "";
    uint8_t h = uid[0] ^ uid[1] ^ (len > 2 ? uid[2] : 0) ^ (len > 3 ? uid[3] : 0);
    return NFC_TRIGGERS[h % NUM_TRIGGERS].match_token;
}
