/*
 * SWTS Mesh — ESPNOW communication protocol
 * Shared between Datapad, Panel, and GM_DATAPAD
 *
 * Architecture:
 *   GM_DATAPAD (ESP32-P4) — broadcasts events, pushes comms, monitors players
 *   DATAPAD (ESP32-S3)    — receives events/comms, reports score back
 *   PANEL (ESP32-S3)      — receives override commands, reports slice activity
 *
 * All devices use ESPNOW on the same WiFi channel.
 * Messages have a type byte + sender ID + payload.
 * GM uses broadcast (FF:FF:FF:FF:FF:FF) for "all devices".
 * Devices send back to GM via its registered MAC.
 */
#pragma once
#include <Arduino.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WiFi.h>

namespace swts {

// ═══════════════════════════════════════════════
//  PROTOCOL
// ═══════════════════════════════════════════════
#define MESH_CHANNEL  1
#define MESH_MAGIC    0x5754  // 'SW'
#define MESH_VERSION  1
#define MESH_MAX_PAYLOAD 220  // ESPNOW limit is 250 bytes total

// Message types
enum MeshMsgType : uint8_t {
    // Device → GM (status/reports)
    MSG_PING        = 0x01,  // Heartbeat — device alive
    MSG_STATUS      = 0x02,  // Full status report
    MSG_SCORE       = 0x03,  // Score change report
    MSG_NFC_SCAN    = 0x04,  // Player scanned a datacard
    MSG_SLICE_RESULT= 0x05,  // Slice minigame result

