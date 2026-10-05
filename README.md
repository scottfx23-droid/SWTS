# SFGE — Sci-Fi Game Engine

Engine for location-based prop games. Players carry rented **datapads**
(ESP32-S3 handhelds) around a physical space, scanning NFC tags and
interacting with networked props — wall **panels**, NFC **terminals**,
**droids**, and a **guild master** — while a **Game Master** runs the session
from a 7" touchscreen console. Missions branch by faction, hacking is played
as minigames, and a session ends with an endgame and a scored debrief.

The engine is setting-agnostic: every name, mission, message, voice line and
light pattern comes from the scenario JSON on the SD cards. The bundled
`twin_suns_demo` scenario is a Star Wars-flavored example.

Business model: the datapads and props are rented hardware. Everything is
configured from SD cards so devices can be re-themed between events without
reflashing.

---

## 1. Devices & firmware environments

One PlatformIO project, seven firmware targets (same repo, `src/`):

| Env          | Device                | Hardware                                   | Source        |
|--------------|-----------------------|--------------------------------------------|---------------|
| `datapad`    | Player handheld       | ESP32-S3 N16R8, ST7796 320x480 + FT6336 touch, PN532 NFC, SD, buzzer, 3 arcade buttons | `main.cpp`    |
| `gm_datapad` | Game Master console   | ESP32-S3 N4R8, 7" 800x480 RGB + GT911 touch, SD_MMC | `gm.cpp`      |
| `panel`      | Wall panel (headless) | ESP32-S3, PN532 NFC, SD → runs from LittleFS, buzzer, RGB strip | `panel.cpp`   |
| `terminal`   | NFC kiosk (headless)  | ESP32-S3, PN532 NFC, SD, buzzer, RGB strip | `terminal.cpp`|
| `droid`      | Astromech prop        | ESP32-S3, SD, buzzer, head servo, RGB strip (eye = pixel 0) | `droid.cpp`   |
| `guildmaster`| Bounty Hunters' Guild | Waveshare DualEye-LCD-1.28 (2x GC9A01 eyes, ES8311 speaker, TF) | `guild.cpp`   |
| `actor`      | Actor tool (staff)    | Same hardware as the player datapad        | `actor.cpp`   |

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
| NFC I2C — primary (panel + terminal) | SDA 14, SCL 13 |
| NFC I2C — secondary, terminal code-cylinder reader (optional) | SDA 38, SCL 39 |
| Droid head servo signal (optional) | 5 (configurable; power the servo from 5V, common ground) |

The droid's eye is **pixel 0 of the RGB strip** (configurable via
`behavior.eye_pixel`) — the mood system drives its color. The head servo is
configured under `behavior.servo` (pulse range, travel limits, idle cadence);
it wanders idly and reacts to interactions (happy wiggle on wins, slow shake
on failures).

NFC note: the PN532's I2C address is fixed, so it's one reader per I2C bus —
the ESP32-S3's two controllers give a hard maximum of **two readers per
prop**. The terminal auto-detects the second reader at boot; absent = normal
single-slot operation. The cylinder slot only accepts tags whose NDEF text
starts with `CODE_` (e.g. `CODE_RED`); anything else is rejected with the
denied effect. Accepted scans broadcast `cyl:<TERM_ID>:<CODE_XX>` events
(usable as mission step targets and comm triggers) and log as `cylinder`
scans in the GM analytics.

---

## 2. Architecture

### Mesh (ESP-NOW, channel 1) — `sfge_mesh.h`

All devices share a broadcast mesh. Key message types:

