# Sci-Fi Game Engine (SFGE) — Feature Catalog

Every gameplay and operations feature in the system, and what it does.
(For wiring, config file formats and protocol details, see the root
`README.md` and `docs/SALLEK_FEATURES.md`.)

---

## 1. Player datapad

### Registration & identity
- **Callsign entry** — a fresh datapad boots to the ALLIANCE REGISTRY screen;
  the player types their operative name on an on-screen keyboard. That name
  becomes their identity everywhere: the mesh, the GM roster, the leaderboard.
- **Operative card** — the datapad then writes the player's name to a blank
  NFC card. The card is their physical identity: it opens terminal sessions
  and lets other players challenge them to duels.
- **Faction selection** — the player declares an allegiance (e.g. REBEL or
  IMPERIAL) from a roster the Game Master's console broadcasts. Their faction
  is shown in the home footer and changes which missions, messages, and
  difficulty they get.
- **Player save file** — everything (name, faction, score, XP, mission
  progress, inventory, read messages) lives on the datapad's SD card and
  survives reboots. Deleting the file resets the datapad for the next renter.

### Missions
- **Mission log** — a live MISSIONS screen showing each operation's status,
  current objective, and description. Missions the player's faction can't see
  stay hidden (no spoilers).
- **Faction-branched campaigns** — missions carry a faction tag, so Rebels
  and Imperials play different story arcs through the same physical props.
- **Objective types** — steps advance by *scanning an NFC tag* (find the
  hidden datacard) or by *game events* (a terminal decrypted your card, a
  droid accepted your intel, a panel was sliced...).
- **Mission chaining** — completing a mission can unlock and auto-start the
  next one, with a briefing message and an on-screen "NEW MISSION" toast.
- **Endgame & debrief** — a mission flagged as the endgame finishes the
  session: a full-screen MISSION ACCOMPLISHED debrief with a service rank
  earned from score, final stats, and a victory fanfare. The GM is notified.

### Comms (story messages)
- **Inbox** — story messages arrive from Command, droids, terminals, and the
  GM, with unread badges on the home screen. Read state persists.
- **Triggered delivery** — messages fire on boot, on mission start, on
  scanning a specific tag, or on any game event, so the story reacts to what
  the player actually does.
- **Faction-scoped messages** — Imperial briefings never land in Rebel
  inboxes and vice versa.
- **Encrypted transmissions** — some messages arrive scrambled into glyphs.
  A DECRYPT button launches the code-breaking minigame; winning reveals the
  real text (and can advance missions). Losing locks the cipher for 30 s.

### Scanning
- **Datacard reader** — scans NFC tags: mission datacards start/advance
  missions with a celebratory result card; item cards go to the inventory;
  another player's operative card starts a duel.
- **Cargo intel** — a separate scanner flow for CARGO_* crates (supply-run
  side content), with wrong-reader redirection so players can't confuse the
  two.

### Inventory & items
- **Inventory** — up to 8 items, collected by scanning ITEM cards or buying
  at a terminal shop. Opened by tapping the home footer; items can be
  DROPped (trade by handing the physical card over) or USEd.
- **KEYCARDs** — satisfy a panel's authorization gate (panels have no card
  reader, so the datapad vouches for what it has scanned).
- **SIGNAL JAMMER** — one-shot: the next enemy-faction prop treats you as
  neutral (normal difficulty instead of hardened). Consumed on use.
- **INFORMANT TIP** — one-shot: signals the Game Master to release the next
  bounty clue.

### Bounties
- **Wanted board** — the GM posts bounty targets with rewards; clues are
  revealed one at a time. Claiming pays out and is announced to all datapads.

### Radar
- **Contact list** — shows nearby operatives and props from mesh signal
  strength, bucketed NEAR / CLOSE / FAR and colored by faction (allies green,
  enemies red, undeclared grey). Adults only by default — hidden in KID mode.

### Duels (player vs. player)
- **Challenge by card** — scan another player's operative card to challenge
  them; they get an ACCEPT / DECLINE prompt on their own datapad.
- **Reaction race** — both datapads light the same sequence of arcade
  buttons; fastest total time wins (wrong presses cost +300 ms). The winner
  takes the credit stake from the loser. Rematch cooldown prevents farming;
  KID-mode players are excluded.

### Countdown HUD
- **Session timer** — when the GM starts a countdown (e.g. EXTRACTION
  WINDOW), every datapad shows it ticking in the home header.

### KID mode
- **Per-player difficulty** — the GM can flip any player between ADULT
  (default) and KID from the console. KID mode makes every minigame gentler:
  fewer rounds/targets, slower sequences, longer time limits, easier ciphers.
  Applied on top of faction difficulty. Radar and duels are disabled for kids.

---

## 2. Hacking minigames

All are launched by interacting with props; difficulty stacks **faction**
(your own faction's gear is easier, the enemy's harder, neutral unchanged)
and **KID mode** modifiers. Wins award score and XP and are reported to the
GM. Walking away mid-game drops the link and counts as a failure.

- **Timing-bar Slice** *(touchscreen)* — a cursor sweeps a bar; tap when it
  crosses the target zones. Zones shrink each round.
- **Simon "Pattern Lock"** *(physical buttons)* — the three lit arcade
  buttons play back a growing color/tone sequence; repeat it to breach the
  lock. On-screen pads mirror the physical layout (White / Blue / Red).
- **Core Purge** *(touchscreen)* — memory-defrag whack-a-mole: corrupted ERR
  blocks flash up on a 4x4 grid, tap them before they vanish, avoid protected
  SYS decoys. Three faults and you're locked out.
- **Decrypt** *(touchscreen)* — Mastermind-style code breaking: guess the
  four-glyph cipher; each attempt reports how many glyphs are LOCKED (right
  place) or LOOSE (right glyph, wrong place). Used by terminals and encrypted
  comms.