    // GM → Device (commands)
    MSG_EVENT       = 0x10,  // Trigger game event
    MSG_COMM        = 0x11,  // Push comm message to inbox
    MSG_BOUNTY      = 0x12,  // Push bounty target
    MSG_OVERRIDE    = 0x13,  // Override prop state
    MSG_RESET       = 0x14,  // Reset player/device
    MSG_ALERT       = 0x15,  // Push urgent alert
    MSG_SCORE_SET   = 0x16,  // Force-set score
    MSG_BOUNTY_CLUE = 0x17,  // Push a clue line for an existing bounty
    MSG_BOUNTY_CLAIM= 0x18,  // GM marks bounty awarded to a specific player
    MSG_BOUNTY_CANCEL=0x19,  // GM closes bounty with no winner
    MSG_SYNC_REQUEST= 0x1A,  // GM asks all devices to send MSG_STATUS now (post-boot resync)
    MSG_ASSIGN      = 0x1B,  // GM assigns a new callsign to a target datapad
    MSG_FACTIONS    = 0x1C,  // GM broadcasts the scenario's faction roster
    MSG_PLAYER_MODE = 0x1D,  // GM sets a player's difficulty mode (ADULT/KID)
    MSG_TIMER       = 0x1E,  // GM countdown broadcast (rebroadcast every 5s)
    MSG_ALARM_CLEAR = 0x1F,  // GM clears a prop's alarm state
    MSG_DUEL_REQUEST= 0x20,  // datapad challenges another datapad
    MSG_DUEL_ACCEPT = 0x21,  // challenge accepted/declined
    MSG_DUEL_RESULT = 0x22,  // each duelist's finish time (also read by GM)
};

// Device roles
enum MeshRole : uint8_t {
    ROLE_GM      = 0x01,
    ROLE_DATAPAD = 0x02,
    ROLE_PANEL   = 0x03,
};

// Header — first 32 bytes of every message
struct __attribute__((packed)) MeshHeader {
    uint16_t magic;       // 0x5754 'SW'
    uint8_t  version;
    uint8_t  type;
    uint32_t seq;
    char     from_id[16]; // Device ID, e.g. "KESTIS-7" or "PANEL_01"
    uint8_t  role;
    uint8_t  reserved[7];
};

// ── Specific payloads (follow header) ──

struct __attribute__((packed)) MeshPing {
    uint32_t uptime_sec;
    uint8_t  rssi;        // received signal of GM (filled by GM)
};

struct __attribute__((packed)) MeshStatus {
    uint32_t uptime_sec;
    int32_t  score;
    uint16_t mission_active;
    uint16_t mission_complete;
    uint16_t scans;
    uint16_t slices_won;
    char     faction[14];   // player's chosen faction ("" = none yet)
    uint8_t  kid;           // 1 = kid difficulty mode
    uint16_t xp;            // experience points
    // Prop extensions (zero for datapads)
    int8_t   mood;          // droid mood -5..+5
    uint8_t  alarm;         // 1 = prop is in ALARM state
    char     session[16];   // terminal: callsign of the open session
};

struct __attribute__((packed)) MeshTimer {
    char     id[16];
    char     label[24];
    uint32_t total_s;       // full duration
    uint32_t remaining_s;   // at send time (receiver re-anchors to millis())
    char     faction[14];   // "" = everyone sees it
    uint8_t  active;        // 0 = timer cancelled/expired
};

struct __attribute__((packed)) MeshAlarmClear {
    char     target_prop[16];
};

struct __attribute__((packed)) MeshDuelRequest {
    char     duel_id[16];
    char     challenger[16];
    char     target[16];
    int32_t  stake;
    uint32_t seed;          // both sides generate the same button sequence
    uint8_t  targets;       // presses to win
};

struct __attribute__((packed)) MeshDuelAccept {
    char     duel_id[16];
    char     callsign[16];
    uint8_t  accept;        // 0 = declined
};

struct __attribute__((packed)) MeshDuelResult {
    char     duel_id[16];
    char     callsign[16];
    uint32_t time_ms;       // total time incl. penalties (0xFFFFFFFF = aborted)
    uint8_t  faults;
};

struct __attribute__((packed)) MeshPlayerMode {
    char     target[16];    // callsign of the datapad to change
    uint8_t  kid;           // 1 = kid mode, 0 = adult (default)
};

#define MESH_MAX_FACTIONS 6
struct __attribute__((packed)) MeshFactions {
    uint8_t count;
    char    names[MESH_MAX_FACTIONS][14];
};

struct __attribute__((packed)) MeshScore {
    int32_t  new_score;
    int16_t  delta;         // +/- change
    char     reason[40];    // "MISSION:ghost_signal", "SLICE:PANEL_01"
};

struct __attribute__((packed)) MeshNfcScan {
    char     tag_id[32];
    char     tag_name[40];
    char     category[16];
};

struct __attribute__((packed)) MeshSliceResult {
    char     panel_id[16];
    bool     won;
    uint8_t  rounds_completed;
    int16_t  score;
    uint16_t time_taken_sec;
};

struct __attribute__((packed)) MeshEvent {
    char     event_id[32];
    char     event_name[40];
    uint8_t  severity;      // 0=info 1=warn 2=alert 3=critical
    char     faction[14];   // "" = all players, else only this faction reacts
    char     payload[120];  // free-form context
};

struct __attribute__((packed)) MeshComm {
    char     comm_id[20];
    char     target[16];    // "" = broadcast; "<callsign>" = one player;
                            // "@<FACTION>" = every player of that faction
                            // (prefix convention — the struct is at the
                            //  ESPNOW size limit, no room for a new field)
    char     from[20];
    char     subject[36];
    char     body[120];
};
// Total: 20+16+20+36+120 = 212 bytes (+32 header = 244 total, fits in 250 ESPNOW limit)

struct __attribute__((packed)) MeshBounty {
    char     bounty_id[24];
    char     target[16];    // empty = broadcast (any player can take)
    char     target_name[24];
    char     description[60];
    int32_t  reward;
};

struct __attribute__((packed)) MeshBountyClue {
    char     bounty_id[24];
    uint8_t  clue_num;       // 1-based index of this clue
    char     clue_text[160];
};

struct __attribute__((packed)) MeshBountyClaim {
    char     bounty_id[24];
    char     claimer_id[16]; // device id of player who collected
    char     claimer_name[24];
    int32_t  reward;
};

struct __attribute__((packed)) MeshBountyCancel {
    char     bounty_id[24];
    char     reason[40];
};

struct __attribute__((packed)) MeshAssign {
    char     target[16];        // current callsign of target datapad (must match exactly)
    char     new_callsign[16];  // what to set its callsign to
};

struct __attribute__((packed)) MeshOverride {
    char     target_prop[16];
    char     command[16];   // "online", "offline", "difficulty", "reset"
    char     value[32];
};

struct __attribute__((packed)) MeshAlert {
    char     title[40];
    char     body[100];
    uint8_t  severity;
    uint16_t duration_ms;
};

// ═══════════════════════════════════════════════
//  STATE
// ═══════════════════════════════════════════════
inline char  myId[16] = "UNKNOWN";
inline uint8_t myRole = 0;
inline uint32_t txSeq = 0;
inline uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Callback: user code provides a handler for incoming messages
typedef void (*MeshHandler)(const MeshHeader *hdr, const uint8_t *payload, int payloadLen);
inline MeshHandler userHandler = nullptr;

// ═══════════════════════════════════════════════
//  INTERNAL RECEIVE CALLBACK
// ═══════════════════════════════════════════════
// Per-sender seq dedupe — catches MAC-layer frame duplicates.
// The table doubles as the peer table for the radar: it remembers each
// sender's role, last-heard time and (when RSSI capture is enabled) a
// rolling received-signal estimate.
#define MESH_PEER_TABLE 16
struct SenderSeq {
    char     id[16];
    uint32_t lastSeq;
    uint8_t  role;
    int8_t   rssi;          // smoothed; 0 = unknown
    unsigned long lastHeard;
};
inline SenderSeq seqTable[MESH_PEER_TABLE] = {};

// Radar support: a promiscuous callback captures the RSSI of the frame the
// ESPNOW receive callback is about to deliver. The two fire back-to-back for
// the same frame, so pairing "last promiscuous RSSI" with the next dispatch
// is accurate enough for proximity buckets.
inline volatile int8_t lastFrameRssi = 0;
inline bool rssiCaptureOn = false;

inline void promiscRssiCb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;   // ESPNOW rides on action (mgmt) frames
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    lastFrameRssi = p->rx_ctrl.rssi;
}

