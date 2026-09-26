/*
 * SWTS TEST PROVISIONING — embedded gameplay configs written on boot
 *
 * While SWTS_WRITE_TEST_CONFIGS is defined, every device writes its
 * Twin Suns demo gameplay files to its own storage at boot, before the
 * normal config load:
 *
 *   datapad  → SD        (config, missions, comms, nfc_map, player)
 *   gm       → SD_MMC    (gm_config)
 *   panel    → LittleFS  (config, dialogue/greeting)
 *   terminal → SD        (config, dialogue/greeting)
 *   droid    → SD        (config, dialogue/greeting)
 *
 * Existing files are overwritten EXCEPT player.json, which is kept if
 * present so player progress survives a reboot mid-session. (Delete
 * /SWTS/player.json from the card to reset a player.)
 *
 * The embedded JSON mirrors scenarios/twin_suns_demo/ — edit both when
 * changing gameplay content, or regenerate this file from the scenario.
 *
 * ██ GO-LIVE: comment out the #define below and rebuild. Devices then
 * ██ boot from whatever is on their provisioned cards, untouched.
 */
#pragma once
#include <Arduino.h>
#include <FS.h>

// ── TESTING ONLY — comment out to go live ──
#define SWTS_WRITE_TEST_CONFIGS

namespace swts_test {

#ifdef SWTS_WRITE_TEST_CONFIGS

struct TestFile {
    const char *path;
    const char *content;
    bool overwrite;   // false = only write when the file doesn't exist yet
};

// ═══════════════════════════════════════
//  DATAPAD — player handheld
// ═══════════════════════════════════════
#if defined(SWTS_DEVICE_DATAPAD)

static const char T_CONFIG[] = R"json({
  "$schema": "swts-config-v1",
  "scenario": {
    "name": "Tatooine Run",
    "version": "0.1-demo",
    "planet": "Outpost 77",
    "currency": "CR"
  }
}
)json";

static const char T_MISSIONS[] = R"json({
  "$schema": "swts-missions-v1",
  "missions": [
    {
      "id": "intel_run",
      "title": "INTEL RUN",
      "subtitle": "Recover encrypted intel from the outpost and deliver it to R5-D8",
      "faction": "REBEL",
      "briefing": "Operative, a rebel sympathizer at the Twin Suns Cantina has dead-dropped an intel package for us.\n\nFind the datacard at the outpost, take it to the Repair Shop Decoder to decrypt, and hand the coordinates to R5-D8 who is hiding in the storage facility.\n\nImperial patrols are sweeping the area. Stay low.",
      "difficulty": 1,
      "reward_credits": 250,
      "reward_xp": 100,
      "reward_item": "ALLIANCE COMMENDATION",
      "unlocks": "extraction",
      "starts_unlocked": true,
      "steps": [
        {
          "title": "Pick up the Datacard",
          "description": "Search the cantina and cafe for an INTEL_01 datacard. Scan it on your datapad.",
          "type": "scan_nfc",
          "target": "INTEL_01",
          "xp": 30
        },
        {
          "title": "Decrypt at the Repair Shop",
          "description": "Carry the encrypted vault key DT_VAULT to the Repair Shop Terminal and scan it on the terminal reader.",
          "type": "event",
          "target": "nfc:TERM_01:DT_VAULT",
          "xp": 60
        },
        {
          "title": "Deliver to R5-D8",
          "description": "Find R5-D8 in the storage facility, purge his scrambled memory core, and hand off the decrypted intel.",
          "type": "event",
          "target": "droid_handoff:DROID_R5D8",
          "xp": 100
        }
      ]
    },
    {
      "id": "extraction",
      "title": "EXTRACTION",
      "subtitle": "Signal the shuttle and get off-world before the Empire locks down the outpost",
      "faction": "REBEL",
      "briefing": "The coordinates are with R5 -- now Command wants you out alive.\n\nThe cantina comm board can reach the shuttle on the outpost's open channel, but you'll have to slice through its security first. Broadcast the extraction signal, then get to the landing zone and scan the extraction beacon.\n\nMove fast. The Empire traces every wideband burst.",
      "difficulty": 2,
      "reward_credits": 300,
      "reward_xp": 200,
      "reward_item": "ALLIANCE SERVICE MEDAL",
      "unlocks": "",
      "starts_unlocked": false,
      "endgame": true,
      "steps": [
        {
          "title": "Signal the shuttle",
          "description": "Slice the Cantina Comm Board and broadcast the extraction signal on the open channel.",
          "type": "event",
          "target": "extraction:PANEL_01",
          "xp": 80
        },
        {
          "title": "Reach the extraction point",
          "description": "Get to the landing zone and scan the EXTRACT_01 beacon to confirm pickup.",
          "type": "scan_nfc",
          "target": "EXTRACT_01",
          "xp": 120
        }
      ]
    }
  ],
  "nfc_triggers": [
    { "token": "INTEL_01",   "action": "step", "mission": "intel_run",  "step": 0 },
    { "token": "EXTRACT_01", "action": "step", "mission": "extraction", "step": 1 }
  ]
}
)json";