---

## 3. Props

### Common to all props
- **SD-card configured** — identity, faction, behavior, dialogue, lights and
  shop stock all come off the card; re-theme a prop for a new event by
  swapping cards, no reflashing.
- **RGB light strips** — up to 64 addressable LEDs with a per-LED idle
  pattern (solid / blink / random flicker, any color) defined in a simple
  text file. Interactions interrupt the pattern with meaningful effects:
  amber activity blinks, green success flashes, red failures, cyan transmit
  strobes — each paired with a buzzer phrase.
- **Proximity gate** — props reject interactions from players who aren't
  physically close (WiFi signal strength, tuned per prop), so hacks can't be
  run from across the room. Wandering off mid-minigame loses the link — and
  the attempt.
- **Countdown visual** — props can red-shift their idle lighting as a GM
  countdown runs out, so the whole room feels the clock.

### Panel (wall prop)
- **Slice-locked data** — the panel's data logs are locked until the player
  beats its minigame (Simon or timing-bar). Breaching it grants access, fires
  a mission event, and can reveal lore files (e.g. the cantina's backroom
  ledger with story clues).
- **Extraction signal** — a breached panel can broadcast the extraction
  signal — the endgame's key action.
- **Keycard authorization** — a panel can require a specific keycard in the
  player's inventory before it will even talk.
- **Alarm state** — repeated failures by enemy-faction players trip a
  klaxon: red strobing lights, a siren, an event to the GM, and a lockout
  for that faction. Friendly players silence it by holding RESET for five
  seconds; enemies must win a one-round pattern-lock bypass. The GM can
  clear it remotely; otherwise it times out.

### Terminal (NFC kiosk)
- **Card scanning station** — reads datacards physically presented at the
  prop; scans broadcast events that advance missions and trigger comms.
- **Tap-to-use sessions** — the player taps their operative card to claim
  the terminal for 60 seconds; only they can use it, and someone else
  tapping gets denied. Session opens/closes are events the GM sees.
- **Decrypt gate** — scanned datacards can be held "encrypted in the
  buffer" until the player connects and wins the Decrypt minigame — only
  then does the scan count.
- **Supply exchange (shop)** — sells items (jammers, informant tips...) for
  the player's credits, stocked from a JSON file on the card.

### Droid (astromech prop)
- **Character interactions** — greets players with astromech chatter,
  accepts the intel handoff that completes the main mission, and announces
  everything to the GM.
- **Memory core (Core Purge)** — his damaged core must be purged before he
  can accept uploads — easier for his own faction, harder for the enemy.
- **Mood** — the droid remembers how he's been treated (−5 grumpy to +5
  delighted): friendly greetings and successful handoffs cheer him up;
  being wiped by the enemy or failed purges sour him. His eye light changes
  color with mood, his dialogue changes tone, his minigame gets easier or
  harder — and below −4 he refuses to cooperate until someone pays a
  50-credit apology. Mood persists across reboots and decays back to
  neutral over time.

---

## 4. Game Master console

- **HOME dashboard** — top-3 leaderboard, prop status pills, scenario stats
  and a live activity feed of everything happening in the room.
- **EVENTS** — one-tap story events (patrols, sandstorms, lockdowns...),
  each broadcastable to everyone or scoped to one faction; plus
  **countdown timers** (start/cancel a dead-man's-switch that automatically
  fires a chosen event at zero).
- **COMMS** — pre-written message templates (tap to send, long-press to
  edit) and a compose screen; target everyone, a whole faction, or one
  player. Faction-tagged templates route automatically.
- **BOUNTIES** — post bounty targets, drip out clues, award the claim to a
  player, or cancel.
- **PLAYERS** — live roster sorted by score with faction and difficulty
  badges. Tap a player for their **detail view**: full stats (score, XP,
  missions done/active, scans, minigames won, online status) plus a
  timestamped **interaction log** of everything they've done — the gameplay
  analytics view. The ADULT/KID toggle lives here too. An UNASSIGNED list
  catches un-named datapads so the operator can assign callsigns.
- **FACTIONS** — per-faction totals (score, members online, missions,
  slices, scans) with progress bars toward the scenario's score goal — the
  team-vs-team scoreboard.
- **PANELS** — every prop's live status: online/offline, alarm (with a
  CLEAR ALARM button), open terminal session and who holds it, droid mood.
- **Remote controls** — rename datapads, force scores, clear alarms, put
  props online/offline, reset devices, and resync the whole room after a
  console reboot.
- **Session persistence** — bounty state and the roster snapshot survive a
  GM reboot; ghost entries from old sessions are pruned automatically.

---

## 5. Infrastructure (what makes it all work)

- **ESP-NOW mesh** — every device shares a radio mesh: heartbeats, status,
  scores, scans, events, comms, bounties, duels, timers. No router or
  internet needed.
- **Game events** — a single event vocabulary (`panel_sliced:...`,
  `nfc:TERM:CARD`, `droid_handoff:...`, `alarm:...`, `item_bought:...`,
  `duel_won:...` and more) ties everything together: any event can advance a
  mission step, trigger a comm, or just inform the GM.
- **Prop WiFi APs** — each prop hosts its own network; datapads hop on to
  talk dialogue, minigames, shops, and lore over a tiny JSON API.
- **Test provisioning** — one compile-time switch embeds a full demo
  scenario in the firmware and writes it to every device's storage at boot;
  comment it out and rebuild to go live on real provisioned cards.
- **Player-state portability** — a player's SD card *is* their save; move
  it to a spare datapad and they carry on where they left off.

---

*Written for: game masters, venue operators, and anyone deciding what the
Sci-Fi Game Engine can do.*