| Type | Direction | Purpose |
|---|---|---|
| `MSG_PING` / `MSG_STATUS` | device → GM | Heartbeat; status carries score, XP, missions active/done, scans, minigames won, faction, kid mode |
| `MSG_SCORE` / `MSG_NFC_SCAN` / `MSG_SLICE_RESULT` | device → GM | Gameplay reports (feed the GM's per-player analytics log) |
| `MSG_EVENT` | any → all | Game events (`panel_sliced:PANEL_01`, `droid_handoff:…`, `nfc:<TERM>:<CARD>`, `extraction:…`, `game_complete`); optional faction scope; payload = acting player's callsign |
| `MSG_EVENT_ACK` | datapad/GM → sender | Receipt confirmation for a game event (see *Reliable events* below) |
| `MSG_COMM` / `MSG_ALERT` / `MSG_BOUNTY*` | GM → datapads | Messages, alerts, bounty flow. Comm target: `""` = all, `<callsign>` = one player, `@FACTION` = one faction. A bounty with `target_player` set is a **manhunt** (see §4) |
| `MSG_AWARD` | actor/GM → datapad | Give a player credits (can be negative) and/or an inventory item; sent 3x with a dedupe code so a lost frame can't eat the award |
| `MSG_ASSIGN` / `MSG_PLAYER_MODE` | GM → datapad | Rename a datapad; set ADULT/KID difficulty |
| `MSG_FACTIONS` | GM → all | Scenario faction roster (drives the datapad's allegiance screen) |
| `MSG_SYNC_REQUEST` / `MSG_RESET` | GM → all | Re-report status; reboot |

**Reliable events.** A missed broadcast must never leave a mission silently
stuck, so `MSG_EVENT` is ack-based. Every event carries a random code;
datapads and the GM answer each code with `MSG_EVENT_ACK` and remember recent
codes so a repeat is never processed twice (no double toasts, buzzes, or log
entries). The sender re-broadcasts the event every 2 s until every datapad/GM
it has heard in the last 30 s has confirmed, giving up after 10 s so a
powered-off datapad can't make an event repeat forever. All of this lives in
`sfge_mesh.h` — senders just call `gmTriggerEvent()`, and every device runs
`eventRetryService()` once per loop pass. As a manual backstop, the GM MENU
has **RE-FIRE EVENT**: the GM keeps the last 8 gameplay events it heard (or
fired), and tapping one re-broadcasts it so an operator can always unstick a
player. Mission steps and comms are idempotent, so a re-fire is safe even for
players who already received the original.

### Prop HTTP API

Each prop is a WiFi AP (`SFGE_<ID>`). Datapads join and POST JSON to
`/api/interact` (`GET /api/info` for identity). Requests carry
`player.callsign` and `player.faction`. Responses are dialogue trees
(`lines` + `choices` with `next_action`), minigame starts, or lore. A response
may include `game_event` — the datapad queues it to advance missions and
trigger comms.

### Storage layout (per device SD, `/SFGE/`)

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

The panel copies `/SFGE/` from SD to LittleFS at boot and runs from flash.
Scenario masters live in `scenarios/<name>/<device>/SFGE/`.

**Test provisioning:** while `SFGE_WRITE_TEST_CONFIGS` is defined in
`src/sfge_test_configs.h`, every device writes embedded copies of the
twin_suns_demo configs to its storage at boot (`player.json` is preserved).
Comment out that one `#define` and rebuild to go live on real cards.

**Firmware updates from the card:** copy a built binary onto the device's
card under its device-specific name and boot — the device flashes itself and
reboots (`.pio/build/<env>/firmware.bin` → `/SFGE/firmware_<device>.bin`,
where `<device>` is `datapad`, `gm`, `panel`, `terminal`, `droid` or
`guild`). The file is renamed before flashing so a power cut can never cause
a flash loop; after the update it becomes `firmware_installed.bin` (or
`firmware_failed.bin`). Fleet updates are a card swap, same as content.

**Session turnover:** GM header → MENU → **NEW GAME** (two confirmations).
Broadcasts a session wipe: every datapad erases its player save and reboots
into registration; props reboot clean (alarms, slices, sessions, moods,
bounty ledgers); the GM clears its roster, analytics, bounty state and
timers. Ten seconds to turn the room over to the next group.

**Battery readout:** datapads run on USB power banks, which expose no charge
level — so the GM estimates remaining power from each datapad's uptime
against `game.battery_budget_min` in gm_config.json (default 300 min). Shown
as `PWR ~NN%` on the roster and player detail, with a LOW POWER alert in the
activity feed at 20%. Set the budget from your measured runtime (a 5000 mAh
bank typically runs a datapad 5-7 h).

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
  "$schema": "sfge-config-v1",
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
  "$schema": "sfge-config-v1",
  "scenario": {"name": "Tatooine Run", "version": "0.1-demo"},
  "prop": {
    "type": "panel",              // panel | terminal | droid (informational)
    "id": "PANEL_01",             // unique — used in events (panel_sliced:PANEL_01)
    "ssid": "SFGE_PANEL_01",      // WiFi AP name the datapad sees
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
  "$schema": "sfge-missions-v1",
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
  "$schema": "sfge-comms-v1",
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
  "$schema": "sfge-nfc-map-v1",
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
  "$schema": "sfge-player-v1",
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
  "$schema": "sfge-gm-config-v1",
  "scenario": { "name": "Tatooine Run", "version": "0.1-demo",
                "planet": "Outpost 77", "currency": "CR" },
  "device":   { "id": "GM-1", "role": "gamemaster" },
  "factions": ["REBEL", "IMPERIAL"],        // 1-6 entries, <= 13 chars each;
                                            // broadcast to datapads for the pick screen
  "faction_colors": ["FF4400", "3388FF"],   // optional, parallel to factions —
                                            // victory strobe color per faction
  "game":     { "max_score": 1000,          // faction progress-bar goal
                "manhunt_reward": 250 },    // bounty on a player (POST MANHUNT)

  "timers": [                               // countdown timers (HOME tab buttons)
    { "id": "extraction_window", "label": "EXTRACTION WINDOW",
      "seconds": 600,
      "on_expire": "reinforcements_arrive", // event fired at zero (optional)
      "faction": "",                        // scope ("" = everyone sees it)
      "declare_victory": true }             // at zero: compare faction score
  ],                                        // totals, broadcast the ending

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

### Manhunt bounties (PvP hide-and-seek)

The GM taps a player on the PLAYERS tab → **POST MANHUNT**. That broadcasts a
bounty whose target is the player's callsign (`game.manhunt_reward`, default
250). Every datapad sees **MANHUNT POSTED** — including the hunted player, who
instead gets a red **YOU ARE WANTED** alert so they know to run. Claiming it =
scanning the hunted player's operative card (capture beats dueling while a
hunt is open); the hunter gets TARGET ACQUIRED, the hunted gets a **CAPTURED**
alert naming the hunter, and the prize is collected at the Guild Master
exactly like a chain-code bounty. The GM can re-post a new hunt on the same
player after one settles.

### Faction victory (countdown ending)

A timer with `"declare_victory": true` turns the scoreboard into an actual
ending: at zero the GM compares faction score totals and broadcasts
`victory_<faction>` (or `victory_draw` on a tie). Every prop strobes in the
winner's color (`faction_colors`, falling back to a built-in palette), the
Guild Master's eyes celebrate, and each faction's datapads receive a scoped
COMMAND comm — triumph for the winners, defeat for the rest. Datapad
`comms.json` entries can add scenario-authored endings via trigger
`event:victory_<faction>` (lowercase).

### Actor tool (`actor` env, spare datapad hardware)

A staff datapad with a cyan UI so it can't be mistaken for a player unit.
The actor scans a player's operative card and gets a configured action menu:
award/deduct credits, hand over a digital item, or fire a story event —
closing the loop on datacards handed to actors without the GM having to
notice. Everything it does is logged in the GM's activity feed and the
player's interaction log. Config `/SFGE/actor.json`:

```json
{
  "$schema": "sfge-actor-v1",
  "device":  { "id": "ACTOR_01" },
  "credits": [ { "label": "REWARD +100", "delta": 100 },
               { "label": "FINE -100",   "delta": -100 } ],
  "items":   [ { "id": "JAMMER", "label": "GIVE: SIGNAL JAMMER" } ],
  "events":  [ { "id": "actor_datacard_received",
                 "label": "FIRE: DATACARD RECEIVED", "severity": 0 } ]
}
```

(Max 8 entries per list. Events ride the reliable event layer, so they can
advance missions via `event` steps and trigger comms like any prop event.)

### Game Master console

Tabs: HOME (leaderboard, panel status, activity feed), EVENTS (faction-scopable
triggers), COMMS (templates + compose; target ALL / a faction / one player),
BOUNTIES, PLAYERS (tap a player for full stats, interaction log, KID/ADULT
toggle, POST MANHUNT; UNASSIGNED list for naming datapads), FACTIONS
(per-faction totals and progress vs the scenario score goal), PANELS.

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
| Victory finale (see below) | rainbow chase + white sparkles, 12 s | C6-E6-G6 fanfare |

**Victory celebration.** When a `game_complete` event fires — or any event
whose id contains `victory` or `celebrate` (the demo GM has REBEL VICTORY and
IMPERIAL VICTORY templates on its EVENTS tab) — every prop in the room
celebrates at once: panel, terminal, and droid run a 12-second rainbow strobe
(the droid adds a happy head wiggle), and the Guild Master's eyes go happy.
An active alarm still outranks the celebration on the panel.

(Prop HTTP handlers run on the WiFi task; effects are queued and fired from the
main loop so LED timing and `tone()` stay safe.)

---

## 6. Lights configuration (`/SFGE/lights.txt`)

Addressable RGB strips (WS2812-class: 5V, GND, DATA) on panel, terminal, and
droid. **DATA wires to GPIO 21** by default. Implementation: `src/sfge_lights.h`
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

*Written for: anyone building, configuring, or theming SFGE devices.*