static const char T_COMMS[] = R"json({
  "$schema": "swts-comms-v1",
  "messages": [
    {
      "id": "welcome",
      "from": "ALLIANCE COMMAND",
      "subject": "WELCOME TO OUTPOST 77",
      "body": "Operative, you've been deployed to the Twin Suns Outpost. A rebel contact has left intel for us at the cantina. Find it, decrypt it at the Repair Shop Terminal, and get the coordinates to R5-D8 in the storage facility. Watch for Imperial patrols.",
      "trigger": "boot"
    },
    {
      "id": "assignment",
      "from": "ALLIANCE COMMAND",
      "subject": "ASSIGNMENT: INTEL RUN",
      "body": "Your first objective is live. A sympathizer dead-dropped a datacard marked INTEL_01 somewhere around the cantina. Find it and scan it on your datapad. The cantina's backroom comm board keeps records of everything that moves through here -- slice it if you need a lead.",
      "trigger": "mission_start:intel_run"
    },
    {
      "id": "intel_scanned",
      "from": "R5-D8",
      "subject": "SIGNAL ACQUIRED",
      "body": "BEEP BOOP. Translation: Datacard located. The encryption is heavy -- you'll need a friendly terminal to break it. The Repair Shop Decoder will do. Bring the vault key DT_VAULT with you.",
      "trigger": "scan:INTEL_01"
    },
    {
      "id": "terminal_directions",
      "from": "ALLIANCE COMMAND",
      "subject": "DECRYPTION ROUTE",
      "body": "Operative, the Repair Shop Terminal on the cantina is rebel-friendly. Carry the vault key DT_VAULT to it and present the card at its reader port. The terminal will broadcast confirmation when decryption is complete.",
      "trigger": "event:nfc:TERM_01:INTEL_01"
    },
    {
      "id": "decrypt_complete",
      "from": "REPAIR SHOP TERMINAL",
      "subject": "DECRYPTION COMPLETE",
      "body": "Vault key DT_VAULT verified. Coordinates extracted. Transmitting to R5-D8. Recommend handoff at the storage facility immediately. Warning: his memory core took a hit from Imperial ice -- you may need to purge it before he can accept the upload.",
      "trigger": "event:nfc:TERM_01:DT_VAULT"
    },
    {
      "id": "handoff_confirmed",
      "from": "ALLIANCE COMMAND",
      "subject": "COORDINATES RECEIVED",
      "body": "R5-D8's transmission just reached the fleet. Outstanding work, operative. Stand by for extraction orders -- you're coming home.",
      "trigger": "event:droid_handoff:DROID_R5D8"
    },
    {
      "id": "extraction_orders",
      "from": "ALLIANCE COMMAND",
      "subject": "EXTRACTION ORDERS",
      "body": "A Lambda shuttle is holding position off-world for you. The cantina comm board can reach it on the outpost's open channel -- slice through its security and broadcast the extraction signal. Then get to the landing zone. Don't keep the pilot waiting.",
      "trigger": "mission_start:extraction"
    },
    {
      "id": "shuttle_inbound",
      "from": "TWIN SUNS FLIGHT",
      "subject": "SHUTTLE INBOUND",
      "body": "Signal received, operative. Lambda shuttle on approach -- ETA ten minutes. Proceed to the landing zone and scan the extraction beacon EXTRACT_01 to confirm pickup. Clear skies.",
      "trigger": "event:extraction:PANEL_01"
    }
  ]
}
)json";