// Call after meshInit() on devices that want per-peer RSSI (the datapad radar)
inline void meshEnableRssi() {
    esp_wifi_set_promiscuous_rx_cb(promiscRssiCb);
    wifi_promiscuous_filter_t filt = {};
    filt.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filt);
    esp_wifi_set_promiscuous(true);
    rssiCaptureOn = true;
}

inline bool seqIsDuplicate(const MeshHeader *hdr) {
    for (int i = 0; i < MESH_PEER_TABLE; i++) {
        SenderSeq &s = seqTable[i];
        bool match = (s.id[0] != 0 && strcmp(s.id, hdr->from_id) == 0);
        if (s.id[0] == 0 || match) {
            if (match && hdr->seq == s.lastSeq) return true;   // exact radio dup
            if (!match) strlcpy(s.id, hdr->from_id, sizeof(s.id));
            s.lastSeq = hdr->seq;
            s.role = hdr->role;
            s.lastHeard = millis();
            if (rssiCaptureOn && lastFrameRssi != 0) {
                s.rssi = s.rssi ? (int8_t)((s.rssi * 3 + lastFrameRssi) / 4)
                                : lastFrameRssi;
            }
            return false;
        }
    }
    return false;   // table full — accept (no dedupe possible)
}

// Internal dispatch — works with either callback signature
inline void dispatchRecv(const uint8_t *data, int len) {
    if (len < (int)sizeof(MeshHeader)) return;
    const MeshHeader *hdr = (const MeshHeader *)data;
    if (hdr->magic != MESH_MAGIC) return;
    if (hdr->version != MESH_VERSION) return;
    if (seqIsDuplicate(hdr)) {
        Serial.printf("[MESH] Dup seq=%u from %s, dropping\n", (unsigned)hdr->seq, hdr->from_id);
        return;
    }
    const uint8_t *payload = data + sizeof(MeshHeader);
    int payloadLen = len - sizeof(MeshHeader);
    if (userHandler) userHandler(hdr, payload, payloadLen);
}

