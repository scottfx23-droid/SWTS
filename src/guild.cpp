/*
 * SFGE Guild Master — Bounty Hunter Guild prop
 * Waveshare ESP32-S3-DualEye-LCD-1.28 (ESP32-S3R8, 16MB flash, 8MB PSRAM)
 *
 * Two round 240x240 GC9A01 displays play a pair of animated eyes; an
 * ES8311 codec drives a speaker for the Guild Master's voice. Players
 * hunt bounties by scanning the target's CHAIN:<bounty_id> tag with
 * their datapad, then connect here and turn the contract in. The Guild
 * Master validates against the mesh bounty ledger, broadcasts the claim
 * (which pays the hunter), plays a per-bounty speech line, and the eyes
 * react.
 *
 * Speech files (16-bit PCM WAV, mono or stereo, on the TF card):
 *   /SFGE/speech/greet.wav         — player connects
 *   /SFGE/speech/<bounty_id>.wav   — that contract turned in
 *   /SFGE/speech/turnin.wav        — fallback turn-in line
 *   /SFGE/speech/denied.wav        — invalid turn-in
 *   /SFGE/speech/idle.wav          — occasional idle chatter
 *
 * Hardware (from the Waveshare wiki):
 *   LCDs (shared SPI): CLK=41, DIN(MOSI)=42, DC=45
 *     Eye L: CS=47, RST=48, BL=46     Eye R: CS=38, RST=8, BL=39
 *   Audio I2S: MCLK=12, BCLK=13, LRCK=14, DOUT(spk)=15, DIN(mic)=16
 *   Codec ctrl I2C: SDA=11, SCL=10   (ES8311 @ 0x18)
 *   TF card (SDMMC 1-bit): CLK=17, CMD=21, D0=18
 */
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <SD_MMC.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <Audio.h>               // ESP32-audioI2S — WAV + MP3 playback
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "sfge_mesh.h"
#include "sfge_proximity.h"
#include "sfge_ota.h"            // firmware-from-TF updates
#include "sfge_test_configs.h"   // boot-time test provisioning (see header to disable)

#define S Serial

// ═══════════════════════════════════════
//  PINS (Waveshare ESP32-S3-DualEye-LCD-1.28)
// ═══════════════════════════════════════
#define LCD_CLK   41
#define LCD_MOSI  42
#define LCD_DC    45
#define EYE_L_CS  47
#define EYE_L_RST 48
#define EYE_L_BL  46
#define EYE_R_CS  38
#define EYE_R_RST 8
#define EYE_R_BL  39

#define I2S_MCLK  12
#define I2S_BCLK  13
#define I2S_LRCK  14
#define I2S_DOUT  15

#define CODEC_SDA 11
#define CODEC_SCL 10
#define ES8311_ADDR 0x18

#define SD_CLK_PIN 17
#define SD_CMD_PIN 21
#define SD_D0_PIN  18

// ═══════════════════════════════════════
//  DISPLAYS — two GC9A01 on one SPI bus, separate CS
// ═══════════════════════════════════════
class EyeLCD : public lgfx::LGFX_Device {
    lgfx::Panel_GC9A01 _panel;
    lgfx::Bus_SPI _bus;
public:
    EyeLCD(int cs, int rst) {
        { auto c = _bus.config();
          c.spi_host = SPI2_HOST; c.spi_mode = 0;
          c.freq_write = 40000000; c.freq_read = 16000000;
          c.pin_sclk = LCD_CLK; c.pin_mosi = LCD_MOSI; c.pin_miso = -1; c.pin_dc = LCD_DC;
          c.use_lock = true;
          _bus.config(c); _panel.setBus(&_bus); }
        { auto c = _panel.config();
          c.pin_cs = cs; c.pin_rst = rst; c.pin_busy = -1;
          c.panel_width = 240; c.panel_height = 240;
          c.offset_x = 0; c.offset_y = 0;
          c.invert = true; c.rgb_order = false;
          _panel.config(c); }
        setPanel(&_panel);
    }
};

EyeLCD eyeL(EYE_L_CS, EYE_L_RST);
EyeLCD eyeR(EYE_R_CS, EYE_R_RST);
LGFX_Sprite eyeFrame(&eyeL);   // one 240x240 PSRAM frame, pushed to each eye