static const char T_NFC_MAP[] = R"json({
  "$schema": "swts-nfc-map-v1",
  "_comment": "UID-to-content map. The 'uid' keys are placeholders — replace with the actual hex UIDs of YOUR physical NFC tags once you've scanned them in the datacard reader to learn the IDs.",
  "tags": {
    "01:23:45:67": {
      "type": "datacard",
      "id": "INTEL_01",
      "name": "Rebel Intel Drop",
      "category": "mission_trigger",
      "description": "An encrypted intel package left by a rebel sympathizer at the cantina.",
      "triggers_mission": "intel_run"
    },
    "01:23:45:68": {
      "type": "datatape",
      "id": "DT_VAULT",
      "name": "Vault Key DT_VAULT",
      "category": "key",
      "description": "Imperial-grade vault key. Must be scanned at a Rebel-friendly terminal to decrypt.",
      "grants_access": "decrypt"
    },
    "01:23:45:69": {
      "type": "cargo",
      "id": "CARGO_01",
      "name": "Spice Shipment",
      "category": "currency",
      "description": "A small crate of refined spice. Sells for credits.",
      "award_credits": 75
    },
    "01:23:45:6A": {
      "type": "beacon",
      "id": "EXTRACT_01",
      "name": "Extraction Beacon",
      "category": "mission_trigger",
      "description": "Alliance extraction beacon at the landing zone. Scanning it confirms pickup.",
      "triggers_mission": "extraction"
    }
  }
}
)json";

static const char T_PLAYER[] = R"json({
  "$schema": "swts-player-v1",
  "callsign": "OPERATIVE",
  "score": 0,
  "xp": 0,
  "totalScans": 0,
  "missions": [],
  "comms_read": [],
  "bounties_won": []
}
)json";

static const TestFile FILES[] = {
    { "/SWTS/config.json",   T_CONFIG,   true  },
    { "/SWTS/missions.json", T_MISSIONS, true  },
    { "/SWTS/comms.json",    T_COMMS,    true  },
    { "/SWTS/nfc_map.json",  T_NFC_MAP,  true  },
    { "/SWTS/player.json",   T_PLAYER,   false },   // keep progress across reboots
};

// ═══════════════════════════════════════
//  GM DATAPAD
// ═══════════════════════════════════════
#elif defined(SWTS_DEVICE_GM)

static const char T_GM_CONFIG[] = R"json({
  "$schema": "swts-gm-config-v1",
  "scenario": {
    "name": "Tatooine Run",
    "version": "0.1-demo",
    "planet": "Outpost 77",
    "currency": "CR"
  },
  "device": {
    "id": "GM-1",
    "role": "gamemaster"
  },
  "game": {
    "max_score": 1000,
    "event_duration_min": 30,
    "auto_advance": false
  },
  "ui": {
    "tabs": [
      {"id": "events",   "label": "EVENTS",   "color": "amber"},
      {"id": "comms",    "label": "COMMS",    "color": "cyan"},
      {"id": "bounties", "label": "BOUNTIES", "color": "red"},
      {"id": "players",  "label": "PLAYERS",  "color": "amber"},
      {"id": "panels",   "label": "PANELS",   "color": "cyan"}
    ]
  },
  "events": [
    {
      "id": "imperial_patrol",
      "name": "IMPERIAL PATROL",
      "severity": 2,
      "description": "Stormtrooper patrol sweeping the area. All operatives take cover and avoid open ground."
    },
    {
      "id": "sandstorm",
      "name": "INCOMING SANDSTORM",
      "severity": 1,
      "description": "Sand levels rising. Visibility dropping. Move quickly between buildings."
    },
    {
      "id": "cantina_lockdown",
      "name": "CANTINA LOCKDOWN",
      "severity": 3,
      "description": "Imperial security has locked down the cantina. Avoid the area until further notice."
    },
    {
      "id": "bounty_posted",
      "name": "BOUNTY POSTED",
      "severity": 1,
      "description": "High-value target identified. Check the wanted board."
    },
    {
      "id": "all_clear",
      "name": "ALL CLEAR",
      "severity": 0,
      "description": "Patrol has moved on. Resume normal operations."
    },
    {
      "id": "extraction_inbound",
      "name": "EXTRACTION INBOUND",
      "severity": 0,
      "description": "Lambda shuttle inbound for pickup. All operatives complete current objectives and report in."
    }
  ],
  "comms": [
    {
      "id": "gm_patrol_warn",
      "from": "OUTPOST SECURITY",
      "subject": "PATROL WARNING",
      "body": "Imperial patrol detected. Avoid the speeder repair area for the next 5 minutes."
    },
    {
      "id": "gm_bounty_alert",
      "from": "ALLIANCE INTEL",
      "subject": "PRIORITY ALERT",
      "body": "A bounty hunter has been spotted near the cantina. Check the wanted board for details."
    },
    {
      "id": "gm_extraction_brief",
      "from": "ALLIANCE COMMAND",
      "subject": "EXTRACTION DETAILS",
      "body": "Once R5 has the intel, regroup at the cafe for pickup. Lambda shuttle ETA 10 minutes."
    },
    {
      "id": "gm_well_done",
      "from": "ALLIANCE COMMAND",
      "subject": "FIELD COMMENDATION",
      "body": "Outstanding work, operative. Mission objectives complete. Your service is logged."
    }
  ],
  "bounties": [
    {
      "id": "bounty_greedo",
      "target_name": "Greedo",
      "description": "Rodian bounty hunter casing the Twin Suns Cantina. Armed, opportunistic, working freelance for the Empire.",
      "reward": 300,
      "clues": [
        "Last seen at the cantina bar. Green skin, big black eyes, holstered DT-12 blaster.",
        "Asked the bartender about Rebel sympathizers. Suspects something.",
        "Heading toward the speeder repair area — intercept before he reaches the terminal."
      ]
    }
  ]
}
)json";