// Pick callback signature based on IDF version.
// IDF 5.1+ (Arduino-ESP32 3.1+, used by ESP32-P4) uses esp_now_recv_info_t
// IDF 5.0 (Arduino-ESP32 3.0, current S3 build) uses raw mac pointer
#if defined(ESP_IDF_VERSION_MAJOR) && (ESP_IDF_VERSION_MAJOR >= 5) && (ESP_IDF_VERSION_MINOR >= 1)
inline void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    dispatchRecv(data, len);
}
#else
inline void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
    dispatchRecv(data, len);
}
#endif

// ═══════════════════════════════════════════════
//  INIT
// ═══════════════════════════════════════════════
inline bool meshInit(const char *deviceId, uint8_t role, MeshHandler handler) {
    strlcpy(myId, deviceId, sizeof(myId));
    myRole = role;
    userHandler = handler;

    // WiFi must be in STA mode for ESPNOW
    if (WiFi.getMode() == WIFI_OFF) WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    // Lock to mesh channel
    esp_wifi_set_channel(MESH_CHANNEL, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println("[MESH] init failed");
        return false;
    }
    esp_now_register_recv_cb(onRecv);

    // Add broadcast peer
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, broadcastMac, 6);
    peer.channel = MESH_CHANNEL;
    peer.encrypt = false;
    if (!esp_now_is_peer_exist(broadcastMac)) esp_now_add_peer(&peer);

    Serial.printf("[MESH] Ready: id=%s role=%d ch=%d\n", myId, role, MESH_CHANNEL);
    return true;
}

// ═══════════════════════════════════════════════
//  SEND
// ═══════════════════════════════════════════════
inline bool meshSend(uint8_t type, const void *payload, int payloadLen, const uint8_t *toMac = broadcastMac) {
    uint8_t buf[250];
    MeshHeader *hdr = (MeshHeader *)buf;
    hdr->magic = MESH_MAGIC;
    hdr->version = MESH_VERSION;
    hdr->type = type;
    hdr->seq = ++txSeq;
    strlcpy(hdr->from_id, myId, sizeof(hdr->from_id));
    hdr->role = myRole;
    memset(hdr->reserved, 0, sizeof(hdr->reserved));

    int total = sizeof(MeshHeader) + payloadLen;
    if (total > 250) {
        Serial.printf("[MESH] send failed: payload %d > 250 (type 0x%02X)\n", total, type);
        return false;
    }
    if (payload && payloadLen > 0) memcpy(buf + sizeof(MeshHeader), payload, payloadLen);

    esp_err_t r = esp_now_send(toMac, buf, total);
    return r == ESP_OK;
}

// Convenience wrappers
inline bool sendPing(uint32_t uptime) {
    MeshPing p = {uptime, 0};
    return meshSend(MSG_PING, &p, sizeof(p));
}

inline bool sendStatus(uint32_t uptime, int score, int activeMsn, int completeMsn, int scans, int slicesWon,
                       const char *faction = "", bool kid = false, int xp = 0) {
    MeshStatus s = {};
    s.uptime_sec = uptime;
    s.score = score;
    s.mission_active = (uint16_t)activeMsn;
    s.mission_complete = (uint16_t)completeMsn;
    s.scans = (uint16_t)scans;
    s.slices_won = (uint16_t)slicesWon;
    if (faction) strlcpy(s.faction, faction, sizeof(s.faction));
    s.kid = kid ? 1 : 0;
    s.xp = (uint16_t)(xp < 0 ? 0 : xp);
    return meshSend(MSG_STATUS, &s, sizeof(s));
}

inline bool sendScore(int newScore, int delta, const char *reason) {
    MeshScore s = {};
    s.new_score = newScore;
    s.delta = delta;
    strlcpy(s.reason, reason, sizeof(s.reason));
    return meshSend(MSG_SCORE, &s, sizeof(s));
}

inline bool sendNfcScan(const char *tagId, const char *tagName, const char *category) {
    MeshNfcScan n = {};
    strlcpy(n.tag_id, tagId, sizeof(n.tag_id));
    strlcpy(n.tag_name, tagName, sizeof(n.tag_name));
    strlcpy(n.category, category, sizeof(n.category));
    return meshSend(MSG_NFC_SCAN, &n, sizeof(n));
}