// ═══════════════════════════════════════
//  EYE ANIMATION
//  One shared state drives both eyes (they track together). Moods tint
//  the iris and shape the lids; saccades + blinks run continuously.
// ═══════════════════════════════════════
enum EyeMood : uint8_t { EYE_IDLE, EYE_ALERT, EYE_HAPPY, EYE_ANGRY, EYE_TALK };
EyeMood eyeMood = EYE_IDLE;
unsigned long eyeMoodUntil = 0;   // revert to IDLE after this (0 = sticky)

float pupilX = 0, pupilY = 0;          // current offset from center
float pupilTX = 0, pupilTY = 0;        // saccade target
unsigned long nextSaccade = 0;
unsigned long nextBlink = 3000;
int blinkPhase = 0;                    // 0 = open, 1..4 closing, 5..8 opening
unsigned long lastEyeFrame = 0;

void setEyeMood(EyeMood m, uint32_t forMs) {
    eyeMood = m;
    eyeMoodUntil = forMs ? millis() + forMs : 0;
    if (m == EYE_ALERT || m == EYE_HAPPY) { pupilTX = 0; pupilTY = 0; }
}

uint32_t eyeIrisColor() {
    switch (eyeMood) {
        case EYE_ALERT: return 0x00AAFF;
        case EYE_HAPPY: return 0x00DD44;
        case EYE_ANGRY: return 0xEE2200;
        case EYE_TALK:  return 0xFFCC22;
        default:        return 0xFFAA00;   // guild amber
    }
}

// Draw one frame into the shared sprite and push to a display.
// mirror=true for the right eye (glints + membrane sweep direction flip,
// so the pair reads as one creature).
void drawEye(lgfx::LGFX_Device &dev, bool mirror) {
    const int CX = 120, CY = 120;
    eyeFrame.fillSprite(TFT_BLACK);

    uint32_t ic = eyeIrisColor();
    uint8_t r = (ic >> 16) & 0xFF, g = (ic >> 8) & 0xFF, b = ic & 0xFF;

    // Sclera: sickly pale-green alien ground with a glowing rim
    eyeFrame.fillCircle(CX, CY, 120, eyeFrame.color888(r / 4, g / 4, b / 5));
    eyeFrame.fillCircle(CX, CY, 116, eyeFrame.color888(26, 34, 18));
    // Faint veins radiating in from the rim
    for (int a = 15; a < 360; a += 40) {
        float ar = (a + (int)(pupilX)) * DEG_TO_RAD;
        eyeFrame.drawLine(CX + (int)(cosf(ar) * 116), CY + (int)(sinf(ar) * 116),
                          CX + (int)(cosf(ar) * 88),  CY + (int)(sinf(ar) * 88),
                          eyeFrame.color888(70, 40, 30));
    }

    int px = CX + (int)pupilX;
    int py = CY + (int)pupilY;

    // Iris: huge — fills the screen edge to edge
    int irisR = (eyeMood == EYE_ALERT) ? 112 : 102;
    eyeFrame.fillCircle(px, py, irisR, eyeFrame.color888(r / 4, g / 4, b / 4));
    eyeFrame.fillCircle(px, py, irisR - 5, eyeFrame.color888(r / 2, g / 2, b / 2));
    eyeFrame.fillCircle(px, py, irisR - 20, eyeFrame.color888(
        (uint8_t)min(255, r * 3 / 4), (uint8_t)min(255, g * 3 / 4), (uint8_t)min(255, b * 3 / 4)));
    // Striations across the whole iris
    for (int a = 0; a < 360; a += 10) {
        float ar = a * DEG_TO_RAD;
        eyeFrame.drawLine(px + (int)(cosf(ar) * 16), py + (int)(sinf(ar) * 16),
                          px + (int)(cosf(ar) * (irisR - 6)), py + (int)(sinf(ar) * (irisR - 6)),
                          eyeFrame.color888(r, g, b));
    }
    // Inner glow ring around the pupil
    eyeFrame.drawCircle(px, py, 38, eyeFrame.color888(
        (uint8_t)min(255, (int)r + 60), (uint8_t)min(255, (int)g + 60), (uint8_t)min(255, (int)b + 40)));

    // Pupil: vertical reptilian slit (narrows when angry, gapes when alert)
    int slitW = (eyeMood == EYE_ANGRY) ? 9 : (eyeMood == EYE_ALERT) ? 34 : 18;
    int slitH = irisR - 12;
    eyeFrame.fillEllipse(px, py, slitW, slitH, TFT_BLACK);
    eyeFrame.fillEllipse(px, py, slitW > 4 ? slitW - 3 : 1, slitH - 4, eyeFrame.color888(8, 10, 6));

    // Twin glints (offset pair reads very non-human)
    int gx = mirror ? px + 26 : px - 26;
    eyeFrame.fillCircle(gx, py - 34, 9, TFT_WHITE);
    eyeFrame.fillCircle(px + (mirror ? -16 : 16), py + 38, 5,
                        eyeFrame.color888(200, 255, 220));

    // Blink: the nictitating membrane sweeps down the eye.
    // (Panels run at rotation 1 for the sideways mount, so the physical
    // "top-to-bottom" blink is a sweep along draw-Y.)
    if (blinkPhase > 0) {
        static const int memSeq[9] = { 0, 80, 170, 240, 240, 240, 170, 80, 0 };
        int sweep = memSeq[blinkPhase];
        uint32_t mem = eyeFrame.color888(34, 44, 26);   // translucent-looking lid tone
        eyeFrame.fillRect(0, 0, 240, sweep, mem);
        if (sweep > 0 && sweep < 240)
            eyeFrame.fillRect(0, sweep - 4, 240, 4, eyeFrame.color888(60, 80, 45));
    }

    // Mood lids (on top of everything; same axis as the blink)
    if (eyeMood == EYE_HAPPY)
        eyeFrame.fillRect(0, 196, 240, 44, TFT_BLACK);   // squint-smile
    if (eyeMood == EYE_ANGRY) {
        eyeFrame.fillRect(0, 0, 240, 34, TFT_BLACK);     // glower
        eyeFrame.fillTriangle(mirror ? 240 : 0, 34,
                              mirror ? 0 : 240, 34,
                              mirror ? 0 : 240, 96, TFT_BLACK);
    }

    eyeFrame.pushSprite(&dev, 0, 0);
}