static const TestFile FILES[] = {
    { "/SWTS/gm_config.json", T_GM_CONFIG, true },
};

// ═══════════════════════════════════════
//  PANEL — Cantina Comm Board (PANEL_01)
// ═══════════════════════════════════════
#elif defined(SWTS_DEVICE_PANEL)

static const char T_CONFIG[] = R"json({
  "$schema": "swts-config-v1",
  "scenario": {"name": "Tatooine Run", "version": "0.1-demo"},
  "prop": {
    "type": "panel",
    "id": "PANEL_01",
    "ssid": "SWTS_PANEL_01",
    "wifi_channel": 1,
    "name": "Cantina Comm Board",
    "faction": "neutral",
    "location_hint": "Twin Suns Cantina dome"
  },
  "hardware": {
    "has_nfc_reader": false,
    "has_leds": false,
    "has_buzzer": true,
    "buzzer_pin": 4
  },
  "behavior": {
    "idle_animation": "cantina_pulse",
    "requires_auth_card": false,
    "slice_first": true,
    "minigame_default": "simon",
    "simon_rounds": 3,
    "dialogue_greeting": "dialogue/greeting.json"
  }
}
)json";

static const char T_GREETING[] = R"json({
  "type": "dialogue",
  "speaker": {"name": "Cantina Comm Board", "faction": "neutral"},
  "lines": [
    {"text": "BACKROOM TERMINAL // OPEN CHANNEL", "style": "system"},
    {"text": "\"You looking for someone? Half the patrons in here are. Spice traders, smugglers, the occasional Jedi — nobody asks questions.\"", "style": "barkeep"},
    {"text": "Local news ticker: Speeder thieves spotted near the repair shop. Imperial bounties posted on the wanted board. Sandstorm warning in effect.", "style": "info"}
  ],
  "choices": [
    {"label": "Listen for rumors", "next_action": "rumor"},
    {"label": "[Disconnect]", "next_action": null}
  ]
}
)json";

// Sliceable data — served by the "Access data logs" choice after a breach
static const char T_LORE1[] = R"lore(TITLE: Cantina Backroom Ledger
CLASS: ENCRYPTED // OUTPOST 77 PRIVATE CHANNEL
DATE: 3 ABY
---
Sliced from the comm board's private
booking ledger:

ENTRY 47: Off-world courier paid cash for
one night, no name given. Left before the
suns rose. Bartender says the courier
taped something under the corner booth --
a datacard marked INTEL_01.

ENTRY 51: Imperial patrol has doubled its
sweeps of the market square. They are
looking for a droid. Storage facility has
NOT been searched yet.

ENTRY 52: The repair shop decoder is
running jobs off the books for the right
people. Bring your own encryption key.
---
END OF LEDGER
)lore";

static const TestFile FILES[] = {
    { "/SWTS/config.json",            T_CONFIG,   true },
    { "/SWTS/dialogue/greeting.json", T_GREETING, true },
    { "/SWTS/lore/l001.txt",          T_LORE1,    true },
};

// ═══════════════════════════════════════
//  TERMINAL — Repair Shop Decoder (TERM_01)
// ═══════════════════════════════════════
#elif defined(SWTS_DEVICE_TERMINAL)

static const char T_CONFIG[] = R"json({
  "$schema": "swts-config-v1",
  "scenario": {"name": "Tatooine Run", "version": "0.1-demo"},
  "prop": {
    "type": "terminal",
    "id": "TERM_01",
    "ssid": "SWTS_TERM_01",
    "wifi_channel": 1,
    "name": "Repair Shop Decoder",
    "faction": "rebel",
    "location_hint": "Speeder repair shop, attached to cantina"
  },
  "hardware": {
    "has_nfc_reader": true,
    "has_buzzer": true,
    "buzzer_pin": 4
  },
  "behavior": {
    "scan_cooldown_ms": 1500,
    "accepts_datatapes": true,
    "datatape_action": "decrypt",
    "dialogue_greeting": "dialogue/greeting.json"
  }
}
)json";