inline bool sendSliceResult(const char *panelId, bool won, int rounds, int score, int timeSec) {
    MeshSliceResult r = {};
    strlcpy(r.panel_id, panelId, sizeof(r.panel_id));
    r.won = won;
    r.rounds_completed = rounds;
    r.score = score;
    r.time_taken_sec = timeSec;
    return meshSend(MSG_SLICE_RESULT, &r, sizeof(r));
}

// GM-side senders
inline bool gmTriggerEvent(const char *eventId, const char *eventName, uint8_t severity, const char *payload,
                           const char *faction = "") {
    MeshEvent e = {};
    strlcpy(e.event_id, eventId, sizeof(e.event_id));
    strlcpy(e.event_name, eventName, sizeof(e.event_name));
    e.severity = severity;
    if (faction) strlcpy(e.faction, faction, sizeof(e.faction));
    if (payload) strlcpy(e.payload, payload, sizeof(e.payload));
    return meshSend(MSG_EVENT, &e, sizeof(e));
}

inline bool gmPushComm(const char *commId, const char *target, const char *from, const char *subject, const char *body) {
    MeshComm c = {};
    strlcpy(c.comm_id, commId, sizeof(c.comm_id));
    if (target) strlcpy(c.target, target, sizeof(c.target));
    strlcpy(c.from, from, sizeof(c.from));
    strlcpy(c.subject, subject, sizeof(c.subject));
    strlcpy(c.body, body, sizeof(c.body));
    return meshSend(MSG_COMM, &c, sizeof(c));
}

inline bool gmPushBounty(const char *bountyId, const char *target, const char *targetName, const char *desc, int reward) {
    MeshBounty b = {};
    strlcpy(b.bounty_id, bountyId, sizeof(b.bounty_id));
    if (target) strlcpy(b.target, target, sizeof(b.target));
    strlcpy(b.target_name, targetName, sizeof(b.target_name));
    strlcpy(b.description, desc, sizeof(b.description));
    b.reward = reward;
    return meshSend(MSG_BOUNTY, &b, sizeof(b));
}

inline bool gmPushBountyClue(const char *bountyId, uint8_t clueNum, const char *clueText) {
    MeshBountyClue c = {};
    strlcpy(c.bounty_id, bountyId, sizeof(c.bounty_id));
    c.clue_num = clueNum;
    strlcpy(c.clue_text, clueText, sizeof(c.clue_text));
    return meshSend(MSG_BOUNTY_CLUE, &c, sizeof(c));
}

inline bool gmClaimBounty(const char *bountyId, const char *claimerId, const char *claimerName, int reward) {
    MeshBountyClaim c = {};
    strlcpy(c.bounty_id, bountyId, sizeof(c.bounty_id));
    strlcpy(c.claimer_id, claimerId, sizeof(c.claimer_id));
    strlcpy(c.claimer_name, claimerName, sizeof(c.claimer_name));
    c.reward = reward;
    return meshSend(MSG_BOUNTY_CLAIM, &c, sizeof(c));
}

// Assign a fresh callsign to one specific datapad (targeted by its current callsign).
inline bool gmAssignCallsign(const char *target, const char *newCallsign) {
    MeshAssign a = {};
    strlcpy(a.target, target, sizeof(a.target));
    strlcpy(a.new_callsign, newCallsign, sizeof(a.new_callsign));
    return meshSend(MSG_ASSIGN, &a, sizeof(a));
}

// Prop status report (panel/terminal/droid): mood, alarm, open session.
// Reuses MeshStatus with the player fields zeroed.
inline bool sendPropStatus(uint32_t uptime, int mood, bool alarm, const char *session) {
    MeshStatus s = {};
    s.uptime_sec = uptime;
    s.mood = (int8_t)mood;
    s.alarm = alarm ? 1 : 0;
    if (session) strlcpy(s.session, session, sizeof(s.session));
    return meshSend(MSG_STATUS, &s, sizeof(s));
}

// GM countdown broadcast (send every ~5s while running; active=0 to cancel)
inline bool gmSendTimer(const char *id, const char *label, uint32_t totalS,
                        uint32_t remainingS, const char *faction, bool active) {
    MeshTimer t = {};
    strlcpy(t.id, id, sizeof(t.id));
    strlcpy(t.label, label, sizeof(t.label));
    t.total_s = totalS;
    t.remaining_s = remainingS;
    if (faction) strlcpy(t.faction, faction, sizeof(t.faction));
    t.active = active ? 1 : 0;
    return meshSend(MSG_TIMER, &t, sizeof(t));
}