void updateEyes() {
    unsigned long now = millis();
    if (now - lastEyeFrame < 45) return;   // ~22 fps budget, shared by 2 pushes
    lastEyeFrame = now;

    if (eyeMoodUntil && now > eyeMoodUntil) { eyeMood = EYE_IDLE; eyeMoodUntil = 0; }

    // Eyes stay fixed — blinking (and mood) carries all the life
    pupilX = 0;
    pupilY = 0;

    // Blinks
    if (blinkPhase > 0) {
        blinkPhase = (blinkPhase + 1) % 9;
    } else if (now > nextBlink) {
        nextBlink = now + 2500 + esp_random() % 5500;
        blinkPhase = 1;
    }

    // Static stare between blinks — only redraw when something changed
    static int prevBlink = -1;
    static EyeMood prevMood = EYE_IDLE;
    if (blinkPhase == prevBlink && eyeMood == prevMood) return;
    prevBlink = blinkPhase;
    prevMood = eyeMood;

    drawEye(eyeL, false);
    drawEye(eyeR, true);
}

// ═══════════════════════════════════════
//  AUDIO — ES8311 codec + ESP32-audioI2S (plays WAV and MP3)
// ═══════════════════════════════════════
Audio audio;
bool codecOk = false;
int  audioVolume = 18;        // 0-21, config behavior.volume
char speechQueue[32] = "";    // one pending clip (base name, latest wins)
bool wasSpeaking = false;

void es8311Write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

