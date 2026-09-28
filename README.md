# SWTS — Star Wars: Twin Suns

Location-based prop game. Players carry rented **datapads** (ESP32-S3 handhelds)
around a physical space, scanning NFC tags and interacting with networked props —
wall **panels**, NFC **terminals**, and astromech **droids** — while a **Game
Master** runs the session from a 7" touchscreen console. Missions branch by
faction, hacking is played as minigames, and a session ends with an extraction
endgame and a scored debrief.

Business model: the datapads and props are rented hardware. Everything is
configured from SD cards so devices can be re-themed between events without
reflashing.

---

## 1. Devices & firmware environments

One PlatformIO project, five firmware targets (same repo, `src/`):

| Env          | Device                | Hardware                                   | Source        |
|--------------|-----------------------|--------------------------------------------|---------------|
| `datapad`    | Player handheld       | ESP32-S3 N16R8, ST7796 320x480 + FT6336 touch, PN532 NFC, SD, buzzer, 3 arcade buttons | `main.cpp`    |
| `gm_datapad` | Game Master console   | ESP32-S3 N4R8, 7" 800x480 RGB + GT911 touch, SD_MMC | `gm.cpp`      |
| `panel`      | Wall panel (headless) | ESP32-S3, PN532 NFC, SD → runs from LittleFS, buzzer, RGB strip | `panel.cpp`   |
| `terminal`   | NFC kiosk (headless)  | ESP32-S3, PN532 NFC, SD, buzzer, RGB strip | `terminal.cpp`|
| `droid`      | Astromech prop        | ESP32-S3, SD, buzzer, eye LED, RGB strip   | `droid.cpp`   |

Build / flash:

```
pio run -e <env>                          # build
pio run -e <env> -t upload --upload-port COMx
```

Dev bench port map: datapad COM4, GM COM5, terminal COM6, panel COM7, droid COM8.

### Datapad pins (ESP32-S3 DevKitC-1)

| Function | Pins |
|---|---|
| Display SPI | SCK 40, MOSI 41, MISO 39, CS 1, DC 42, RST 2 |
| Touch I2C0 (FT6336) | SDA 47, SCL 38, RST 48, INT 21 |
| NFC I2C1 (PN532) | SDA 14, SCL 13 |
| SD (HSPI) | CS 15, MOSI 16, CLK 17, MISO 18 |
| Buzzer | 4 |
| Buttons (LED / switch) | Blue 5/6, White 7/8, Red 9/10 — physical order on board: White, Blue, Red |

### Prop pins (panel / terminal / droid)

| Function | Pins |
|---|---|
| SD (HSPI) | CS 15, MOSI 16, CLK 17, MISO 18 |
| Buzzer (passive piezo) | 4 |
| **RGB strip data** | **21** (overridable in `lights.txt`) |
| Panel NFC I2C | SDA 38, SCL 39 |
| Terminal NFC I2C | SDA 14, SCL 13 |
| Droid eye LED | 5 |

---

## 2. Architecture

### Mesh (ESP-NOW, channel 1) — `swts_mesh.h`

All devices share a broadcast mesh. Key message types:

