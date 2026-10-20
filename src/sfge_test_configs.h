/*
 * SFGE TEST PROVISIONING — embedded gameplay configs written on boot
 *
 * While SFGE_WRITE_TEST_CONFIGS is defined, every device writes its
 * Outpost 77 demo gameplay files to its own storage at boot, before the
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
 * /SFGE/player.json from the card to reset a player.)
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
#define SFGE_WRITE_TEST_CONFIGS

namespace sfge_test {

#ifdef SFGE_WRITE_TEST_CONFIGS

struct TestFile {
    const char *path;
    const char *content;
    bool overwrite;   // false = only write when the file doesn't exist yet
};

// ═══════════════════════════════════════
//  DATAPAD — player handheld
// ═══════════════════════════════════════
#if defined(SFGE_DEVICE_DATAPAD)

static const char T_CONFIG[] = R"json({
  "$schema": "sfge-config-v1",
  "scenario": {
    "name": "Tatooine Run",
    "version": "0.1-demo",
    "planet": "Outpost 77",
    "currency": "CR"
  },
  "radar_enabled": true,
  "radar_kid_mode": false,
  "duel": { "enabled": true, "targets": 5, "stake": 100, "cooldown_s": 120 }
}
)json";

static const char T_MISSIONS[] = R"json({
  "$schema": "sfge-missions-v1",
  "missions": [
    {
      "id": "intel_run",
      "title": "INTEL RUN",
      "subtitle": "Recover encrypted intel from the outpost and deliver it to R5-D8",
      "faction": "REBEL",
      "briefing": "Operative, a rebel sympathizer at the Outpost 77 Cantina has dead-dropped an intel package for us.\n\nFind the datacard at the outpost, take it to the Repair Shop Decoder to decrypt, and hand the coordinates to R5-D8 who is hiding in the storage facility.\n\nImperial patrols are sweeping the area. Stay low.",
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
    },
    {
      "id": "counter_slice",
      "title": "COUNTER-SLICE",
      "subtitle": "Trace the rebel slicer network and seize their dead drop",
      "faction": "IMPERIAL",
      "briefing": "ISB has flagged slicer traffic on the cantina's backroom comm board. The rebels are moving something through this outpost.\n\nBreach the board yourself and pull its private ledger, then locate the dead-dropped datacard before their courier collects it. Log it as evidence.\n\nThe Empire rewards efficiency.",
      "difficulty": 2,
      "reward_credits": 350,
      "reward_xp": 250,
      "reward_item": "IMPERIAL COMMENDATION",
      "unlocks": "",
      "starts_unlocked": true,
      "endgame": true,
      "steps": [
        {
          "title": "Breach the comm board",
          "description": "Slice the Cantina Comm Board's pattern lock and recover its backroom ledger.",
          "type": "event",
          "target": "panel_sliced:PANEL_01",
          "xp": 80
        },
        {
          "title": "Seize the dead drop",
          "description": "Locate the rebel datacard INTEL_01 and scan it to log it as evidence.",
          "type": "scan_nfc",
          "target": "INTEL_01",
          "xp": 120
        }
      ]
    }
  ],
  "nfc_triggers": [
    { "token": "INTEL_01",   "action": "step", "mission": "intel_run",     "step": 0 },
    { "token": "INTEL_01",   "action": "step", "mission": "counter_slice", "step": 1 },
    { "token": "EXTRACT_01", "action": "step", "mission": "extraction",    "step": 1 }
  ],
  "codes": [
    { "code": "7731", "event": "dock_access_granted", "name": "DOCK ACCESS" },
    { "code": "2187", "event": "supply_cache_opened", "name": "SUPPLY CACHE",
      "reusable": true }
  ]
}
)json";

static const char T_COMMS[] = R"json({
  "$schema": "sfge-comms-v1",
  "messages": [
    {
      "id": "welcome",
      "from": "ALLIANCE COMMAND",
      "subject": "WELCOME TO OUTPOST 77",
      "body": "Operative, you've been deployed to the Outpost 77. A rebel contact has left intel for us at the cantina. Find it, decrypt it at the Repair Shop Terminal, and get the coordinates to R5-D8 in the storage facility. Watch for Imperial patrols.",
      "trigger": "boot",
      "faction": "REBEL"
    },
    {
      "id": "welcome_imp",
      "from": "ISB COMMAND",
      "subject": "WELCOME TO OUTPOST 77",
      "body": "Agent, rebel activity is confirmed at the Outpost 77. They are moving intelligence through the cantina. Trace their slicer network, recover whatever they are hiding, and log it as evidence. The Empire is watching your performance.",
      "trigger": "boot",
      "faction": "IMPERIAL"
    },
    {
      "id": "assignment",
      "from": "ALLIANCE COMMAND",
      "subject": "ASSIGNMENT: INTEL RUN",
      "body": "Your first objective is live. A sympathizer dead-dropped a datacard marked INTEL_01 somewhere around the cantina. Find it and scan it on your datapad. The cantina's backroom comm board keeps records of everything that moves through here -- slice it if you need a lead.",
      "trigger": "mission_start:intel_run",
      "faction": "REBEL"
    },
    {
      "id": "counter_orders",
      "from": "ISB COMMAND",
      "subject": "ASSIGNMENT: COUNTER-SLICE",
      "body": "The cantina's backroom comm board carries the rebels' private traffic. Breach its pattern lock and pull the ledger. Whatever they are hiding at this outpost, the ledger will point you to it.",
      "trigger": "mission_start:counter_slice",
      "faction": "IMPERIAL"
    },
    {
      "id": "intel_scanned",
      "from": "R5-D8",
      "subject": "SIGNAL ACQUIRED",
      "body": "BEEP BOOP. Translation: Datacard located. The encryption is heavy -- you'll need a friendly terminal to break it. The Repair Shop Decoder will do. Bring the vault key DT_VAULT with you.",
      "trigger": "scan:INTEL_01",
      "faction": "REBEL"
    },
    {
      "id": "evidence_logged",
      "from": "ISB COMMAND",
      "subject": "EVIDENCE SECURED",
      "body": "Rebel datacard INTEL_01 logged and quarantined. Their courier network at this outpost is finished. Outstanding work, agent. Report for debrief.",
      "trigger": "scan:INTEL_01",
      "faction": "IMPERIAL"
    },
    {
      "id": "ledger_recovered",
      "from": "ISB COMMAND",
      "subject": "LEDGER RECOVERED",
      "body": "Good work on the breach. The ledger shows an off-world courier paid cash and taped something under the corner booth -- a datacard marked INTEL_01. Find it and scan it as evidence before their courier collects.",
      "trigger": "event:panel_sliced:PANEL_01",
      "faction": "IMPERIAL"
    },
    {
      "id": "terminal_directions",
      "from": "ALLIANCE COMMAND",
      "subject": "DECRYPTION ROUTE",
      "body": "Operative, the Repair Shop Terminal on the cantina is rebel-friendly. Carry the vault key DT_VAULT to it and present the card at its reader port. The terminal will broadcast confirmation when decryption is complete.",
      "trigger": "event:nfc:TERM_01:INTEL_01",
      "faction": "REBEL"
    },
    {
      "id": "decrypt_complete",
      "from": "REPAIR SHOP TERMINAL",
      "subject": "DECRYPTION COMPLETE",
      "body": "Vault key DT_VAULT verified. Coordinates extracted. Transmitting to R5-D8. Recommend handoff at the storage facility immediately. Warning: his memory core took a hit from Imperial ice -- you may need to purge it before he can accept the upload.",
      "trigger": "event:nfc:TERM_01:DT_VAULT",
      "faction": "REBEL"
    },
    {
      "id": "handoff_confirmed",
      "from": "ALLIANCE COMMAND",
      "subject": "COORDINATES RECEIVED",
      "body": "R5-D8's transmission just reached the fleet. Outstanding work, operative. Stand by for extraction orders -- you're coming home.",
      "trigger": "event:droid_handoff:DROID_R5D8",
      "faction": "REBEL"
    },
    {
      "id": "extraction_orders",
      "from": "ALLIANCE COMMAND",
      "subject": "EXTRACTION ORDERS",
      "body": "A Lambda shuttle is holding position off-world for you. The cantina comm board can reach it on the outpost's open channel -- slice through its security and broadcast the extraction signal. Then get to the landing zone. Don't keep the pilot waiting.",
      "trigger": "mission_start:extraction",
      "faction": "REBEL"
    },
    {
      "id": "shuttle_inbound",
      "from": "OUTPOST 77 FLIGHT",
      "subject": "SHUTTLE INBOUND",
      "body": "Signal received, operative. Lambda shuttle on approach -- ETA ten minutes. Proceed to the landing zone and scan the extraction beacon EXTRACT_01 to confirm pickup. Clear skies.",
      "trigger": "event:extraction:PANEL_01",
      "faction": "REBEL"
    },
    {
      "id": "reinforcements",
      "from": "OUTPOST WATCH",
      "subject": "REINFORCEMENTS ARRIVE",
      "body": "Imperial troop shuttles are on the deck. The extraction window is closed -- whatever you were doing, it just got a lot harder.",
      "trigger": "event:reinforcements_arrive"
    },
    {
      "id": "shadow_freq",
      "from": "UNKNOWN SENDER",
      "subject": "INTERCEPTED BURST",
      "body": "Backup extraction frequency is 121.9 -- memorize it and burn this message. If the comm board is compromised, broadcast from the repair shop decoder instead. Trust no one wearing white armor.",
      "trigger": "boot",
      "encrypted": true
    },
    {
      "id": "guild_contract",
      "from": "BOUNTY HUNTERS' GUILD",
      "subject": "CONTRACT POSTED",
      "body": "The Guild is open for business at the cantina back table. First contract of the season: SHILAK, Rodian gun-for-hire, armed and freelancing for the Empire -- 300 credits, no questions asked. Find him, scan his chain code, and bring it to Guild Master Vex. First hunter to deliver gets paid. The Guild does not split fees.",
      "trigger": "boot"
    },
    {
      "id": "grid_breached",
      "from": "OUTPOST WATCH",
      "subject": "POWER GRID BREACH",
      "body": "Someone just retuned the docking bay power junction. The duty log mentions berth 7 going dark off the books -- and a backup spin-up code the techs were told not to share. Try it on your keypad.",
      "trigger": "event:panel_sliced:PANEL_02"
    },
    {
      "id": "clamps_released",
      "from": "DOCKING CONTROL",
      "subject": "CLAMP ALERT",
      "body": "Berth 7 docking clamps just released without authorization. Whatever was parked there is free to fly. If that was you -- move now, before control re-engages them.",
      "trigger": "event:btn_clamp_release"
    },
    {
      "id": "bolt_removed_c1",
      "from": "ALLIANCE INTEL",
      "subject": "C1 UNIT UNSHACKLED",
      "body": "Someone pulled the restraining bolt off the C1 unit in the docking bay. That droid has been carrying a locked uplink since before the occupation -- feed it the battle plans datacard while it's free.",
      "trigger": "event:bolt_removed:DROID_C11U17K"
    },
    {
      "id": "battle_plans_up",
      "from": "ALLIANCE COMMAND",
      "subject": "BATTLE PLANS RECEIVED",
      "body": "The C1 unit just burst-transmitted the battle plans to the fleet. Outstanding work, operative. Keep that droid out of Imperial hands.",
      "trigger": "event:nfc:DROID_C11U17K:DATACARD_02"
    }
  ]
}
)json";

static const char T_NFC_MAP[] = R"json({
  "$schema": "sfge-nfc-map-v1",
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
    },
    "01:23:45:6B": {
      "type": "datacard",
      "id": "DATACARD_01",
      "name": "Fleet Movement Logs",
      "category": "datacard",
      "description": "Encrypted Imperial patrol schedules. Insert at a friendly terminal, or hand it to the contact who asked for it."
    },
    "01:23:45:6C": {
      "type": "datacard",
      "id": "DATACARD_02",
      "name": "Imperial Payroll Ledger",
      "category": "datacard",
      "description": "Garrison payroll records. Somebody at the cantina will pay well for these."
    }
  }
}
)json";

static const char T_PLAYER[] = R"json({
  "$schema": "sfge-player-v1",
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
    { "/SFGE/config.json",   T_CONFIG,   true  },
    { "/SFGE/missions.json", T_MISSIONS, true  },
    { "/SFGE/comms.json",    T_COMMS,    true  },
    { "/SFGE/nfc_map.json",  T_NFC_MAP,  true  },
    { "/SFGE/player.json",   T_PLAYER,   false },   // keep progress across reboots
};

// ═══════════════════════════════════════
//  GM DATAPAD
// ═══════════════════════════════════════
#elif defined(SFGE_DEVICE_GM)

static const char T_GM_CONFIG[] = R"json({
  "$schema": "sfge-gm-config-v1",
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
  "factions": ["REBEL", "IMPERIAL"],
  "timers": [
    { "id": "extraction_window", "label": "EXTRACTION WINDOW",
      "seconds": 600, "on_expire": "reinforcements_arrive", "faction": "",
      "declare_victory": true }
  ],
  "faction_colors": ["FF4400", "3388FF"],
  "game": {
    "max_score": 1000,
    "event_duration_min": 30,
    "auto_advance": false,
    "battery_budget_min": 300,
    "manhunt_reward": 250
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
      "description": "Lambda shuttle inbound for pickup. All operatives complete current objectives and report in.",
      "faction": "REBEL"
    },
    {
      "id": "isb_crackdown",
      "name": "ISB CRACKDOWN",
      "severity": 2,
      "description": "ISB authorizes force. All Imperial agents move on known rebel positions immediately.",
      "faction": "IMPERIAL"
    },
    {
      "id": "reinforcements_arrive",
      "name": "REINFORCEMENTS ARRIVE",
      "severity": 3,
      "description": "Imperial reinforcements have landed. The extraction window is closed."
    },
    {
      "id": "victory_rebel",
      "name": "REBEL VICTORY",
      "severity": 0,
      "description": "The Alliance carries the day. Outpost 77 is ours -- every prop lights up."
    },
    {
      "id": "victory_imperial",
      "name": "IMPERIAL VICTORY",
      "severity": 0,
      "description": "Order is restored. The Empire holds Outpost 77 -- every prop lights up."
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
      "body": "Once R5 has the intel, regroup at the cafe for pickup. Lambda shuttle ETA 10 minutes.",
      "faction": "REBEL"
    },
    {
      "id": "gm_well_done",
      "from": "ALLIANCE COMMAND",
      "subject": "FIELD COMMENDATION",
      "body": "Outstanding work, operative. Mission objectives complete. Your service is logged.",
      "faction": "REBEL"
    },
    {
      "id": "gm_imperial_orders",
      "from": "ISB COMMAND",
      "subject": "PRIORITY DIRECTIVE",
      "body": "Agents: rebel sympathizers are operating inside the cantina. Increase surveillance and report all datacard traffic to ISB immediately.",
      "faction": "IMPERIAL"
    }
  ],
  "bounties": [
    {
      "id": "bounty_shilak",
      "target_name": "Shilak",
      "description": "Rodian bounty hunter casing the Outpost 77 Cantina. Armed, opportunistic, working freelance for the Empire.",
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
    { "/SFGE/gm_config.json", T_GM_CONFIG, true },
};

// ═══════════════════════════════════════
//  PANEL — Cantina Comm Board (PANEL_01), or with the SFGE_PANEL2 build
//  flag, Docking Bay Power Junction (PANEL_02, env:panel2)
// ═══════════════════════════════════════
#elif defined(SFGE_DEVICE_PANEL)

#if defined(SFGE_PANEL2)

static const char T_CONFIG[] = R"json({
  "$schema": "sfge-config-v1",
  "scenario": {"name": "Tatooine Run", "version": "0.1-demo"},
  "prop": {
    "type": "panel",
    "id": "PANEL_02",
    "ssid": "SFGE_PANEL_02",
    "wifi_channel": 1,
    "name": "Docking Bay Power Junction",
    "faction": "imperial",
    "location_hint": "Docking bay service wall"
  },
  "hardware": {
    "has_nfc_reader": false,
    "has_leds": false,
    "has_buzzer": true,
    "buzzer_pin": 4,
    "aux_mode": "buttons"
  },
  "behavior": {
    "buttons": [
      { "event": "btn_clamp_release", "name": "DOCKING CLAMPS" },
      { "event": "btn_floodlights",   "name": "FLOODLIGHTS" }
    ],
    "button_cooldown_ms": 1500,
    "idle_animation": "grid_hum",
    "requires_auth_card": false,
    "slice_first": true,
    "minigame_default": "freq",
    "startup_song": true,
    "proximity_enabled": true,
    "proximity_rssi_start": -60,
    "proximity_rssi_drop": -72,
    "proximity_grace_ms": 2000,
    "alarm_after_fails": 2,
    "alarm_timeout_s": 90,
    "alarm_siren_period_ms": 350,
    "timer_visual": true,
    "sabotage_enabled": true,
    "dialogue_greeting": "dialogue/greeting.json"
  }
}
)json";

static const char T_GREETING[] = R"json({
  "type": "dialogue",
  "speaker": {"name": "Docking Bay Power Junction", "faction": "imperial"},
  "lines": [
    {"text": "IMPERIAL GRID NODE 77-B // AUTHORIZED PERSONNEL ONLY", "style": "system"},
    {"text": "Heavy conduits hum behind the service wall. A maintenance readout cycles through power figures for the docking bay clamps and floodlights.", "style": "narration"},
    {"text": "Carrier channels drift across the monitor band -- the grid is poorly tuned.", "style": "info"}
  ],
  "choices": [
    {"label": "[Disconnect]", "next_action": null}
  ]
}
)json";

// Sliceable data — served by the "Access data logs" choice after a breach
static const char T_LORE1[] = R"lore(TITLE: Docking Bay Power Allocation
CLASS: IMPERIAL MAINTENANCE // NODE 77-B
DATE: 3 ABY
---
Sliced from the junction's duty log:

CYCLE 112: Clamp power rerouted twice this
week on verbal orders only. No requisition
forms filed. Someone is keeping a berth
energized off the books.

CYCLE 115: Floodlight ring 3 disabled for
"maintenance" during the night shift. The
bay cameras lost coverage of berth 7 for
two full hours.

CYCLE 118: Note from the duty tech: if the
grid drops again, the backup spin-up code
is 2187. Do not share with contractors.
---
END OF LOG
)lore";

// RGB strip pattern — cold Imperial grid tones
static const char T_LIGHTS[] = R"txt(# PANEL_02 - Docking Bay Power Junction strip
chip=ws2812
order=GRB
pin=21
# RRGGBB,on_ms,off_ms  (on 0 = solid, on -1 = flicker)
3388FF,0,0
AAC8FF,0,0
3388FF,800,500
FFFFFF,-1,0
3388FF,0,0
AAC8FF,600,900
3388FF,0,0
FFFFFF,0,0
)txt";

#else   // PANEL_01 (default panel build)

static const char T_CONFIG[] = R"json({
  "$schema": "sfge-config-v1",
  "scenario": {"name": "Tatooine Run", "version": "0.1-demo"},
  "prop": {
    "type": "panel",
    "id": "PANEL_01",
    "ssid": "SFGE_PANEL_01",
    "wifi_channel": 1,
    "name": "Cantina Comm Board",
    "faction": "neutral",
    "location_hint": "Outpost 77 Cantina dome"
  },
  "hardware": {
    "has_nfc_reader": false,
    "has_leds": false,
    "has_buzzer": true,
    "buzzer_pin": 4,
    "aux_mode": "buttons"
  },
  "behavior": {
    "buttons": [
      { "event": "btn_door_cycle",  "name": "DOOR CONTROL" },
      { "event": "btn_power_relay", "name": "POWER RELAY" }
    ],
    "button_cooldown_ms": 1500,
    "idle_animation": "cantina_pulse",
    "requires_auth_card": false,
    "slice_first": true,
    "minigame_default": "simon",
    "simon_rounds": 3,
    "proximity_enabled": true,
    "proximity_rssi_start": -60,
    "proximity_rssi_drop": -72,
    "proximity_grace_ms": 2000,
    "alarm_after_fails": 2,
    "alarm_timeout_s": 90,
    "alarm_siren_period_ms": 350,
    "timer_visual": true,
    "sabotage_enabled": true,
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

// RGB strip pattern (see sfge_lights.h for the format)
static const char T_LIGHTS[] = R"txt(# PANEL_01 - Cantina Comm Board strip
chip=ws2812
order=GRB
pin=21
# RRGGBB,on_ms,off_ms  (on 0 = solid, on -1 = flicker)
FFAA00,0,0
CC8800,0,0
FFAA00,900,600
CC8800,0,0
FF6600,-1,0
CC8800,0,0
FFAA00,700,500
CC8800,0,0
)txt";

#endif  // SFGE_PANEL2

static const TestFile FILES[] = {
    { "/SFGE/config.json",            T_CONFIG,   true },
    { "/SFGE/dialogue/greeting.json", T_GREETING, true },
    { "/SFGE/lore/l001.txt",          T_LORE1,    true },
    { "/SFGE/lights.txt",             T_LIGHTS,   true },
};

// ═══════════════════════════════════════
//  TERMINAL — Repair Shop Decoder (TERM_01), or with the SFGE_TERM2 build
//  flag, Rebel Safehouse Uplink (TERM_02, env:terminal2)
// ═══════════════════════════════════════
#elif defined(SFGE_DEVICE_TERMINAL)

#if defined(SFGE_TERM2)

static const char T_CONFIG[] = R"json({
  "$schema": "sfge-config-v1",
  "scenario": {"name": "Tatooine Run", "version": "0.1-demo"},
  "prop": {
    "type": "terminal",
    "id": "TERM_02",
    "ssid": "SFGE_TERM_02",
    "wifi_channel": 1,
    "name": "Rebel Safehouse Uplink",
    "faction": "rebel",
    "location_hint": "Behind the storage facility, under the awning"
  },
  "hardware": {
    "has_nfc_reader": true,
    "has_buzzer": true,
    "buzzer_pin": 4,
    "nfc_primary_role": "cards",
    "nfc_secondary_role": "cylinder"
  },
  "behavior": {
    "scan_cooldown_ms": 1500,
    "require_tap_to_use": true,
    "session_timeout_s": 60,
    "decrypt_required": false,
    "sabotage_enabled": true,
    "dialogue_greeting": "dialogue/greeting.json"
  }
}
)json";

static const char T_GREETING[] = R"json({
  "type": "dialogue",
  "speaker": {"name": "Rebel Safehouse Uplink", "faction": "rebel"},
  "lines": [
    {"text": "UPLINK LIVE // FRIENDS OF THE ALLIANCE ONLY", "style": "system"},
    {"text": "A battered field transceiver, wired into a crate of spare parts. Someone has scratched a starbird into the casing.", "style": "narration"},
    {"text": "Cards read here go straight to Alliance intel -- no decryption delay, no questions.", "style": "info"}
  ],
  "choices": [
    {"label": "[Disconnect]", "next_action": null}
  ]
}
)json";

static const char T_SHOP[] = R"json({
  "$schema": "sfge-shop-v1",
  "items": [
    { "id": "JAMMER", "name": "SIGNAL JAMMER (ALLIANCE RATE)",
      "cost": 250, "effect": "next_hostile_as_neutral" },
    { "id": "INFORMANT_TIP", "name": "INFORMANT TIP",
      "cost": 100, "effect": "reveal_bounty_clue" }
  ]
}
)json";

// RGB strip pattern — rebel ember-and-white
static const char T_LIGHTS[] = R"txt(# TERM_02 - Rebel Safehouse Uplink strip
chip=ws2812
order=GRB
pin=21
# RRGGBB,on_ms,off_ms  (on 0 = solid, on -1 = flicker)
FF4400,0,0
FFFFFF,900,700
FF4400,-1,0
FF8844,0,0
FF4400,600,400
FFFFFF,0,0
)txt";

#else   // TERM_01 (default terminal build)

static const char T_CONFIG[] = R"json({
  "$schema": "sfge-config-v1",
  "scenario": {"name": "Tatooine Run", "version": "0.1-demo"},
  "prop": {
    "type": "terminal",
    "id": "TERM_01",
    "ssid": "SFGE_TERM_01",
    "wifi_channel": 1,
    "name": "Repair Shop Decoder",
    "faction": "rebel",
    "location_hint": "Speeder repair shop, attached to cantina"
  },
  "hardware": {
    "has_nfc_reader": true,
    "has_buzzer": true,
    "buzzer_pin": 4,
    "nfc_primary_role": "cards",
    "nfc_secondary_role": "cylinder"
  },
  "behavior": {
    "scan_cooldown_ms": 1500,
    "accepts_datatapes": true,
    "datatape_action": "decrypt",
    "require_tap_to_use": true,
    "session_timeout_s": 60,
    "allow_queue": false,
    "decrypt_required": true,
    "decrypt_tries": 6,
    "decrypt_glyphs": 6,
    "sabotage_enabled": true,
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

static const char T_SHOP[] = R"json({
  "$schema": "sfge-shop-v1",
  "items": [
    { "id": "JAMMER", "name": "SIGNAL JAMMER",
      "cost": 300, "effect": "next_hostile_as_neutral" },
    { "id": "INFORMANT_TIP", "name": "INFORMANT TIP",
      "cost": 100, "effect": "reveal_bounty_clue" }
  ]
}
)json";

// RGB strip pattern (see sfge_lights.h for the format)
static const char T_LIGHTS[] = R"txt(# TERM_01 - Repair Shop Decoder strip
chip=ws2812
order=GRB
pin=21
# RRGGBB,on_ms,off_ms  (on 0 = solid, on -1 = flicker)
00FF40,0,0
FFAA00,800,400
00FF40,-1,0
FFAA00,500,900
00FF40,0,0
CC8800,1200,300
)txt";

#endif  // SFGE_TERM2

static const TestFile FILES[] = {
    { "/SFGE/config.json",            T_CONFIG,   true },
    { "/SFGE/dialogue/greeting.json", T_GREETING, true },
    { "/SFGE/shop.json",              T_SHOP,     true },
    { "/SFGE/lights.txt",             T_LIGHTS,   true },
};

// ═══════════════════════════════════════
//  DROID — R5-D8 (DROID_R5D8), or with the SFGE_DROID2 build flag,
//  C1-1U17K (env:droid2) — a scrappy C1-series with a restless head
// ═══════════════════════════════════════
#elif defined(SFGE_DEVICE_DROID)

#if defined(SFGE_DROID2)

static const char T_CONFIG[] = R"json({
  "$schema": "sfge-config-v1",
  "scenario": {"name": "Tatooine Run", "version": "0.1-demo"},
  "prop": {
    "type": "droid",
    "id": "DROID_C11U17K",
    "ssid": "SFGE_DROID_C1",
    "wifi_channel": 1,
    "name": "C1-1U17K",
    "faction": "rebel",
    "location_hint": "Docking bay, parked by the fuel drums"
  },
  "hardware": {
    "has_nfc_reader": true,
    "has_leds": true,
    "has_buzzer": true,
    "buzzer_pin": 4
  },
  "behavior": {
    "restraining_bolt": { "enabled": true, "grace_ms": 3000 },
    "startup_tune": true,
    "startup_head_shakes": 3,
    "idle_animation": "astromech_idle",
    "minigame_default": "purge",
    "purge_targets": 10,
    "purge_time_s": 40,
    "proximity_enabled": true,
    "proximity_rssi_start": -60,
    "proximity_rssi_drop": -72,
    "proximity_grace_ms": 2000,
    "mood_enabled": true,
    "mood_decay_s": 90,
    "mood_colors": { "hostile": "FF2000", "neutral": "FF8800", "friendly": "FFC400" },
    "eye_pixel": 0,
    "servo": {
      "enabled": true,
      "pin": 5,
      "min_us": 600,
      "max_us": 2400,
      "min_deg": 20,
      "max_deg": 160,
      "idle_s": 5
    },
    "dialogue_greeting": "dialogue/greeting.json"
  }
}
)json";

static const char T_GREETING[] = R"json({
  "type": "dialogue",
  "speaker": {"name": "C1-1U17K", "faction": "rebel"},
  "lines": [
    {"text": "WAAAAH-bwop-bwop! (Translation: WHAT. I'm busy.)", "style": "droid"},
    {"text": "A dented C1-series astromech, paint scorched orange. Its head keeps swiveling like it expects trouble from every direction at once.", "style": "narration"},
    {"text": "Dock gossip, fuel levels, who parked at berth 7 -- this unit has seen everything and respects none of it.", "style": "info"}
  ],
  "choices": [
    {"label": "[Disconnect]", "next_action": null}
  ]
}
)json";

// RGB strip pattern — C1 orange/yellow, eye = pixel 0 (mood overrides it)
static const char T_LIGHTS[] = R"txt(# DROID_C11U17K - C1 astromech dome/body lights
chip=ws2812
order=GRB
pin=21
# RRGGBB,on_ms,off_ms  (on 0 = solid, on -1 = flicker)
FF8800,-1,0
FFC400,500,350
FF4400,0,0
FFC400,800,600
FF8800,300,700
FFFFFF,0,0
)txt";

#else   // DROID_R5D8 (default droid build)

static const char T_CONFIG[] = R"json({
  "$schema": "sfge-config-v1",
  "scenario": {"name": "Tatooine Run", "version": "0.1-demo"},
  "prop": {
    "type": "droid",
    "id": "DROID_R5D8",
    "ssid": "SFGE_DROID_R5",
    "wifi_channel": 1,
    "name": "R5-D8",
    "faction": "rebel",
    "location_hint": "Storage facility (the generic building)"
  },
  "hardware": {
    "has_nfc_reader": true,
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
    "proximity_enabled": true,
    "proximity_rssi_start": -60,
    "proximity_rssi_drop": -72,
    "proximity_grace_ms": 2000,
    "mood_enabled": true,
    "mood_decay_s": 120,
    "mood_colors": { "hostile": "FF2000", "neutral": "FFAA00", "friendly": "2060FF" },
    "eye_pixel": 0,
    "servo": {
      "enabled": true,
      "pin": 5,
      "min_us": 600,
      "max_us": 2400,
      "min_deg": 25,
      "max_deg": 155,
      "idle_s": 8
    },
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

// RGB strip pattern (see sfge_lights.h for the format)
static const char T_LIGHTS[] = R"txt(# DROID_R5D8 - astromech dome/body lights
chip=ws2812
order=GRB
pin=21
# RRGGBB,on_ms,off_ms  (on 0 = solid, on -1 = flicker)
FF2000,-1,0
00AAFF,400,300
FFFFFF,0,0
00AAFF,650,450
FF2000,300,900
0055FF,0,0
)txt";

#endif  // SFGE_DROID2

static const TestFile FILES[] = {
    { "/SFGE/config.json",            T_CONFIG,   true },
    { "/SFGE/dialogue/greeting.json", T_GREETING, true },
    { "/SFGE/lights.txt",             T_LIGHTS,   true },
};

// ═══════════════════════════════════════
//  GUILD MASTER — Bounty Hunters' Guild (GUILD_01)
// ═══════════════════════════════════════
#elif defined(SFGE_DEVICE_GUILD)

static const char T_CONFIG[] = R"json({
  "$schema": "sfge-config-v1",
  "scenario": {"name": "Tatooine Run", "version": "0.1-demo"},
  "prop": {
    "type": "guild",
    "id": "GUILD_01",
    "ssid": "SFGE_GUILD_01",
    "wifi_channel": 1,
    "name": "Guild Master Vex",
    "faction": "neutral",
    "location_hint": "Bounty Hunters' Guild table, cantina back room"
  },
  "hardware": {
    "has_nfc_reader": false,
    "has_speaker": true,
    "has_eyes": true
  },
  "behavior": {
    "proximity_enabled": true,
    "proximity_rssi_start": -62,
    "proximity_rssi_drop": -72,
    "proximity_grace_ms": 1500,
    "volume": 18,
    "audio_node": "AUDIO_01",
    "speech_tracks": {
      "boot": 1, "greet": 2, "turnin": 3, "denied": 4,
      "bounty_shilak": 5, "idle_01": 6, "idle_02": 7,
      "bounty_shilak_posted": 8
    }
  }
}
)json";

static const TestFile FILES[] = {
    { "/SFGE/config.json", T_CONFIG, true },
};

// ═══════════════════════════════════════
//  ACTOR TOOL — staff datapad (ACTOR_01)
// ═══════════════════════════════════════
#elif defined(SFGE_DEVICE_ACTOR)

static const char T_CONFIG[] = R"json({
  "$schema": "sfge-actor-v1",
  "device": { "id": "ACTOR_01" },
  "credits": [
    { "label": "REWARD +100",  "delta": 100 },
    { "label": "REWARD +250",  "delta": 250 },
    { "label": "FINE -100",    "delta": -100 }
  ],
  "items": [
    { "id": "JAMMER",        "label": "GIVE: SIGNAL JAMMER" },
    { "id": "INFORMANT_TIP", "label": "GIVE: INFORMANT TIP" }
  ],
  "events": [
    { "id": "actor_datacard_received", "label": "FIRE: DATACARD RECEIVED", "severity": 0 },
    { "id": "imperial_patrol",         "label": "FIRE: IMPERIAL PATROL",   "severity": 2 }
  ]
}
)json";

static const TestFile FILES[] = {
    { "/SFGE/actor.json", T_CONFIG, true },
};

#else
#error "sfge_test_configs.h: no SFGE_DEVICE_* build flag defined"
#endif

// Write this device's embedded test files to `fs` (SD, SD_MMC, or LittleFS).
// Call after the filesystem is mounted, before the normal config load.
inline void writeTestConfigs(fs::FS &fs) {
    Serial.println("[TEST] SFGE_WRITE_TEST_CONFIGS active — writing embedded configs");
    fs.mkdir("/SFGE");
#if defined(SFGE_DEVICE_PANEL) || defined(SFGE_DEVICE_TERMINAL) || defined(SFGE_DEVICE_DROID)
    fs.mkdir("/SFGE/dialogue");
#endif
#if defined(SFGE_DEVICE_PANEL)
    fs.mkdir("/SFGE/lore");
#endif
#if defined(SFGE_DEVICE_GUILD)
    fs.mkdir("/SFGE/speech");   // WAV voice lines go here (can't be embedded)
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

// Direct access to this device's embedded files — lets a prop with a dead
// or missing SD slot FORCE the demo config from flash (testing only; the
// go-live build below removes this, restoring the strict cards-only rule).
inline const char* embeddedFile(const char *path) {
    for (const TestFile &tf : FILES)
        if (strcmp(tf.path, path) == 0) return tf.content;
    return nullptr;
}

#else   // SFGE_WRITE_TEST_CONFIGS not defined — go-live: no-op

inline void writeTestConfigs(fs::FS &) {}
inline const char* embeddedFile(const char *) { return nullptr; }

#endif  // SFGE_WRITE_TEST_CONFIGS

} // namespace sfge_test