// Minimal DAC-playback init: I2S slave, MCLK = 256*fs, 16-bit
bool es8311Init() {
    Wire.begin(CODEC_SDA, CODEC_SCL, 100000);
    Wire.beginTransmission(ES8311_ADDR);
    if (Wire.endTransmission() != 0) { S.println("[AUDIO] ES8311 not found"); return false; }

    es8311Write(0x00, 0x1F);   // reset
    delay(20);
    es8311Write(0x00, 0x00);
    es8311Write(0x00, 0x80);   // power on, slave mode
    es8311Write(0x01, 0xBF);   // clocks on, MCLK from pin
    es8311Write(0x02, 0x00);   // pre-div 1, pre-mult 1 (MCLK = 256*fs)
    es8311Write(0x03, 0x10);   // ADC OSR
    es8311Write(0x04, 0x10);   // DAC OSR
    es8311Write(0x05, 0x00);   // adc/dac div 1
    es8311Write(0x06, 0x03);   // bclk div (master-mode only; benign as slave)
    es8311Write(0x07, 0x00);
    es8311Write(0x08, 0xFF);
    es8311Write(0x09, 0x0C);   // SDP in: I2S, 16-bit
    es8311Write(0x0A, 0x0C);   // SDP out: I2S, 16-bit
    es8311Write(0x0D, 0x01);   // power up analog
    es8311Write(0x0E, 0x02);   // PGA/modulator
    es8311Write(0x12, 0x00);   // DAC power up
    es8311Write(0x13, 0x10);   // HP drive
    es8311Write(0x37, 0x08);   // DAC EQ bypass
    es8311Write(0x32, 0xBF);   // DAC volume (~75%)
    S.println("[AUDIO] ES8311 ready");
    return true;
}

bool audioStart() {
    audio.setPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT, I2S_MCLK);
    audio.setVolume(audioVolume);   // 0-21
    return true;
}

// Queue a speech clip by BASE NAME (no extension). The player looks for
// /SFGE/speech/<name>.wav then <name>.mp3. Latest queued clip wins.
void speechQueueFile(const char *name) {
    snprintf(speechQueue, sizeof(speechQueue), "%s", name);
}

bool speechPlay(const char *base) {
    if (!codecOk) return false;
    char path[72];
    snprintf(path, sizeof(path), "/SFGE/speech/%s.wav", base);
    if (!SD_MMC.exists(path)) {
        snprintf(path, sizeof(path), "/SFGE/speech/%s.mp3", base);
        if (!SD_MMC.exists(path)) {
            S.printf("[AUDIO] missing %s(.wav/.mp3)\n", base);
            return false;
        }
    }
    if (audio.isRunning()) audio.stopSong();
    bool ok = audio.connecttoFS(SD_MMC, path);
    if (ok) {
        setEyeMood(EYE_TALK, 0);
        S.printf("[AUDIO] playing %s\n", path);
    }
    return ok;
}

void speechService() {
    audio.loop();   // feed the decoder every pass

    bool speaking = audio.isRunning();
    if (wasSpeaking && !speaking && eyeMood == EYE_TALK)
        setEyeMood(EYE_IDLE, 0);
    wasSpeaking = speaking;

    // Start a queued clip once the current one is done
    if (!speaking && speechQueue[0]) {
        char base[32];
        strlcpy(base, speechQueue, sizeof(base));
        speechQueue[0] = '\0';
        speechPlay(base);
    }
}

// ═══════════════════════════════════════
//  CONFIG (/SFGE/config.json on the TF card)
// ═══════════════════════════════════════
struct GuildConfig {
    char id[16]      = "GUILD_01";
    char name[40]    = "Guild Master";
    char ssid[24]    = "SFGE_GUILD_01";
    char faction[14] = "neutral";
};
GuildConfig cfg;
bool sdOk = false;

bool loadConfig() {
    File f = SD_MMC.open("/SFGE/config.json", FILE_READ);
    if (!f) { S.println("[CFG] no /SFGE/config.json — defaults"); return false; }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) { S.printf("[CFG] parse error: %s\n", err.c_str()); return false; }
    strlcpy(cfg.id,      doc["prop"]["id"]      | cfg.id,      sizeof(cfg.id));
    strlcpy(cfg.name,    doc["prop"]["name"]    | cfg.name,    sizeof(cfg.name));
    strlcpy(cfg.ssid,    doc["prop"]["ssid"]    | cfg.ssid,    sizeof(cfg.ssid));
    strlcpy(cfg.faction, doc["prop"]["faction"] | cfg.faction, sizeof(cfg.faction));
    audioVolume = doc["behavior"]["volume"] | audioVolume;   // 0-21
    if (audioVolume < 0) audioVolume = 0;
    if (audioVolume > 21) audioVolume = 21;
    sfge_prox::loadConfig(doc);
    S.printf("[CFG] %s (%s) vol=%d\n", cfg.name, cfg.id, audioVolume);
    return true;
}

