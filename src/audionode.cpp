/*
 * SFGE Audio Node — a small ESP32-S3 that drives a DFPlayer Mini
 * (via the DFRobotDFPlayerMini library: verified handshake + error reports)
 *
 * Props with no (working) speaker delegate their voice to this node over
 * the mesh — BY NAME. The DFPlayer itself only understands track numbers
 * and hides its card from us, so the name->number table is TAUGHT to this
 * node over ESP-NOW: the Guild Master broadcasts its speech_tracks map
 * (from its own card's config.json) at boot and every 30 s. After that:
 *
 *   MSG_OVERRIDE {target: AUDIO_01, command: "play", value: "boot"}
 *     -> node resolves "boot" -> track 1 -> DFPlayer plays /mp3/0001.mp3
 *
 * Mesh commands (target = this node's id or "ALL"):
 *   map    <name>=<n>   teach one mapping (sent by the guild automatically)
 *   play   <name|n>     play by name, or directly by number
 *   volume <0-30>       set DFPlayer volume
 *   stop                stop playback
 *
 * DFPlayer wiring (module pin -> this board):
 *   VCC -> 5V                    SPK_1/SPK_2 -> speaker (3W max)
 *   RX  -> GPIO 5 (via 1k)       GND -> GND (common)
 *   TX  -> GPIO 6
 *   BUSY-> GPIO 7 (LOW while a track is playing)
 *
 * DFPlayer SD layout: folder  mp3  in the card root, tracks 0001.mp3,
 * 0002.mp3, ... (tools/make_dfplayer_card.ps1 builds it from named files).
 */
#include <Arduino.h>
#include <WiFi.h>
#include <DFRobotDFPlayerMini.h>
#include "sfge_mesh.h"
#define S Serial

#define DF_TX_PIN   5   // this board's TX -> DFPlayer RX (through 1k)
#define DF_RX_PIN   6   // this board's RX <- DFPlayer TX
#define DF_BUSY_PIN 7   // DFPlayer BUSY: LOW = playing
#define NODE_ID   "AUDIO_01"
#define DF_DEFAULT_VOLUME 24   // 0-30

HardwareSerial dfSerial(1);
DFRobotDFPlayerMini player;
bool dfOk = false;
unsigned long dfRetryAt = 0;

// ── Name -> track table, taught over the mesh ──
#define MAX_TRACKS 24
struct TrackMap { char name[24]; uint16_t track; };
TrackMap trackMap[MAX_TRACKS];
int trackMapCount = 0;

int lookupTrack(const char *name) {
    for (int i = 0; i < trackMapCount; i++)
        if (strcasecmp(trackMap[i].name, name) == 0) return trackMap[i].track;
    return -1;
}

// "name=number" — update an existing entry or add a new one
void mapTeach(const char *kv) {
    const char *eq = strchr(kv, '=');
    if (!eq || eq == kv) return;
    int n = atoi(eq + 1);
    if (n <= 0) return;
    char name[24];
    int len = eq - kv;
    if (len >= (int)sizeof(name)) len = sizeof(name) - 1;
    memcpy(name, kv, len);
    name[len] = 0;
    for (int i = 0; i < trackMapCount; i++) {
        if (strcasecmp(trackMap[i].name, name) == 0) {
            trackMap[i].track = (uint16_t)n;
            return;
        }
    }
    if (trackMapCount < MAX_TRACKS) {
        strlcpy(trackMap[trackMapCount].name, name, sizeof(trackMap[0].name));
        trackMap[trackMapCount].track = (uint16_t)n;
        trackMapCount++;
        S.printf("[MAP] learned %s = %d (%d entries)\n", name, n, trackMapCount);
    }
}

// Commands arrive on the mesh RX callback — defer playback to loop()
char pendingPlay[32] = "";
volatile bool pendingPlayFlag = false;
volatile int  pendingVolume   = -1;
volatile bool pendingStop     = false;
volatile bool restartRequested = false;

void onMeshMsg(const sfge::MeshHeader *hdr, const uint8_t *payload, int len) {
    if (hdr->type == sfge::MSG_SYNC_REQUEST) { sfge::sendPing(millis() / 1000); return; }
    if (hdr->type == sfge::MSG_RESET || hdr->type == sfge::MSG_NEW_GAME) {
        restartRequested = true;
        return;
    }
    if (hdr->type == sfge::MSG_OVERRIDE && len >= (int)sizeof(sfge::MeshOverride)) {
        const sfge::MeshOverride *o = (const sfge::MeshOverride *)payload;
        if (strcmp(o->target_prop, NODE_ID) != 0 && strcmp(o->target_prop, "ALL") != 0) return;
        if      (strcmp(o->command, "map") == 0)    mapTeach(o->value);
        else if (strcmp(o->command, "play") == 0) {
            strlcpy(pendingPlay, o->value, sizeof(pendingPlay));
            pendingPlayFlag = true;
        }
        else if (strcmp(o->command, "volume") == 0) pendingVolume = atoi(o->value);
        else if (strcmp(o->command, "stop") == 0)   pendingStop   = true;
    }
}