static const char T_GREETING[] = R"json({
  "type": "dialogue",
  "speaker": {"name": "Repair Shop Decoder", "faction": "rebel"},
  "lines": [
    {"text": "DECODER ONLINE // REBEL-FRIENDLY", "style": "system"},
    {"text": "Present an encrypted datacard at the reader port. I'll handle the rest.", "style": "info"}
  ],
  "choices": [
    {"label": "[Disconnect]", "next_action": null}
  ]
}
)json";

static const TestFile FILES[] = {
    { "/SWTS/config.json",            T_CONFIG,   true },
    { "/SWTS/dialogue/greeting.json", T_GREETING, true },
};

// ═══════════════════════════════════════
//  DROID — R5-D8 (DROID_R5D8)
// ═══════════════════════════════════════
#elif defined(SWTS_DEVICE_DROID)

static const char T_CONFIG[] = R"json({
  "$schema": "swts-config-v1",
  "scenario": {"name": "Tatooine Run", "version": "0.1-demo"},
  "prop": {
    "type": "droid",
    "id": "DROID_R5D8",
    "ssid": "SWTS_DROID_R5",
    "wifi_channel": 1,
    "name": "R5-D8",
    "faction": "rebel",
    "location_hint": "Storage facility (the generic building)"
  },
  "hardware": {
    "has_nfc_reader": false,
    "has_leds": true,
    "has_buzzer": true,
    "buzzer_pin": 4
  },
  "behavior": {
    "idle_animation": "astromech_idle",
    "accepts_intel": true,
    "completes_mission": "intel_run",
    "minigame_default": "purge",
    "purge_targets": 12,
    "purge_time_s": 35,
    "dialogue_greeting": "dialogue/greeting.json"
  }
}
)json";

static const char T_GREETING[] = R"json({
  "type": "dialogue",
  "speaker": {"name": "R5-D8", "faction": "rebel"},
  "lines": [
    {"text": "BEEP-BWEEP-WHIRR. (Translation: About time you showed up.)", "style": "droid"},
    {"text": "Did you decrypt the package? If yes, I'll log the handoff and signal Command. If not, get to the Repair Shop Decoder first.", "style": "system"}
  ],
  "choices": [
    {"label": "Hand off intel", "next_action": "deliver_intel"},
    {"label": "[Disconnect]", "next_action": null}
  ]
}
)json";

static const TestFile FILES[] = {
    { "/SWTS/config.json",            T_CONFIG,   true },
    { "/SWTS/dialogue/greeting.json", T_GREETING, true },
};

#else
#error "swts_test_configs.h: no SWTS_DEVICE_* build flag defined"
#endif

// Write this device's embedded test files to `fs` (SD, SD_MMC, or LittleFS).
// Call after the filesystem is mounted, before the normal config load.
inline void writeTestConfigs(fs::FS &fs) {
    Serial.println("[TEST] SWTS_WRITE_TEST_CONFIGS active — writing embedded configs");
    fs.mkdir("/SWTS");
#if defined(SWTS_DEVICE_PANEL) || defined(SWTS_DEVICE_TERMINAL) || defined(SWTS_DEVICE_DROID)
    fs.mkdir("/SWTS/dialogue");
#endif
#if defined(SWTS_DEVICE_PANEL)
    fs.mkdir("/SWTS/lore");
#endif
    for (const TestFile &tf : FILES) {
        if (!tf.overwrite && fs.exists(tf.path)) {
            Serial.printf("[TEST]   keep  %s (exists)\n", tf.path);
            continue;
        }
        File f = fs.open(tf.path, FILE_WRITE);
        if (!f) {
            Serial.printf("[TEST]   FAIL  %s (open for write failed)\n", tf.path);
            continue;
        }
        f.print(tf.content);
        f.close();
        Serial.printf("[TEST]   wrote %s (%u bytes)\n", tf.path, (unsigned)strlen(tf.content));
    }
}

#else   // SWTS_WRITE_TEST_CONFIGS not defined — go-live: no-op

inline void writeTestConfigs(fs::FS &) {}

#endif  // SWTS_WRITE_TEST_CONFIGS

} // namespace swts_test