// ═══════════════════════════════════════
//  BOUNTY LEDGER — mirrored from the GM's mesh broadcasts
// ═══════════════════════════════════════
struct GuildBounty {
    char id[24];
    char target_name[24];
    int  reward;
    bool open;
};
GuildBounty ledger[8];
int ledgerCount = 0;

GuildBounty* ledgerFind(const char *id) {
    for (int i = 0; i < ledgerCount; i++)
        if (strcmp(ledger[i].id, id) == 0) return &ledger[i];
    return nullptr;
}

// ═══════════════════════════════════════
//  HTTP — the datapad turn-in interaction
// ═══════════════════════════════════════
AsyncWebServer server(80);
volatile uint8_t pendingEye = 0;   // 1 alert, 2 happy, 3 angry, 4 celebrate (set on async task)

void handleInfo(AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["prop_id"] = cfg.id;
    doc["name"] = cfg.name;
    doc["type"] = "guild";
    doc["faction"] = cfg.faction;
    JsonObject feat = doc["features"].to<JsonObject>();
    feat["has_nfc"] = false;
    feat["has_minigame"] = false;
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
}

void handleInteract(AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t, size_t) {
    JsonDocument reqDoc;
    if (deserializeJson(reqDoc, data, len)) {
        req->send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }
    const char *action   = reqDoc["action"] | "greet";
    const char *callsign = reqDoc["player"]["callsign"] | "UNKNOWN";
    S.printf("[HTTP] %s by %s\n", action, callsign);

    int stationRssi = 0;
    if (!sfge_prox::check(&stationRssi)) {
        char body[64];
        snprintf(body, sizeof(body), "{\"error\":\"too_far\",\"rssi\":%d}", stationRssi);
        req->send(403, "application/json", body);
        return;
    }

    JsonDocument resp;
    resp["type"] = "dialogue";
    JsonObject sp = resp["speaker"].to<JsonObject>();
    sp["name"] = cfg.name;
    JsonArray lines = resp["lines"].to<JsonArray>();
    JsonArray choices = resp["choices"].to<JsonArray>();

    if (strncmp(action, "turn_in:", 8) == 0) {
        const char *bid = action + 8;
        GuildBounty *b = ledgerFind(bid);
        // The hunter must actually hold the chain code (datapad sends its
        // captured list with every request)
        bool captured = false;
        for (const char *c : reqDoc["player"]["captured"].as<JsonArray>())
            if (c && strcmp(c, bid) == 0) { captured = true; break; }

        if (!b || !b->open || !captured) {
            pendingEye = 3;   // angry
            speechQueueFile("denied");
            JsonObject l1 = lines.add<JsonObject>();
            l1["text"] = !b ? "THE GUILD HAS NO SUCH CONTRACT."
                       : !b->open ? "THAT CONTRACT IS ALREADY SETTLED."
                       : "YOUR CHAIN CODE DOES NOT VERIFY. BRING ME PROOF.";
            l1["style"] = "system";
        } else {
            b->open = false;
            pendingEye = 2;   // happy
            // Per-bounty voice line (<id>.wav or .mp3), else the stock one
            char p1[72], p2[72];
            snprintf(p1, sizeof(p1), "/SFGE/speech/%s.wav", b->id);
            snprintf(p2, sizeof(p2), "/SFGE/speech/%s.mp3", b->id);
            speechQueueFile((SD_MMC.exists(p1) || SD_MMC.exists(p2)) ? b->id : "turnin");

            // The claim broadcast pays the hunter and closes the bounty on
            // every datapad + the GM console
            sfge::gmClaimBounty(b->id, callsign, callsign, b->reward);
            sfge::gmClaimBounty(b->id, callsign, callsign, b->reward);
            char eventId[40];
            snprintf(eventId, sizeof(eventId), "bounty_turned_in:%s", b->id);
            sfge::gmTriggerEvent(eventId, "Bounty turned in", 0, callsign);
            resp["game_event"] = eventId;

            char msg[96];
            snprintf(msg, sizeof(msg), "CONTRACT FULFILLED. %d CREDITS AUTHORIZED.\nThe Guild remembers good work, %s.",
                     b->reward, callsign);
            JsonObject l1 = lines.add<JsonObject>();
            l1["text"] = msg;
            l1["style"] = "system";
            S.printf("[GUILD] %s turned in %s (+%d)\n", callsign, b->id, b->reward);
        }
        JsonObject c1 = choices.add<JsonObject>();
        c1["label"] = "[Disconnect]";
        c1["next_action"] = nullptr;
    }
    else {
        // ── Greeting: list the hunter's verifiable contracts ──
        pendingEye = 1;   // alert — look at the player
        speechQueueFile("greet");

        JsonObject l1 = lines.add<JsonObject>();
        l1["text"] = "THE BOUNTY HUNTERS' GUILD RECOGNIZES YOU.";
        l1["style"] = "system";

        int offered = 0;
        for (const char *c : reqDoc["player"]["captured"].as<JsonArray>()) {
            if (!c || !c[0]) continue;
            GuildBounty *b = ledgerFind(c);
            if (!b || !b->open) continue;
            char lbl[40];
            snprintf(lbl, sizeof(lbl), "Turn in: %s", b->target_name);
            char act[36];
            snprintf(act, sizeof(act), "turn_in:%s", b->id);
            JsonObject ch = choices.add<JsonObject>();
            ch["label"] = lbl;
            ch["next_action"] = act;
            offered++;
        }
        JsonObject l2 = lines.add<JsonObject>();
        if (offered == 0) {
            l2["text"] = "No completed contracts on file. Acquire a target, scan its chain code, and return to me.";
        } else {
            l2["text"] = "Your chain codes verify. The Guild pays on delivery.";
        }
        l2["style"] = "speech";

        JsonObject cd = choices.add<JsonObject>();
        cd["label"] = "[Disconnect]";
        cd["next_action"] = nullptr;
    }

    String out;
    serializeJson(resp, out);
    req->send(200, "application/json", out);
}

