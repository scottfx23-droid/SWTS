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
#include "sfge_scenario.h"       // GM-selected scenario -> content folder
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
unsigned long lastEyeFrame = 0;

// ── Blink engine — "Data-style" natural blinking ──
// Star Trek's Data used a chaotic function for his blink timing because a
// fixed rhythm instantly reads as mechanical. Same tricks here:
//   * intervals drawn from a bell-ish distribution (sum of 3 uniforms),
//     never a uniform spread, with occasional long unnerving stares
//   * ~1 in 5 blinks is a quick double, ~1 in 7 only half-closes
//   * the motion is asymmetric: fast close, a beat of hold, slow settle open
//   * mood scales the rate — ALERT blinks quicker, ANGRY slow and deliberate
unsigned long nextBlink   = 3000;
unsigned long blinkStart  = 0;         // 0 = eye fully open
int  blinkDepth    = 240;              // how far the membrane sweeps (px)
int  blinkSweep    = 0;                // current sweep this frame (px)
bool blinkIsDouble = false;            // a second flick follows this one
#define BLINK_CLOSE_MS 90
#define BLINK_HOLD_MS  50
#define BLINK_OPEN_MS  190

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

    // Blink: the nictitating membrane sweeps down the eye by blinkSweep px.
    // (Panels run at rotation 1 for the sideways mount, so the physical
    // "top-to-bottom" blink is a sweep along draw-Y.)
    if (blinkSweep > 0) {
        uint32_t mem = eyeFrame.color888(34, 44, 26);   // translucent-looking lid tone
        eyeFrame.fillRect(0, 0, 240, blinkSweep, mem);
        if (blinkSweep < 240)
            eyeFrame.fillRect(0, blinkSweep - 4, 240, 4, eyeFrame.color888(60, 80, 45));
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

    // ── Natural blink engine (see the notes at the state block) ──
    int sweep = 0;
    if (blinkStart) {
        unsigned long t = now - blinkStart;
        if (t < BLINK_CLOSE_MS) {
            // Fast snap shut
            sweep = (int)(blinkDepth * (float)t / BLINK_CLOSE_MS);
        } else if (t < BLINK_CLOSE_MS + BLINK_HOLD_MS) {
            sweep = blinkDepth;                      // a beat fully closed
        } else if (t < BLINK_CLOSE_MS + BLINK_HOLD_MS + BLINK_OPEN_MS) {
            // Slow settle open: fast at first, decelerating at the end
            float u = (float)(t - BLINK_CLOSE_MS - BLINK_HOLD_MS) / BLINK_OPEN_MS;
            float v = 1.0f - u;
            sweep = (int)(blinkDepth * v * v);
        } else {
            blinkStart = 0;
            if (blinkIsDouble) {
                // Quick second flick lands right after the first
                blinkIsDouble = false;
                nextBlink = now + 220 + esp_random() % 240;
            }
        }
    }
    if (!blinkStart && now > nextBlink) {
        blinkStart = now;
        // Mostly full blinks; sometimes a lazy half-close
        blinkDepth = (esp_random() % 100 < 14) ? 130 + (int)(esp_random() % 70) : 240;
        blinkIsDouble = (esp_random() % 100 < 22);
        // Interval: sum of three uniforms ~ bell curve centered ~4.5 s
        uint32_t iv = 1200 + esp_random() % 2200 + esp_random() % 2200
                           + esp_random() % 2200;
        if (eyeMood == EYE_ALERT) iv = iv * 3 / 5;   // keyed-up: quicker
        if (eyeMood == EYE_ANGRY) iv = iv * 8 / 5;   // slow and deliberate
        if (esp_random() % 100 < 12)                 // the long alien stare
            iv += 4000 + esp_random() % 5000;
        nextBlink = now + iv;
    }

    // Static stare between blinks — only redraw when the lid moved a
    // visible amount (8 px quantum) or the mood changed
    int q = (sweep + 4) / 8;
    static int prevQ = -1;
    static EyeMood prevMood = EYE_IDLE;
    if (q == prevQ && eyeMood == prevMood) return;
    prevQ = q;
    prevMood = eyeMood;
    blinkSweep = sweep;

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

// ── Remote audio (DFPlayer node) ──
// behavior.audio_node names a mesh audio node (e.g. "AUDIO_01"); speech then
// plays THERE: behavior.speech_tracks maps clip names to the numbered MP3s
// on the DFPlayer's own card (/mp3/0001.mp3 = track 1). Empty = onboard
// ES8311 as before.
char audioNode[16] = "";
struct SpeechTrack { char name[24]; int track; };
SpeechTrack speechTracks[16];
int speechTrackCount = 0;

// Idle grumble cadence (behavior.idle_min_s / idle_max_s)
int idleMinS = 120;
int idleMaxS = 300;

int trackFor(const char *base) {
    for (int i = 0; i < speechTrackCount; i++)
        if (strcasecmp(speechTracks[i].name, base) == 0) return speechTracks[i].track;
    return -1;
}

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
    // Clock tree FIRST, then power-on (esp-bsp order). REG01 bit7 selects the
    // codec's clock source: 0 = the MCLK pin (which the audio lib drives at
    // 256*fs on GPIO12), 1 = derive from BCLK. We previously set bit7 (0xBF),
    // so the dividers below were computed against the wrong clock -> silence.
    es8311Write(0x01, 0x3F);   // all clocks on, clock from the MCLK pin
    es8311Write(0x02, 0x00);   // pre-div 1, pre-mult 1 (MCLK = 256*fs)
    es8311Write(0x03, 0x10);   // ADC OSR
    es8311Write(0x16, 0x24);   // ADC config (reference sequence)
    es8311Write(0x04, 0x10);   // DAC OSR
    es8311Write(0x05, 0x00);   // adc/dac div 1
    es8311Write(0x06, 0x03);   // bclk div (master-mode only; benign as slave)
    es8311Write(0x07, 0x00);
    es8311Write(0x08, 0xFF);
    es8311Write(0x09, 0x0C);   // SDP in: I2S, 16-bit
    es8311Write(0x0A, 0x0C);   // SDP out: I2S, 16-bit
    es8311Write(0x00, 0x80);   // CSM power on, slave mode
    delay(10);
    es8311Write(0x0D, 0x01);   // power up analog
    es8311Write(0x0E, 0x02);   // PGA/modulator
    es8311Write(0x12, 0x00);   // DAC power up
    es8311Write(0x13, 0x10);   // HP drive (output stage on)
    es8311Write(0x1C, 0x6A);   // ADC EQ bypass (reference sequence)
    es8311Write(0x37, 0x08);   // DAC EQ bypass
    es8311Write(0x31, 0x00);   // DAC UNMUTE (never set before — muted = silent)
    es8311Write(0x32, 0xBF);   // DAC volume (~75%)
    es8311Write(0x45, 0x00);   // GP control defaults
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

// Teach the audio node the name->track table (its DFPlayer only knows
// numbers and hides its card, so the node learns the map from OUR config).
// Called after mesh init and every ~30 s so a rebooted node re-learns it.
void speechMapBroadcast() {
    if (!audioNode[0]) return;
    for (int i = 0; i < speechTrackCount; i++) {
        char kv[32];
        snprintf(kv, sizeof(kv), "%s=%d", speechTracks[i].name, speechTracks[i].track);
        sfge::gmOverride(audioNode, "map", kv);
        delay(25);
    }
}

bool speechPlay(const char *base) {
    // Remote mode: tell the DFPlayer audio node the clip NAME over the
    // mesh — the node resolves it against the map we taught it
    if (audioNode[0]) {
        if (trackFor(base) <= 0) {
            S.printf("[AUDIO] no track mapped for '%s' (behavior.speech_tracks)\n", base);
            return false;
        }
        sfge::gmOverride(audioNode, "play", base);
        setEyeMood(EYE_TALK, 3500);   // mouth-less talking eyes, ~clip length
        S.printf("[AUDIO] -> %s play '%s'\n", audioNode, base);
        return true;
    }

    if (!codecOk) return false;
    char path[96];
    snprintf(path, sizeof(path), "%s/speech/%s.wav", sfge_scn::root, base);
    if (!SD_MMC.exists(path)) {
        snprintf(path, sizeof(path), "%s/speech/%s.mp3", sfge_scn::root, base);
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
    if (codecOk) audio.loop();   // feed the decoder every pass (onboard mode)

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
    File f = SD_MMC.open(sfge_scn::path("config.json"), FILE_READ);
    if (!f) { S.println("[CFG] no config.json — defaults"); return false; }
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

    idleMinS = doc["behavior"]["idle_min_s"] | idleMinS;
    idleMaxS = doc["behavior"]["idle_max_s"] | idleMaxS;
    if (idleMaxS <= idleMinS) idleMaxS = idleMinS + 60;

    // Remote DFPlayer audio node + the clip-name -> track-number map
    strlcpy(audioNode, doc["behavior"]["audio_node"] | "", sizeof(audioNode));
    speechTrackCount = 0;
    JsonObject st = doc["behavior"]["speech_tracks"].as<JsonObject>();
    for (JsonPair kv : st) {
        if (speechTrackCount >= 16) break;
        SpeechTrack &t = speechTracks[speechTrackCount++];
        strlcpy(t.name, kv.key().c_str(), sizeof(t.name));
        t.track = kv.value().as<int>();
    }

    sfge_prox::loadConfig(doc);
    S.printf("[CFG] %s (%s) vol=%d audio=%s tracks=%d\n", cfg.name, cfg.id,
             audioVolume, audioNode[0] ? audioNode : "onboard", speechTrackCount);
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
        S.printf("[PROX] rejected %s (rssi %d — gate needs >= start threshold)\n",
                 callsign, stationRssi);
        char body[64];
        snprintf(body, sizeof(body), "{\"error\":\"too_far\",\"rssi\":%d}", stationRssi);
        req->send(403, "application/json", body);
        return;
    }
    S.printf("[PROX] %s in range (rssi %d)\n", callsign, stationRssi);

    JsonDocument resp;
    resp["type"] = "dialogue";
    JsonObject sp = resp["speaker"].to<JsonObject>();
    sp["name"] = cfg.name;
    JsonArray lines = resp["lines"].to<JsonArray>();
    JsonArray choices = resp["choices"].to<JsonArray>();

    if (strncmp(action, "accept:", 7) == 0) {
        // ── Take a contract (the 3-foot handshake at the Guild table) ──
        const char *bid = action + 7;
        GuildBounty *b = ledgerFind(bid);
        if (!b || !b->open) {
            pendingEye = 3;
            JsonObject l1 = lines.add<JsonObject>();
            l1["text"] = !b ? "THE GUILD HAS NO SUCH CONTRACT."
                            : "THAT CONTRACT IS ALREADY SETTLED.";
            l1["style"] = "system";
        } else {
            pendingEye = 2;   // pleased — new blood on the books
            resp["accepted_bounty"] = b->id;   // datapad records + persists it

            // Vex delivers the contract briefing: <id>_posted voice line
            // (generic "posted" as fallback)
            char base[32];
            snprintf(base, sizeof(base), "%s_posted", b->id);
            bool haveClip;
            if (audioNode[0]) haveClip = (trackFor(base) > 0);
            else {
                char p1[96], p2[96];
                snprintf(p1, sizeof(p1), "%s/speech/%s.wav", sfge_scn::root, base);
                snprintf(p2, sizeof(p2), "%s/speech/%s.mp3", sfge_scn::root, base);
                haveClip = SD_MMC.exists(p1) || SD_MMC.exists(p2);
            }
            if (haveClip) speechQueueFile(base);
            else if (audioNode[0] ? (trackFor("posted") > 0) : true) speechQueueFile("posted");

            char eventId[48];
            snprintf(eventId, sizeof(eventId), "bounty_accepted:%s", b->id);
            sfge::gmTriggerEvent(eventId, "Contract accepted", 0, callsign);
            char msg[120];
            snprintf(msg, sizeof(msg),
                     "CONTRACT REGISTERED TO %s.\nTarget: %s. Reward: %d credits on delivery.\nScan the target's chain code and return to collect.",
                     callsign, b->target_name, b->reward);
            JsonObject l1 = lines.add<JsonObject>();
            l1["text"] = msg;
            l1["style"] = "system";
            S.printf("[GUILD] %s accepted %s\n", callsign, b->id);
        }
        JsonObject c1 = choices.add<JsonObject>();
        c1["label"] = "[Disconnect]";
        c1["next_action"] = nullptr;
    }
    else if (strncmp(action, "turn_in:", 8) == 0) {
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
            // Per-bounty voice line (<id> clip), else the stock turn-in.
            // Remote mode consults the track MAP — the clips live on the
            // DFPlayer's card, not this prop's TF card.
            bool haveClip;
            if (audioNode[0]) {
                haveClip = (trackFor(b->id) > 0);
            } else {
                char p1[96], p2[96];
                snprintf(p1, sizeof(p1), "%s/speech/%s.wav", sfge_scn::root, b->id);
                snprintf(p2, sizeof(p2), "%s/speech/%s.mp3", sfge_scn::root, b->id);
                haveClip = SD_MMC.exists(p1) || SD_MMC.exists(p2);
            }
            speechQueueFile(haveClip ? b->id : "turnin");

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

        // COLLECT — contracts whose chain code the hunter holds
        int collectable = 0;
        for (const char *c : reqDoc["player"]["captured"].as<JsonArray>()) {
            if (!c || !c[0]) continue;
            GuildBounty *b = ledgerFind(c);
            if (!b || !b->open) continue;
            char lbl[40];
            snprintf(lbl, sizeof(lbl), "Collect bounty: %s", b->target_name);
            char act[36];
            snprintf(act, sizeof(act), "turn_in:%s", b->id);
            JsonObject ch = choices.add<JsonObject>();
            ch["label"] = lbl;
            ch["next_action"] = act;
            collectable++;
        }

        // ACCEPT — open contracts this hunter hasn't taken or captured yet
        int acceptable = 0;
        for (int i = 0; i < ledgerCount; i++) {
            GuildBounty *b = &ledger[i];
            if (!b->open) continue;
            bool mine = false;
            for (const char *a : reqDoc["player"]["accepted"].as<JsonArray>())
                if (a && strcmp(a, b->id) == 0) { mine = true; break; }
            for (const char *c : reqDoc["player"]["captured"].as<JsonArray>())
                if (c && strcmp(c, b->id) == 0) { mine = true; break; }
            if (mine) continue;
            char lbl[44];
            snprintf(lbl, sizeof(lbl), "Accept contract: %s (%d)", b->target_name, b->reward);
            char act[36];
            snprintf(act, sizeof(act), "accept:%s", b->id);
            JsonObject ch = choices.add<JsonObject>();
            ch["label"] = lbl;
            ch["next_action"] = act;
            acceptable++;
        }

        JsonObject l2 = lines.add<JsonObject>();
        if (collectable > 0)      l2["text"] = "Your chain codes verify. The Guild pays on delivery.";
        else if (acceptable > 0)  l2["text"] = "Contracts are open on the board. Take one, hunt, and bring me the chain code.";
        else                      l2["text"] = "No open contracts, no pending deliveries. The board is quiet... for now.";
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
            bool wasOpen = (g && g->open);
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
                // A newly opened contract perks the eyes up but stays SILENT —
                // the <id>_posted voice line plays when a hunter ACCEPTS the
                // contract at the table (3-foot handshake), not at GM posting.
                if (!wasOpen) pendingEye = 1;
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
        case sfge::MSG_SCENARIO: {
            if (len < (int)sizeof(sfge::MeshScenario)) break;
            const sfge::MeshScenario *sc = (const sfge::MeshScenario *)payload;
            if (sdOk && sfge_scn::applyScenario(SD_MMC, sc->id)) {
                delay(400 + (esp_random() % 500));
                ESP.restart();
            }
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
        sfge_scn::begin(SD_MMC);   // GM-selected scenario -> content folder
        loadConfig();
    } else {
        S.println("[SD] Mount failed — insert configured card");
    }

    // What voice lines does this card actually have? (boot diagnostic)
    if (sdOk) {
        File sdir = SD_MMC.open(sfge_scn::path("speech"));
        if (sdir && sdir.isDirectory()) {
            S.println("[SPEECH] files on card:");
            File sf;
            int n = 0;
            while ((sf = sdir.openNextFile())) {
                if (!sf.isDirectory()) { S.printf("[SPEECH]   %s\n", sf.name()); n++; }
                sf.close();
            }
            if (!n) S.println("[SPEECH]   (none — put boot/greet/idle1../turnin/denied .mp3|.wav here)");
            sdir.close();
        } else {
            S.printf("[SPEECH] no %s folder on the card\n", sfge_scn::path("speech"));
        }
    }

    // Audio (ES8311 codec + WAV/MP3 player, or the remote DFPlayer node)
    codecOk = es8311Init();
    if (codecOk) audioStart();
    else S.println("[AUDIO] onboard codec disabled");
    if (audioNode[0]) S.printf("[AUDIO] remote mode -> %s\n", audioNode);
    // Boot sound — plays once the loop services the speech queue (onboard
    // file or the node's mapped "boot" track; missing = logs and skips)
    if (codecOk || audioNode[0]) speechQueueFile("boot");

    // WiFi AP + mesh
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    WiFi.softAP(cfg.ssid, nullptr, MESH_CHANNEL);
    S.printf("[WIFI] AP '%s' ch=%d\n", cfg.ssid, MESH_CHANNEL);

    server.on("/api/info", HTTP_GET, handleInfo);
    server.on("/api/interact", HTTP_POST, [](AsyncWebServerRequest *r) {}, nullptr, handleInteract);
    server.begin();

    sfge::meshInit(cfg.id, sfge::ROLE_PANEL, onMeshMsg);
    delay(200);
    speechMapBroadcast();   // teach the audio node our clip names
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

    // Occasional idle grumbling — picks randomly from the idle pool.
    // Onboard mode: speech/idle1..idle9.(wav|mp3) files, plain idle fallback.
    // Remote mode: any speech_tracks entries whose name starts with "idle".
    // Cadence: behavior.idle_min_s .. idle_max_s (default 2-5 minutes).
    static unsigned long nextIdle = 0;
    if (!nextIdle) nextIdle = millis() + (unsigned long)idleMinS * 1000UL;
    if ((codecOk || audioNode[0]) && !audio.isRunning() && millis() > nextIdle) {
        nextIdle = millis() + (unsigned long)idleMinS * 1000UL +
                   esp_random() % ((unsigned long)(idleMaxS - idleMinS) * 1000UL + 1);
        if (audioNode[0]) {
            const char *pool[16];
            int poolN = 0;
            for (int i = 0; i < speechTrackCount; i++)
                if (strncasecmp(speechTracks[i].name, "idle", 4) == 0)
                    pool[poolN++] = speechTracks[i].name;
            if (poolN > 0) speechQueueFile(pool[esp_random() % poolN]);
        } else if (sdOk) {
            int pool[9], poolN = 0;
            for (int i = 1; i <= 9; i++) {
                char p1[64], p2[64];
                snprintf(p1, sizeof(p1), "%s/speech/idle%d.wav", sfge_scn::root, i);
                snprintf(p2, sizeof(p2), "%s/speech/idle%d.mp3", sfge_scn::root, i);
                if (SD_MMC.exists(p1) || SD_MMC.exists(p2)) pool[poolN++] = i;
            }
            if (poolN > 0) {
                char base[16];
                snprintf(base, sizeof(base), "idle%d", pool[esp_random() % poolN]);
                speechQueueFile(base);
            } else {
                // Plain idle.* fallback — skip quietly when the card has none
                char pi1[64], pi2[64];
                snprintf(pi1, sizeof(pi1), "%s/speech/idle.wav", sfge_scn::root);
                snprintf(pi2, sizeof(pi2), "%s/speech/idle.mp3", sfge_scn::root);
                if (SD_MMC.exists(pi1) || SD_MMC.exists(pi2)) speechQueueFile("idle");
            }
        }
    }

    // Heartbeat
    static unsigned long lastBeat = 0;
    if (millis() - lastBeat > 5000) {
        lastBeat = millis();
        sfge::sendPing(millis() / 1000);
        sfge::sendPropStatus(millis() / 1000, 0, false, "");
    }

    // Re-teach the audio node its track map every 30 s (covers node reboots)
    static unsigned long lastMapBcast = 0;
    if (audioNode[0] && millis() - lastMapBcast > 30000) {
        lastMapBcast = millis();
        speechMapBroadcast();
    }

    delay(2);
}