| Type | Direction | Purpose |
|---|---|---|
| `MSG_PING` / `MSG_STATUS` | device → GM | Heartbeat; status carries score, XP, missions active/done, scans, minigames won, faction, kid mode |
| `MSG_SCORE` / `MSG_NFC_SCAN` / `MSG_SLICE_RESULT` | device → GM | Gameplay reports (feed the GM's per-player analytics log) |
| `MSG_EVENT` | any → all | Game events (`panel_sliced:PANEL_01`, `droid_handoff:…`, `nfc:<TERM>:<CARD>`, `extraction:…`, `game_complete`); optional faction scope; payload = acting player's callsign |
| `MSG_COMM` / `MSG_ALERT` / `MSG_BOUNTY*` | GM → datapads | Messages, alerts, bounty flow. Comm target: `""` = all, `<callsign>` = one player, `@FACTION` = one faction |
| `MSG_ASSIGN` / `MSG_PLAYER_MODE` | GM → datapad | Rename a datapad; set ADULT/KID difficulty |
| `MSG_FACTIONS` | GM → all | Scenario faction roster (drives the datapad's allegiance screen) |
| `MSG_SYNC_REQUEST` / `MSG_RESET` | GM → all | Re-report status; reboot |

### Prop HTTP API

Each prop is a WiFi AP (`SWTS_<ID>`). Datapads join and POST JSON to
`/api/interact` (`GET /api/info` for identity). Requests carry
`player.callsign` and `player.faction`. Responses are dialogue trees
(`lines` + `choices` with `next_action`), minigame starts, or lore. A response
may include `game_event` — the datapad queues it to advance missions and
trigger comms.

### Storage layout (per device SD, `/SWTS/`)

| File | Device | Purpose |
|---|---|---|
| `config.json` | all | Identity, scenario, behavior (minigame type, rounds/targets, faction, …) |
| `missions.json` | datapad | Missions + NFC trigger table |
| `comms.json` | datapad | Story messages with triggers + faction tags |
| `nfc_map.json` | datapad | Tag UID → content map |
| `player.json` | datapad | Player state: callsign, faction, mode, score/XP, mission progress (survives reboot; delete to reset a player) |
| `gm_config.json` / `gm_state.json` | GM | Scenario, factions, events/comms/bounty templates; live session state |
| `lights.txt` | props | RGB strip configuration (see §6) |
| `dialogue/`, `lore/` | props | Greeting dialogue, sliceable data files |

The panel copies `/SWTS/` from SD to LittleFS at boot and runs from flash.
Scenario masters live in `scenarios/<name>/<device>/SWTS/`.

**Test provisioning:** while `SWTS_WRITE_TEST_CONFIGS` is defined in
`src/swts_test_configs.h`, every device writes embedded copies of the
twin_suns_demo configs to its storage at boot (`player.json` is preserved).
Comment out that one `#define` and rebuild to go live on real cards.

---

## 3. Creating the JSON files

All content is plain JSON on the SD cards. Copy an existing scenario
(`scenarios/twin_suns_demo/`) as a starting point, then edit. General rules:

- Unknown keys are ignored; missing keys fall back to firmware defaults.
- The `// comments` in the examples below are annotations for this document
  only — real JSON files must not contain them (the parser will reject the file).
- Faction names must match the roster in `gm_config.json` (compared
  case-insensitively). An empty/omitted `faction` means "everyone".
- NFC tags must be written as **NDEF text records** whose text is the token
  the JSON refers to (`INTEL_01`, `DT_VAULT`, `EXTRACT_01`, `CARGO_01`, …).
  The reader matches on that text, not the tag UID.

### 3.1 Datapad `config.json`

```json
{
  "$schema": "swts-config-v1",
  "scenario": {
    "name": "Tatooine Run",       // display only
    "version": "0.1-demo",
    "planet": "Outpost 77",       // shown in the home header
    "currency": "CR"              // score suffix: CR, RP, ...
  }
}
```

### 3.2 Prop `config.json` (panel / terminal / droid)

Common shape:

```json
{
  "$schema": "swts-config-v1",
  "scenario": {"name": "Tatooine Run", "version": "0.1-demo"},
  "prop": {
    "type": "panel",              // panel | terminal | droid (informational)
    "id": "PANEL_01",             // unique — used in events (panel_sliced:PANEL_01)
    "ssid": "SWTS_PANEL_01",      // WiFi AP name the datapad sees
    "wifi_channel": 1,            // keep 1 — must match the mesh channel
    "name": "Cantina Comm Board", // shown in dialogue headers
    "faction": "neutral",         // neutral | REBEL | IMPERIAL | ... (drives
                                  // friendly/hostile minigame difficulty)
    "location_hint": "Twin Suns Cantina dome"
  },
  "hardware": { "has_nfc_reader": false, "has_buzzer": true, "buzzer_pin": 4 },
  "behavior": { }                 // per prop type, below
}
```

`behavior` keys by prop type:

| Prop | Key | Meaning |
|---|---|---|
| panel | `minigame_default` | `"simon"` or `"slice"` — omit for no minigame |
| panel | `simon_rounds` | Simon rounds to win (default 3) |
| panel | `slice_first` | `true` = data locked until sliced (defaults to true when a minigame is set) |
| panel | `requires_auth_card` / `auth_card_ids` | optional NFC auth gate |
| terminal | `scan_cooldown_ms` | per-card rescan cooldown (default 1500) |
| droid | `minigame_default` | `"purge"` — gate the intel handoff behind Core Purge |
| droid | `purge_targets` / `purge_time_s` | blocks to clear / time limit (12 / 35) |

### 3.3 `missions.json` (datapad)

```json
{
  "$schema": "swts-missions-v1",
  "missions": [
    {
      "id": "intel_run",                  // referenced by unlocks/triggers/comms
      "title": "INTEL RUN",               // <= 23 chars
      "subtitle": "One-line description", // <= 79 chars
      "faction": "REBEL",                 // "", "ALL" = every faction
      "briefing": "Multi-line text.\n\nShown on the missions screen.",  // <= 319 chars
      "difficulty": 1,
      "reward_credits": 250,              // score awarded on completion
      "reward_xp": 100,
      "reward_item": "ALLIANCE COMMENDATION",
      "unlocks": "extraction",            // mission id auto-unlocked+started on completion ("" = none)
      "starts_unlocked": true,            // auto-starts at boot (faction permitting)
      "endgame": false,                   // true = completion triggers the debrief screen
      "steps": [                          // max 6, done in order
        {
          "title": "Pick up the Datacard",
          "description": "Player-facing objective text.",
          "type": "scan_nfc",             // scan_nfc | event
          "target": "INTEL_01",           // NDEF token, or event id (<= 35 chars)
          "xp": 30                        // XP for this step
        },
        {
          "title": "Decrypt at the Repair Shop",
          "description": "Carry DT_VAULT to the terminal.",
          "type": "event",
          "target": "nfc:TERM_01:DT_VAULT",
          "xp": 60
        }
      ]
    }
  ],
  "nfc_triggers": [                       // max 16 — how scans touch missions
    { "token": "INTEL_01", "action": "step",  "mission": "intel_run", "step": 0 },
    { "token": "GO_CARD",  "action": "start", "mission": "intel_run" }
  ]
}
```

Event ids you can use as `event` step targets:

| Event | Fired when |
|---|---|
| `nfc:<TERM_ID>:<TOKEN>` | a card is scanned on that terminal |
| `panel_sliced:<PANEL_ID>` | a panel's minigame is won |
| `extraction:<PANEL_ID>` | the post-slice "Broadcast extraction signal" action |
| `droid_handoff:<DROID_ID>` | intel delivered to that droid |
| `droid_slice:<DROID_ID>` | a droid's Core Purge is won |
| any GM event template id | the GM taps it on the EVENTS tab |

Limits: 8 missions, 6 steps each, 16 triggers.

### 3.4 `comms.json` (datapad)

```json
{
  "$schema": "swts-comms-v1",
  "messages": [
    {
      "id": "assignment",                    // unique, <= 23 chars
      "from": "ALLIANCE COMMAND",            // <= 23 chars
      "subject": "ASSIGNMENT: INTEL RUN",    // <= 39 chars
      "body": "Message text.",               // <= 255 chars
      "trigger": "mission_start:intel_run",  // when it lands in the inbox
      "faction": "REBEL"                     // omit for everyone
    }
  ]
}
```

Trigger vocabulary (each message delivers once):

| Trigger | Fires |
|---|---|
| `boot` | at power-on (and again after the faction pick) |
| `mission_start:<id>` | when that mission starts |
| `scan:<TOKEN>` | when the player scans that tag themselves |
| `event:<event_id>` | on any game event (see §3.3 table) |
| `manual` | never automatically (reserved) |

Limit: 16 messages.

### 3.5 `nfc_map.json` (datapad)

UID → content catalog for datacards. Currently informational (gameplay keys off
NDEF text tokens) — keep it updated as documentation of which physical tags
exist and what text they carry.

```json
{
  "$schema": "swts-nfc-map-v1",
  "tags": {
    "01:23:45:67": {
      "type": "datacard",
      "id": "INTEL_01",
      "name": "Rebel Intel Drop",
      "category": "mission_trigger",
      "description": "Encrypted intel package."
    }
  }
}
```

### 3.6 `player.json` (datapad)

Firmware-managed save file; ship the starting template and let the datapad
maintain it. Delete it from the card to reset the datapad to the registration
flow (name + faction).

```json
{
  "$schema": "swts-player-v1",
  "callsign": "OPERATIVE",   // placeholder -> triggers the name-entry screen
  "score": 0, "xp": 0, "totalScans": 0,
  "missions": [], "comms_read": [], "bounties_won": []
}
```

(`faction`, `mode` (ADULT/KID), `slicesWon` and mission progress are written by
the firmware as the player plays.)

### 3.7 `gm_config.json` (Game Master)

```json
{
  "$schema": "swts-gm-config-v1",
  "scenario": { "name": "Tatooine Run", "version": "0.1-demo",
                "planet": "Outpost 77", "currency": "CR" },
  "device":   { "id": "GM-1", "role": "gamemaster" },
  "factions": ["REBEL", "IMPERIAL"],        // 1-6 entries, <= 13 chars each;
                                            // broadcast to datapads for the pick screen
  "game":     { "max_score": 1000 },        // faction progress-bar goal

  "events": [                               // EVENTS tab buttons -> MSG_EVENT
    { "id": "imperial_patrol",              // event id (usable as mission step target)
      "name": "IMPERIAL PATROL",
      "severity": 2,                        // 0 info / 1 warning / 2+ critical
      "description": "Shown on the card and sent as payload.",
      "faction": "REBEL" }                  // omit = all players react
  ],

  "comms": [                                // COMMS tab templates -> MSG_COMM
    { "id": "gm_patrol_warn", "from": "OUTPOST SECURITY",
      "subject": "PATROL WARNING", "body": "Message text.",
      "faction": "IMPERIAL" }               // omit = broadcast to everyone
  ],

  "bounties": [                             // BOUNTIES tab
    { "id": "bounty_greedo", "target_name": "Greedo",
      "description": "Short wanted text.", "reward": 300,
      "clues": ["Clue 1.", "Clue 2."] }     // max 5, revealed one at a time
  ]
}
```

Limits: 16 each of events/comms/bounties. `gm_state.json` is written by the GM
itself (bounty state + last-known roster) — never author it by hand.

---

## 4. Gameplay

### Registration (rental handout flow)

1. Fresh datapad boots to **ALLIANCE REGISTRY** → player types a callsign
   (on-screen keyboard, 12 chars, becomes the mesh ID).
2. **DECLARE ALLEGIANCE** → player picks a faction from the GM's roster
   (defaults REBEL / IMPERIAL).
3. Faction-matching missions auto-start, briefing comms arrive, home screen loads.
   The footer shows the declared faction.

### Missions (`missions.json`)

Step types:
- `scan_nfc` — advance when the datapad scans a tag whose NDEF text matches `target`.
- `event` — advance when a matching game event arrives (mesh broadcast or prop response).
- Missions have `faction` (empty/`ALL` = everyone), `unlocks` (chains auto-start),
  and `endgame: true` (completion shows the **debrief screen**: rank by score,
  final stats, fanfare, `game_complete` event to the GM).

Twin Suns demo arc — Rebel: INTEL RUN (scan INTEL_01 → decrypt DT_VAULT at the
terminal → purge R5's core + hand off) then EXTRACTION (slice the panel, broadcast
the signal, scan the EXTRACT_01 beacon). Imperial: COUNTER-SLICE (breach the
panel, seize the INTEL_01 dead drop).

### Minigames (hacking)

| Game | Where | Play |
|---|---|---|
| Timing-bar slice | touchscreen | Tap when the cursor crosses target zones |
| Simon "Pattern Lock" | physical buttons + LEDs | Repeat a growing color sequence |
| Core Purge (defrag) | touchscreen | Tap flashing ERR blocks on a 4x4 grid, avoid SYS decoys, 3 faults out |

Difficulty modifiers (stack):
- **Faction**: friendly prop easier (−1 round / −1 difficulty / −4 targets +10s),
  hostile harder (+2 rounds / +1 difficulty / +4 targets −5s). Neutral props unchanged.
- **KID mode** (GM-set per player): one notch/round easier, slower Simon playback,
  longer input windows, fewer purge targets, longer block lifetimes.

### Game Master console

Tabs: HOME (leaderboard, panel status, activity feed), EVENTS (faction-scopable
triggers), COMMS (templates + compose; target ALL / a faction / one player),
BOUNTIES, PLAYERS (tap a player for full stats, interaction log, KID/ADULT
toggle; UNASSIGNED list for naming datapads), FACTIONS (per-faction totals and
progress vs the scenario score goal), PANELS.

---

## 5. Interaction feedback (lights + buzzer)

Prop actions interrupt the idle light pattern with a whole-strip flash and a
buzzer phrase, then the idle pattern resumes:

| Trigger | Lights | Sound |
|---|---|---|
| Greet / menu / minigame start / log read | amber double-blink | click |
| Minigame won | green celebration flash | rising chime |
| Minigame failed | red flash | low buzz |
| Extraction signal / intel handoff | cyan transmit strobe | rising triple |
| Terminal scan accepted / rejected | green / red flash | granted / denied tones |

(Prop HTTP handlers run on the WiFi task; effects are queued and fired from the
main loop so LED timing and `tone()` stay safe.)

---

## 6. Lights configuration (`/SWTS/lights.txt`)

Addressable RGB strips (WS2812-class: 5V, GND, DATA) on panel, terminal, and
droid. **DATA wires to GPIO 21** by default. Implementation: `src/swts_lights.h`
(Adafruit NeoPixel, runtime-configurable chip and color order, max 64 LEDs).

The file lives on the prop's SD card (the panel picks it up through its normal
SD → LittleFS provisioning). No file = strip stays off.

### Format

```
# comments start with #
chip=ws2812          # ws2812 | ws2811 | sk6812   (ws2811 runs at 400 kHz)
order=GRB            # RGB | RBG | GRB | GBR | BRG | BGR
pin=21               # optional — data GPIO override

# Then ONE LINE PER LED, in strip order:
# RRGGBB,on_ms,off_ms
FFAA00,0,0           # on_ms 0  -> always on (solid)
FF6600,-1,0          # on_ms -1 -> random flicker (faulty cell / fire)
00AAFF,400,300       # otherwise -> blink: 400 ms lit, 300 ms dark
```

### Rules

1. `chip`, `order`, `pin` may appear in any order but must come before use;
   anything that isn't a `key=value` line or comment is parsed as an LED line.
2. LED lines are positional — line N drives pixel N. The LED count is simply
   the number of LED lines (max 64).
3. Color is 6-digit hex `RRGGBB` (logical RGB — the `order` setting handles the
   strip's wire order, so you never rewrite colors for GRB strips).
4. Timing semantics per LED:
   - `on_ms == 0` → solid, always on.
   - `on_ms == -1` (or `off_ms == -1`) → random flicker: brightness wanders
     25–100 % with occasional wink-outs, re-rolled every 50–190 ms.
   - otherwise → blink, `on_ms` lit then `off_ms` dark (values under 20 ms are
     clamped to 20).
5. Interaction effects (§5) temporarily override the whole strip, then every
   LED resumes its configured behavior.
6. Edit the file on the card and reboot the prop to apply. For the panel,
   insert the card so boot provisioning copies it to flash.

### Shipped patterns (twin_suns_demo)

- `PANEL_01` — 8 LEDs: warm cantina ambers, one slow pulse, one flickering
  faulty cell.
- `TERM_01` — 6 LEDs: solid greens with amber data-activity blinks and one
  green flicker.
- `DROID_R5D8` — 6 LEDs: red processor flicker, blue status blinks, solid
  white running light.

---

*Written for: anyone building, configuring, or theming SWTS devices.*