// ═══════════════════════════════════════
//  MESH
// ═══════════════════════════════════════
void onMeshMsg(const sfge::MeshHeader *hdr, const uint8_t *payload, int len) {
    switch (hdr->type) {
        case sfge::MSG_BOUNTY: {
            if (len < (int)sizeof(sfge::MeshBounty)) return;
            const sfge::MeshBounty *b = (const sfge::MeshBounty *)payload;
            GuildBounty *g = ledgerFind(b->bounty_id);
            if (!g && ledgerCount < 8) {
                g = &ledger[ledgerCount++];
                memset(g, 0, sizeof(*g));
                strlcpy(g->id, b->bounty_id, sizeof(g->id));
            }
            if (g) {
                strlcpy(g->target_name, b->target_name, sizeof(g->target_name));
                g->reward = b->reward;
                g->open = true;
                S.printf("[LEDGER] contract open: %s (%d)\n", g->id, g->reward);
            }
            break;
        }
        case sfge::MSG_BOUNTY_CLAIM: {
            if (len < (int)sizeof(sfge::MeshBountyClaim)) return;
            const sfge::MeshBountyClaim *c = (const sfge::MeshBountyClaim *)payload;
            GuildBounty *g = ledgerFind(c->bounty_id);
            if (g) g->open = false;
            break;
        }
        case sfge::MSG_BOUNTY_CANCEL: {
            if (len < (int)sizeof(sfge::MeshBountyCancel)) return;
            const sfge::MeshBountyCancel *c = (const sfge::MeshBountyCancel *)payload;
            GuildBounty *g = ledgerFind(c->bounty_id);
            if (g) g->open = false;
            break;
        }
        case sfge::MSG_SYNC_REQUEST:
            sfge::sendPing(millis() / 1000);
            break;
        case sfge::MSG_RESET:
            delay(200);
            ESP.restart();
            break;
        case sfge::MSG_NEW_GAME:
            // Session wipe: reboot clears the contract ledger
            S.println("[MESH] NEW GAME — resetting");
            delay(300 + (esp_random() % 400));
            ESP.restart();
            break;
        case sfge::MSG_EVENT: {
            // Victory finale: no strip on this prop, so the eyes celebrate
            if (len < (int)sizeof(sfge::MeshEvent)) break;
            const sfge::MeshEvent *e = (const sfge::MeshEvent *)payload;
            if (sfge::isCelebrationEvent(e->event_id)) pendingEye = 4;
            break;
        }
        default: break;
    }
}