inline bool gmAlarmClear(const char *propId) {
    MeshAlarmClear a = {};
    strlcpy(a.target_prop, propId, sizeof(a.target_prop));
    return meshSend(MSG_ALARM_CLEAR, &a, sizeof(a));
}

inline bool sendDuelRequest(const char *duelId, const char *challenger, const char *target,
                            int stake, uint32_t seed, uint8_t targets) {
    MeshDuelRequest d = {};
    strlcpy(d.duel_id, duelId, sizeof(d.duel_id));
    strlcpy(d.challenger, challenger, sizeof(d.challenger));
    strlcpy(d.target, target, sizeof(d.target));
    d.stake = stake;
    d.seed = seed;
    d.targets = targets;
    return meshSend(MSG_DUEL_REQUEST, &d, sizeof(d));
}

inline bool sendDuelAccept(const char *duelId, const char *callsign, bool accept) {
    MeshDuelAccept d = {};
    strlcpy(d.duel_id, duelId, sizeof(d.duel_id));
    strlcpy(d.callsign, callsign, sizeof(d.callsign));
    d.accept = accept ? 1 : 0;
    return meshSend(MSG_DUEL_ACCEPT, &d, sizeof(d));
}

inline bool sendDuelResult(const char *duelId, const char *callsign, uint32_t timeMs, uint8_t faults) {
    MeshDuelResult d = {};
    strlcpy(d.duel_id, duelId, sizeof(d.duel_id));
    strlcpy(d.callsign, callsign, sizeof(d.callsign));
    d.time_ms = timeMs;
    d.faults = faults;
    return meshSend(MSG_DUEL_RESULT, &d, sizeof(d));
}

// Set one datapad's difficulty mode (targeted by its current callsign)
inline bool gmSetPlayerMode(const char *target, bool kid) {
    MeshPlayerMode m = {};
    strlcpy(m.target, target, sizeof(m.target));
    m.kid = kid ? 1 : 0;
    return meshSend(MSG_PLAYER_MODE, &m, sizeof(m));
}

// Broadcast the scenario's faction roster (datapads use it for the pick screen)
inline bool gmSendFactions(const char names[][14], uint8_t count) {
    MeshFactions f = {};
    if (count > MESH_MAX_FACTIONS) count = MESH_MAX_FACTIONS;
    f.count = count;
    for (uint8_t i = 0; i < count; i++) strlcpy(f.names[i], names[i], sizeof(f.names[i]));
    return meshSend(MSG_FACTIONS, &f, sizeof(f));
}

// Empty-payload request to make every device re-send its current MSG_STATUS.
// Used by GM at boot to resync after a swap or power cycle.
inline bool gmSyncRequest() {
    return meshSend(MSG_SYNC_REQUEST, nullptr, 0);
}

inline bool gmCancelBounty(const char *bountyId, const char *reason) {
    MeshBountyCancel c = {};
    strlcpy(c.bounty_id, bountyId, sizeof(c.bounty_id));
    if (reason) strlcpy(c.reason, reason, sizeof(c.reason));
    return meshSend(MSG_BOUNTY_CANCEL, &c, sizeof(c));
}

inline bool gmOverride(const char *targetProp, const char *command, const char *value) {
    MeshOverride o = {};
    strlcpy(o.target_prop, targetProp, sizeof(o.target_prop));
    strlcpy(o.command, command, sizeof(o.command));
    if (value) strlcpy(o.value, value, sizeof(o.value));
    return meshSend(MSG_OVERRIDE, &o, sizeof(o));
}

inline bool gmAlert(const char *title, const char *body, uint8_t severity, uint16_t durationMs) {
    MeshAlert a = {};
    strlcpy(a.title, title, sizeof(a.title));
    strlcpy(a.body, body, sizeof(a.body));
    a.severity = severity;
    a.duration_ms = durationMs;
    return meshSend(MSG_ALERT, &a, sizeof(a));
}

} // namespace swts
