# SWTS — Sallek demo feature additions

Companion to the root `README.md`. This is the feature spec for the Sallek
demo wave, with implementation status. Everything is additive; existing
config files keep working unchanged (unknown keys are ignored).

## Implementation status

| Feature | Status | Notes / deviations from the original spec |
|---|---|---|
| RSSI proximity gate (panel, droid, terminal) | **done** | `swts_proximity.h`; server-side 403 `too_far` with hysteresis; datapad mirrors it (aborts minigames as `lost_signal` after a 2.5 s grace below −78 dBm) |
| Tap-to-use sessions (terminal) | **done** | `PLAYER:<callsign>` card opens a session validated against the mesh roster; `session_open/close:<ID>:<cs>` events; second player denied (`allow_queue` not implemented yet) |
| Decrypt gate on terminal scans | **done** | `decrypt_required` holds the scanned card; the `nfc:<TERM>:<CARD>` event fires only after the Decrypt minigame is won |
| Decrypt minigame (Mastermind) | **done** | 4-glyph code (KID: 3 glyphs from 4, 10 tries); prop mode + encrypted-comm mode |
| Encrypted comms | **done** | `"encrypted": true` in comms.json; scrambled body + DECRYPT button; 30 s retry lockout; `comm_decrypted:<id>` event. GM compose encrypt toggle: **v2** (no field left in the comm packet) |
| Player card write at registration | **done** | Writes NDEF text `PLAYER:<callsign>` to NTAG2xx; 20 s timeout or SKIP |
| Alarm state (panel) | **done** | Trips after N consecutive hostile fails/lost-signals; red strobe + siren; friendly hold-5s reset, hostile 1-round Simon bypass; GM CLEAR ALARM on PANELS tab; auto-timeout. Droid alarm: **v2** |
| Countdown / dead man's switch | **done** | `gm_config.json timers[]`; start/cancel buttons on the GM EVENTS tab; broadcast every 5 s; datapad header shows it; props with `timer_visual` red-shift their strip; `on_expire` event fires at zero |
| Droid mood | **done** | −5..+5, persisted to `droid_state.json`, decays; eye = strip pixel 0 color (single-color GPIO eye LED can't do color); greet tone per band (`dialogue/greet_<band>.json` optional); purge targets − mood; refuses at ≤ −4 until "Apologize (50 CR)" |
| Inventory + item cards | **done** | `ITEM:<id>` cards; max 8; DROP/USE screen via home-footer tap; `item_pickup:<id>` events; sent as `player.inventory[]` on every interact |
| Shop (terminal) | **done** | `shop.json`; datapad renders + deducts (it is authoritative for its own score); terminal best-effort validates from the mesh roster; `item_bought:<id>` event |
| JAMMER effect | **done** | Hostile prop treats the holder as neutral; consumed via `consumed_item` in the response |
| INFORMANT_TIP effect | **deviation** | The datapad doesn't hold unrevealed clues (the GM does), so USE broadcasts an `informant_tip` event that prompts the GM to push the next clue |
| Duel (PvP) | **done** | Scan another player's operative card → challenge → seeded reaction race; +300 ms per wrong press; loser pays stake (never below 0); `duel_won:<w>:<l>`; per-pair cooldown; KID players can't initiate and auto-decline |
| Radar | **done** | Promiscuous-RX RSSI paired with mesh frames; NEAR/CLOSE/FAR buckets; faction-colored; RADAR button on the Nearby screen; hidden in KID mode (config `radar_kid_mode`) |
| Keycard auth on panels | **done** | `requires_auth_card` + `auth_card_ids` validated against `player.inventory[]` (panels have no NFC) |
| Sync Slice / Bypass / Hot Wire / Frequency Lock | **v2** | Need a second panel to be worthwhile |
| Sabotage budget, Replay tab, Holo-log | **v2** | |

## New mesh messages

| Type | Direction | Payload |
|---|---|---|
| `MSG_TIMER` | GM → all | id, label, total/remaining s, faction scope, active |
| `MSG_ALARM_CLEAR` | GM → prop | prop id |
| `MSG_DUEL_REQUEST/ACCEPT/RESULT` | datapad ↔ datapad (GM logs results) | duel id, callsigns, stake, seed, time |
| `MSG_STATUS` (extended) | prop → GM | + `mood`, `alarm`, `session` (via `sendPropStatus`) |

## New events (mission step targets / comm triggers)

`alarm:<ID>`, `alarm_clear:<ID>`, `session_open:<ID>:<cs>`,
`session_close:<ID>:<cs>`, `item_pickup:<id>`, `item_bought:<id>`,
`comm_decrypted:<comm_id>`, `duel_won:<winner>:<loser>`, `informant_tip`,
plus timer `on_expire` ids (e.g. `reinforcements_arrive`).

## New config keys

**Prop `behavior`** (panel/droid/terminal as applicable):
`proximity_enabled`, `proximity_rssi_start` (−60), `proximity_rssi_drop`
(−72), `proximity_grace_ms` (2000), `ap_tx_power_dbm`; panel:
`alarm_after_fails` (2), `alarm_timeout_s` (90), `alarm_siren_period_ms`
(350), `timer_visual`; droid: `mood_enabled`, `mood_decay_s` (120),
`mood_colors{hostile,neutral,friendly}`; terminal: `require_tap_to_use`,
`session_timeout_s` (60), `decrypt_required`, `decrypt_tries` (6),
`decrypt_glyphs` (6).

**Datapad `config.json`**: `radar_enabled`, `radar_kid_mode`,
`duel{enabled,targets,stake,cooldown_s}`.

**GM `gm_config.json`**: `timers[{id,label,seconds,on_expire,faction}]`.

**Terminal `shop.json`**: `items[{id,name,cost,effect}]` (max 8).

## RSSI calibration

Indoors on ESP32-S3: roughly −45 dBm ≈ 1 m, −60 ≈ 3 m, −70+ ≈ across a
room; bodies swing readings ±10 dB. Thresholds are per-prop config.