// ═══════════════════════════════════════
//  SETUP / LOOP
// ═══════════════════════════════════════
void setup() {
    S.begin(115200);
    delay(300);
    S.println("\n================================");
    S.println("  SFGE GUILD MASTER — starting");
    S.println("================================");

    // Displays + backlights
    pinMode(EYE_L_BL, OUTPUT); digitalWrite(EYE_L_BL, HIGH);
    pinMode(EYE_R_BL, OUTPUT); digitalWrite(EYE_R_BL, HIGH);
    eyeL.init();
    eyeR.init();
    // The board is mounted with the displays turned 90°, and the right
    // panel is flipped 180° relative to the left — rotate each so the
    // blink sweeps top-to-bottom on both
    eyeL.setRotation(1);
    eyeR.setRotation(3);
    eyeFrame.setColorDepth(16);
    eyeFrame.setPsram(true);
    if (!eyeFrame.createSprite(240, 240)) S.println("[EYE] sprite alloc FAILED");
    drawEye(eyeL, false);
    drawEye(eyeR, true);
    S.println("[EYE] displays up");

    // TF card (SDMMC 1-bit)
    SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN);
    if (SD_MMC.begin("/sdcard", true)) {
        sdOk = true;
        S.printf("[SD] Mounted %lluMB\n", SD_MMC.cardSize() / (1024 * 1024));
        sfge_ota::checkAndFlash(SD_MMC);
        sfge_test::writeTestConfigs(SD_MMC);
        loadConfig();
    } else {
        S.println("[SD] Mount failed — insert configured card");
    }

    // Audio (ES8311 codec + WAV/MP3 player)
    codecOk = es8311Init();
    if (codecOk) audioStart();
    else S.println("[AUDIO] disabled");

    // WiFi AP + mesh
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    WiFi.softAP(cfg.ssid, nullptr, MESH_CHANNEL);
    S.printf("[WIFI] AP '%s' ch=%d\n", cfg.ssid, MESH_CHANNEL);

    server.on("/api/info", HTTP_GET, handleInfo);
    server.on("/api/interact", HTTP_POST, [](AsyncWebServerRequest *r) {}, nullptr, handleInteract);
    server.begin();

    sfge::meshInit(cfg.id, sfge::ROLE_PANEL, onMeshMsg);
    S.println("[GUILD] Ready — the Guild is open for business");
}

void loop() {
    sfge::eventRetryService();   // re-fire gameplay events until datapads/GM confirm

    // Eye reactions queued by the HTTP task
    if (pendingEye) {
        uint8_t pe = pendingEye;
        pendingEye = 0;
        if (pe == 1) setEyeMood(EYE_ALERT, 6000);
        if (pe == 2) setEyeMood(EYE_HAPPY, 5000);
        if (pe == 3) setEyeMood(EYE_ANGRY, 5000);
        if (pe == 4) setEyeMood(EYE_HAPPY, 12000);   // victory celebration
    }

    updateEyes();
    speechService();

    // Occasional idle grumbling
    static unsigned long nextIdle = 60000;
    if (sdOk && codecOk && !audio.isRunning() && millis() > nextIdle) {
        nextIdle = millis() + 45000 + esp_random() % 60000;
        speechQueueFile("idle");
    }

    // Heartbeat
    static unsigned long lastBeat = 0;
    if (millis() - lastBeat > 5000) {
        lastBeat = millis();
        sfge::sendPing(millis() / 1000);
        sfge::sendPropStatus(millis() / 1000, 0, false, "");
    }

    delay(2);
}