// Decode the library's status/error reports onto serial (from the user's
// MP3Player wrapper — invaluable for "why is it silent" sessions)
void dfPrintStatus() {
    if (!dfOk || !player.available()) return;
    uint8_t type = player.readType();
    int value = player.read();
    switch (type) {
        case DFPlayerCardInserted:  S.println("[DF] card inserted");  break;
        case DFPlayerCardRemoved:   S.println("[DF] card REMOVED");   break;
        case DFPlayerCardOnline:    S.println("[DF] card online");    break;
        case DFPlayerPlayFinished:  S.printf("[DF] track %d finished\n", value); break;
        case DFPlayerError:
            S.print("[DF] ERROR: ");
            switch (value) {
                case Busy:             S.println("card not found");        break;
                case Sleeping:         S.println("sleeping");              break;
                case SerialWrongStack: S.println("wrong stack");           break;
                case CheckSumNotMatch: S.println("checksum mismatch");     break;
                case FileIndexOut:     S.println("file index out of bounds"); break;
                case FileMismatch:     S.println("cannot find file");      break;
                case Advertise:        S.println("in advertise");          break;
                default:               S.printf("code %d\n", value);       break;
            }
            break;
        default: break;
    }
}

bool dfInit() {
    S.println("[DF] initializing DFPlayer...");
    // isACK=true (verified handshake), doReset=false (fast start)
    if (!player.begin(dfSerial, /*isACK*/true, /*doReset*/false)) {
        S.println("[DF] begin FAILED — check wiring (RX->5 via 1k, TX->6) and the microSD");
        return false;
    }
    player.volume(DF_DEFAULT_VOLUME);
    S.printf("[DF] online — volume %d/30\n", DF_DEFAULT_VOLUME);
    return true;
}

void setup() {
    S.begin(115200);
    delay(300);
    S.println("\n================================");
    S.println("  SFGE AUDIO NODE — starting");
    S.println("================================");

    pinMode(DF_BUSY_PIN, INPUT_PULLUP);
    dfSerial.begin(9600, SERIAL_8N1, DF_RX_PIN, DF_TX_PIN);
    delay(800);                     // DFPlayer boot time after power-on
    dfOk = dfInit();
    if (!dfOk) dfRetryAt = millis() + 10000;   // keep retrying from loop()

    // BOOT SELF-TEST: play the first file on the card by index (works no
    // matter how the files are named) — audible proof the DFPlayer chain
    // works end to end. Sent even if begin() failed, in case only the
    // module's ACK line is flaky.
    delay(300);
    S.println("[DF] self-test: playing first file on card");
    player.play(1);

    WiFi.mode(WIFI_STA);
    sfge::meshInit(NODE_ID, sfge::ROLE_PANEL, onMeshMsg);
    S.println("[NODE] Ready — waiting for the track map + play commands");
}

void loop() {
    sfge::eventRetryService();
    dfPrintStatus();

    // DFPlayer didn't answer at boot? Retry every 10 s (fixed wiring or a
    // late-inserted card comes up without a reboot)
    if (!dfOk && millis() > dfRetryAt) {
        dfRetryAt = millis() + 10000;
        dfOk = dfInit();
    }

    // New request? Park it — a clip already playing is never interrupted;
    // the newest request waits for the BUSY line, then plays. (Latest wins
    // while waiting.)
    static char deferredPlay[32] = "";
    if (pendingPlayFlag) {
        pendingPlayFlag = false;
        strlcpy(deferredPlay, pendingPlay, sizeof(deferredPlay));
        if (digitalRead(DF_BUSY_PIN) == LOW)
            S.printf("[DF] '%s' queued (busy)\n", deferredPlay);
    }
    if (deferredPlay[0] && digitalRead(DF_BUSY_PIN) != LOW) {
        char what[32];
        strlcpy(what, deferredPlay, sizeof(what));
        deferredPlay[0] = 0;
        // A bare number plays directly; otherwise resolve the taught map
        int t = -1;
        bool numeric = what[0] && strspn(what, "0123456789") == strlen(what);
        if (numeric) t = atoi(what);
        else         t = lookupTrack(what);
        if (!dfOk) {
            S.printf("[DF] can't play '%s' — DFPlayer offline\n", what);
        } else if (t > 0) {
            S.printf("[DF] play '%s' -> /mp3/%04d.mp3\n", what, t);
            player.playMp3Folder(t);
            delay(60);   // give BUSY a beat to assert so we don't double-fire
        } else {
            S.printf("[DF] unknown clip '%s' (map has %d entries)\n", what, trackMapCount);
        }
    }
    if (pendingVolume >= 0) {
        int v = pendingVolume;
        pendingVolume = -1;
        if (dfOk) { S.printf("[DF] volume %d\n", v); player.volume(v > 30 ? 30 : v); }
    }
    if (pendingStop) { pendingStop = false; if (dfOk) player.stop(); }
    if (restartRequested) { restartRequested = false; delay(300); ESP.restart(); }

    // Log play start/stop via the BUSY line (LOW = playing)
    static bool wasBusy = false;
    bool busy = (digitalRead(DF_BUSY_PIN) == LOW);
    if (busy != wasBusy) {
        wasBusy = busy;
        S.printf("[DF] %s\n", busy ? "playing..." : "done");
    }

    // Heartbeat so the GM sees the node
    static unsigned long lastBeat = 0;
    if (millis() - lastBeat > 10000) {
        lastBeat = millis();
        sfge::sendPing(millis() / 1000);
    }

    delay(5);
}
