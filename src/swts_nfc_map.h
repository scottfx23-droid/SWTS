/*
 * SWTS NFC Map — Load tag-to-content mapping from /SWTS/nfc_map.json
 */
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

namespace swts {

struct NfcTag {
    char uid[24];            // "A7:3F:02:B1"
    char type[16];           // "datacard", "datatape", "bounty_puck"
    char id[32];             // "DC_CLEARANCE_EAST"
    char name[40];           // "East Wing Clearance"
    char category[20];       // "key","intel","mission_trigger","lore","currency","bounty","item"
    char description[80];

    // Category-specific
    char triggers_mission[24]; // mission_trigger
    char grants_access[16];    // key
    char content_file[40];     // lore/intel
    int  award_credits;        // currency
    int  award_xp;             // lore
    int  bounty_reward;        // bounty
    bool requires_decrypt;
    char decrypt_minigame[12];
    bool single_use;
    char faction_required[12];

    // Bounty target display
    char bounty_name[24];
    char bounty_species[16];
    char bounty_last_seen[24];
    char bounty_threat[16];
};

#define MAX_NFC_TAGS 40
inline NfcTag nfcTags[MAX_NFC_TAGS];
inline int nfcTagCount = 0;

// Format raw UID bytes to "XX:XX:XX:XX" string
inline void formatUid(uint8_t *uid, uint8_t len, char *out, int outLen) {
    out[0] = 0;
    for (int i = 0; i < len && (i * 3 + 2) < outLen; i++) {
        if (i > 0) strcat(out, ":");
        char hex[4]; snprintf(hex, sizeof(hex), "%02X", uid[i]);
        strcat(out, hex);
    }
}

// Lookup by UID string — returns null if not found
inline NfcTag* lookupByUid(const char *uidStr) {
    for (int i = 0; i < nfcTagCount; i++) {
        if (strcmp(nfcTags[i].uid, uidStr) == 0) return &nfcTags[i];
    }
    return nullptr;
}

// Lookup by ID string (e.g. "DC_CLEARANCE_EAST")
inline NfcTag* lookupById(const char *id) {
    for (int i = 0; i < nfcTagCount; i++) {
        if (strcmp(nfcTags[i].id, id) == 0) return &nfcTags[i];
    }
    return nullptr;
}

inline bool loadNfcMap() {
    File f = LittleFS.open("/SWTS/nfc_map.json", "r");
    if (!f) {
        Serial.println("[NFC] No nfc_map.json on flash");
        return false;
    }

    JsonDocument doc;
    if (deserializeJson(doc, f)) {
        Serial.println("[NFC] Parse error");
        f.close();
        return false;
    }
    f.close();

    JsonObject tags = doc["tags"].as<JsonObject>();
    nfcTagCount = 0;

    for (JsonPair kv : tags) {
        if (nfcTagCount >= MAX_NFC_TAGS) break;
        NfcTag &t = nfcTags[nfcTagCount];
        memset(&t, 0, sizeof(NfcTag));

        strlcpy(t.uid, kv.key().c_str(), sizeof(t.uid));
        JsonObject v = kv.value().as<JsonObject>();

        strlcpy(t.type, v["type"] | "", sizeof(t.type));
        strlcpy(t.id, v["id"] | "", sizeof(t.id));
        strlcpy(t.name, v["name"] | "Unknown", sizeof(t.name));
        strlcpy(t.category, v["category"] | "", sizeof(t.category));
        strlcpy(t.description, v["description"] | "", sizeof(t.description));

        strlcpy(t.triggers_mission, v["triggers_mission"] | "", sizeof(t.triggers_mission));
        strlcpy(t.grants_access, v["grants_access"] | "", sizeof(t.grants_access));
        strlcpy(t.content_file, v["content_file"] | "", sizeof(t.content_file));
        t.award_credits = v["award_credits"] | 0;
        t.award_xp = v["award_xp"] | 0;
        t.bounty_reward = v["bounty_reward"] | 0;
        t.requires_decrypt = v["requires_decrypt"] | false;
        strlcpy(t.decrypt_minigame, v["decrypt_minigame"] | "", sizeof(t.decrypt_minigame));
        t.single_use = v["single_use"] | false;
        strlcpy(t.faction_required, v["faction_required"] | "", sizeof(t.faction_required));

        // Bounty display
        JsonObject bd = v["target_display"].as<JsonObject>();
        if (!bd.isNull()) {
            strlcpy(t.bounty_name, bd["name"] | "", sizeof(t.bounty_name));
            strlcpy(t.bounty_species, bd["species"] | "", sizeof(t.bounty_species));
            strlcpy(t.bounty_last_seen, bd["last_seen"] | "", sizeof(t.bounty_last_seen));
            strlcpy(t.bounty_threat, bd["threat_level"] | "", sizeof(t.bounty_threat));
        }

        nfcTagCount++;
    }

    Serial.printf("[NFC] Loaded %d tag definitions\n", nfcTagCount);
    return true;
}

} // namespace swts
