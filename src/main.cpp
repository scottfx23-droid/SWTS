/*
 * SWTS Datapad — Czerka Arms DP-47
 * LVGL + LovyanGFX on ESP32-S3
 */
#include <LovyanGFX.hpp>
#include <lvgl.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_PN532.h>
#include <soc/rtc_cntl_reg.h>   // RTC_CNTL_BROWN_OUT_REG — disable brownout detector
#include <esp_random.h>         // esp_random() — hardware RNG for the Simon sequence

#include "icons.h"
#include "missions.h"
#include "swts_provision.h"
#include "swts_test_configs.h"   // boot-time test provisioning (see header to disable)
#include "swts_config.h"
#include "swts_nfc_map.h"
#include "swts_missions.h"
#include "swts_comms.h"
#include "swts_mesh.h"
#define S Serial

// ═══════════════════════════════════════
//  PIN ASSIGNMENTS
// ═══════════════════════════════════════
// Display SPI:  SCK=40, MOSI=41, MISO=39, CS=1, DC=42, RST=2
// Touch I2C0:   SDA=47, SCL=38, RST=48, INT=21  (FT6336 @ 0x38)
// NFC   I2C1:   SDA=14, SCL=13                   (PN532)
// SD    HSPI:   CS=15, MOSI=16, CLK=17, MISO=18
// Buzzer:       GPIO 4
// Buttons:      Blue LED=5  Btn=6   White LED=7  Btn=8   Red LED=9  Btn=10
//               (originally specced 37/36 for White, but those are octal-PSRAM pins
//                on the WROOM-1 N16R8 — moved to 7/8.)

#define TOUCH_SDA  47
#define TOUCH_SCL  38
#define TOUCH_RST  48
#define TOUCH_INT  21

#define NFC_SDA    14
#define NFC_SCL    13

#define SD_CS      15
#define SD_MOSI    16
#define SD_CLK     17
#define SD_MISO    18

#define BUZZER_PIN 4

#define BTN_BLUE_LED   5
#define BTN_BLUE_PIN   6
#define BTN_WHITE_LED  7
#define BTN_WHITE_PIN  8
#define BTN_RED_LED    9
#define BTN_RED_PIN    10

// ═══════════════════════════════════════
//  BUZZER — passive piezo on GPIO 4
// ═══════════════════════════════════════
void buzzerTone(int freq, int ms) {
    tone(BUZZER_PIN, freq, ms);
}

void buzzerScanOk() {
    // Two rising tones — Star Wars datapad chirp
    buzzerTone(880, 60);
    delay(70);
    buzzerTone(1320, 80);
}

void buzzerScanFail() {
    // Low descending buzz
    buzzerTone(300, 150);
    delay(160);
    buzzerTone(200, 200);
}

void buzzerSuccess() {
    // Ascending three-note chime
    buzzerTone(523, 80);
    delay(90);
    buzzerTone(659, 80);
    delay(90);
    buzzerTone(784, 120);
}

void buzzerFail() {
    // Flat rejection tone
    buzzerTone(200, 300);
}

void buzzerClick() {
    buzzerTone(1000, 20);
}

// ═══════════════════════════════════════════════
//  HARDWARE BUTTONS — three colored arcade buttons with LEDs
//  Wiring: LED+ to GPIO (LED- to GND, drive HIGH = on)
//          Button+ to GPIO (Button- to GND, INPUT_PULLUP, LOW = pressed)
//  Used for the Simon Says slicing minigame and similar interactions.
// ═══════════════════════════════════════════════
enum BtnColor : uint8_t { BTN_BLUE = 0, BTN_WHITE = 1, BTN_RED = 2 };

struct BtnDef { uint8_t led; uint8_t pin; const char *name; };
static const BtnDef BUTTONS[3] = {
    { BTN_BLUE_LED,  BTN_BLUE_PIN,  "BLUE"  },
    { BTN_WHITE_LED, BTN_WHITE_PIN, "WHITE" },
    { BTN_RED_LED,   BTN_RED_PIN,   "RED"   },
};

// Edge-detected state — `pressed` is set for one poll tick on each press,
// then a consumer must clear it (or the poller resets next tick).
struct BtnState {
    bool down;           // current debounced state
    bool pressed;        // edge: just transitioned to down this poll
    unsigned long lastChange;
    unsigned long ledUntil;   // ms timestamp to turn LED off (0 = solid/off)
    bool ledHeld;             // true = LED stays on until explicitly cleared
};
static BtnState btnState[3] = {};

// Callback hook: register one of these to receive press events. Set to nullptr to disable.
typedef void (*BtnHandler)(BtnColor c);
BtnHandler btnHandler = nullptr;

void ledOn(BtnColor c)              { digitalWrite(BUTTONS[c].led, HIGH); btnState[c].ledHeld = true;  btnState[c].ledUntil = 0; }
void ledOff(BtnColor c)             { digitalWrite(BUTTONS[c].led, LOW);  btnState[c].ledHeld = false; btnState[c].ledUntil = 0; }
void ledPulse(BtnColor c, uint16_t ms) {
    digitalWrite(BUTTONS[c].led, HIGH);
    btnState[c].ledHeld = false;
    btnState[c].ledUntil = millis() + ms;
}
void ledAllOff() { for (int i = 0; i < 3; i++) ledOff((BtnColor)i); }

void initButtons() {
    for (int i = 0; i < 3; i++) {
        pinMode(BUTTONS[i].led, OUTPUT);
        digitalWrite(BUTTONS[i].led, LOW);
        pinMode(BUTTONS[i].pin, INPUT_PULLUP);
        btnState[i] = {};
    }
    // Quick boot self-test: cycle through each LED for 100 ms
    for (int i = 0; i < 3; i++) { digitalWrite(BUTTONS[i].led, HIGH); delay(100); digitalWrite(BUTTONS[i].led, LOW); }
}

// Call once per loop. Debounce 25 ms; emits one edge per press.
void pollButtons() {
    unsigned long now = millis();
    for (int i = 0; i < 3; i++) {
        bool raw = (digitalRead(BUTTONS[i].pin) == LOW);   // pressed = LOW (pullup)
        if (raw != btnState[i].down && now - btnState[i].lastChange > 25) {
            btnState[i].lastChange = now;
            btnState[i].down = raw;
            if (raw) {
                btnState[i].pressed = true;
                Serial.printf("[BTN] %s pressed\n", BUTTONS[i].name);
                if (btnHandler) btnHandler((BtnColor)i);
                else ledPulse((BtnColor)i, 150);   // visible feedback even before a handler is wired
            }
        }
        // Auto-extinguish pulse LEDs
        if (!btnState[i].ledHeld && btnState[i].ledUntil && now >= btnState[i].ledUntil) {
            btnState[i].ledUntil = 0;
            digitalWrite(BUTTONS[i].led, LOW);
        }
    }
}

// Consumer helpers — read+clear a single edge
bool buttonConsume(BtnColor c) {
    if (!btnState[c].pressed) return false;
    btnState[c].pressed = false;
    return true;
}
bool buttonAnyConsume(BtnColor *out) {
    for (int i = 0; i < 3; i++) {
        if (btnState[i].pressed) { btnState[i].pressed = false; if (out) *out = (BtnColor)i; return true; }
    }
    return false;
}

// Default press feedback: each button beeps its own tone. Frequencies span
// ~5x because passive piezos have a narrow resonant peak — closer notes
// (523/698/880 Hz) all sound the same. Simon uses these same tones as its
// pattern cues, so the game swaps in a no-op handler while it runs.
static void defaultBtnTone(BtnColor c) {
    switch (c) {
        case BTN_BLUE:  buzzerTone(600, 120);  break;   // low
        case BTN_WHITE: buzzerTone(1500, 120); break;   // mid
        case BTN_RED:   buzzerTone(3000, 120); break;   // high
    }
}

bool sdOk = false;
char sdDbgMsg[128] = "SD: not checked";

// ═══════════════════════════════════════
//  DISPLAY — SPI2_HOST
// ═══════════════════════════════════════
class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7796 _panel; lgfx::Bus_SPI _bus;
public:
    LGFX() {
        { auto c = _bus.config(); c.spi_host=SPI2_HOST; c.spi_mode=0;
          c.freq_write=40000000; c.freq_read=16000000;
          c.pin_sclk=40; c.pin_mosi=41; c.pin_miso=39; c.pin_dc=42;
          _bus.config(c); _panel.setBus(&_bus); }
        { auto c = _panel.config(); c.pin_cs=1; c.pin_rst=2; c.pin_busy=-1;
          c.panel_width=320; c.panel_height=480; c.readable=true;
          c.invert=false; c.rgb_order=false; c.bus_shared=false;
          _panel.config(c); }
        setPanel(&_panel);
    }
};
LGFX tft;

#define W 320
#define H 480

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1, *buf2;

void lvgl_flush(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px) {
    tft.startWrite();
    tft.setAddrWindow(a->x1, a->y1, a->x2 - a->x1 + 1, a->y2 - a->y1 + 1);
    tft.writePixels((uint16_t *)px, (a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1));
    tft.endWrite();
    lv_disp_flush_ready(drv);
}

// ═══════════════════════════════════════
//  TOUCH
// ═══════════════════════════════════════
#define T_ADDR 0x38
bool touchOk = false;

bool ft_read(int16_t &x, int16_t &y) {
    Wire.beginTransmission(T_ADDR);
    Wire.write(0x02);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((uint8_t)T_ADDR, (uint8_t)5) != 5) return false;
    uint8_t n = Wire.read() & 0x0F;
    uint8_t xH = Wire.read(), xL = Wire.read(), yH = Wire.read(), yL = Wire.read();
    if (n == 0 || n > 2) return false;
    x = 319 - (((xH & 0x0F) << 8) | xL);
    y = 479 - (((yH & 0x0F) << 8) | yL);
    return true;
}

void lvgl_touch(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    int16_t x, y;
    if (ft_read(x, y)) { data->state = LV_INDEV_STATE_PRESSED; data->point.x = x; data->point.y = y; }
    else data->state = LV_INDEV_STATE_RELEASED;
}

// ═══════════════════════════════════════
//  NFC
// ═══════════════════════════════════════
TwoWire I2C_NFC = TwoWire(1);
Adafruit_PN532 nfc(-1, -1, &I2C_NFC);
bool nfcOk = false;

// ═══════════════════════════════════════
//  COLORS — amber monochrome on black
// ═══════════════════════════════════════
#define C_BG         lv_color_hex(0x000000)
#define C_PNL        lv_color_hex(0x0C0A04)
#define C_PNL2       lv_color_hex(0x181208)
#define C_PNL_HI     lv_color_hex(0x241C0C)
#define C_FRM        lv_color_hex(0x2A2010)
#define C_FRM_HI     lv_color_hex(0x483818)
#define C_AMB        lv_color_hex(0xCC8800)
#define C_AMB_DIM    lv_color_hex(0x4A3000)
#define C_AMB_BRT    lv_color_hex(0xFFAA00)
#define C_GRN        lv_color_hex(0xCC8800)
#define C_RED        lv_color_hex(0xCC8800)
#define C_TXT        lv_color_hex(0xCC8800)
#define C_DIM        lv_color_hex(0x664400)
#define C_MUT        lv_color_hex(0x332200)
#define C_WHITE      lv_color_hex(0xFFAA00)

// ═══════════════════════════════════════
//  STYLES
// ═══════════════════════════════════════
static lv_style_t s_scr, s_pnl, s_btn, s_btn_pr, s_hdr, s_ftr, s_badge;

// Helper: make a thin colored line (horizontal rule)
lv_obj_t* hline(lv_obj_t *par, int y, lv_color_t c, int h = 2) {
    lv_obj_t *o = lv_obj_create(par);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, W, h);
    lv_obj_set_pos(o, 0, y);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

// Helper: accent bar inside a parent
lv_obj_t* accentBar(lv_obj_t *par, lv_color_t c, int w = 4) {
    lv_obj_t *o = lv_obj_create(par);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, lv_pct(80));
    lv_obj_align(o, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

void initStyles() {
    // Screen bg — pure black, opaque
    lv_style_init(&s_scr);
    lv_style_set_bg_color(&s_scr, C_BG);
    lv_style_set_bg_opa(&s_scr, LV_OPA_COVER);
    lv_style_set_text_color(&s_scr, C_TXT);

    // Panel — very dark, opaque (no transparency artifacts)
    lv_style_init(&s_pnl);
    lv_style_set_bg_color(&s_pnl, C_PNL);
    lv_style_set_bg_opa(&s_pnl, LV_OPA_COVER);
    lv_style_set_border_color(&s_pnl, C_FRM);
    lv_style_set_border_width(&s_pnl, 1);
    lv_style_set_radius(&s_pnl, 0);
    lv_style_set_pad_all(&s_pnl, 10);
    lv_style_set_shadow_width(&s_pnl, 0);

    // Button — black bg, amber border
    lv_style_init(&s_btn);
    lv_style_set_bg_color(&s_btn, C_BG);
    lv_style_set_bg_opa(&s_btn, LV_OPA_COVER);
    lv_style_set_border_color(&s_btn, C_FRM);
    lv_style_set_border_width(&s_btn, 1);
    lv_style_set_radius(&s_btn, 0);
    lv_style_set_pad_all(&s_btn, 0);
    lv_style_set_shadow_width(&s_btn, 0);
    lv_style_set_text_color(&s_btn, C_TXT);

    // Button pressed — slightly lighter, bright border
    lv_style_init(&s_btn_pr);
    lv_style_set_bg_color(&s_btn_pr, C_PNL2);
    lv_style_set_border_color(&s_btn_pr, C_AMB_BRT);

    // Header — black, opaque
    lv_style_init(&s_hdr);
    lv_style_set_bg_color(&s_hdr, C_BG);
    lv_style_set_bg_opa(&s_hdr, LV_OPA_COVER);
    lv_style_set_border_width(&s_hdr, 0);
    lv_style_set_radius(&s_hdr, 0);
    lv_style_set_pad_left(&s_hdr, 12);
    lv_style_set_pad_right(&s_hdr, 12);
    lv_style_set_pad_ver(&s_hdr, 0);

    // Footer — black, opaque
    lv_style_init(&s_ftr);
    lv_style_set_bg_color(&s_ftr, C_BG);
    lv_style_set_bg_opa(&s_ftr, LV_OPA_COVER);
    lv_style_set_border_width(&s_ftr, 0);
    lv_style_set_radius(&s_ftr, 0);
    lv_style_set_pad_left(&s_ftr, 12);
    lv_style_set_pad_right(&s_ftr, 12);

    // Badge — amber on black
    lv_style_init(&s_badge);
    lv_style_set_bg_color(&s_badge, C_AMB);
    lv_style_set_bg_opa(&s_badge, LV_OPA_COVER);
    lv_style_set_text_color(&s_badge, C_BG);
    lv_style_set_radius(&s_badge, 2);
    lv_style_set_pad_hor(&s_badge, 6);
    lv_style_set_pad_ver(&s_badge, 2);
    lv_style_set_text_font(&s_badge, &lv_font_montserrat_12);
}

// ═══════════════════════════════════════
//  GAME STATE
// ═══════════════════════════════════════
char callsign[16] = "OPERATIVE";   // overridden by /SWTS/player.json

// Derive a unique default callsign from the device's MAC. Format: "DATAPAD-XXXX"
// where XXXX is the last 4 hex chars of the WiFi MAC. Lets GM distinguish
// multiple un-named datapads on the same mesh.
#include <esp_mac.h>
void defaultCallsignFromMac(char *out, size_t outSz) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, outSz, "DATAPAD-%02X%02X", mac[4], mac[5]);
}

// Treat the default-on-first-boot names as "not yet assigned a real player".
// GM puts these in the UNASSIGNED tab so an operator can name them on the fly.
bool isUnassignedCallsign(const char *cs) {
    if (!cs || !cs[0]) return true;
    if (strcmp(cs, "OPERATIVE") == 0) return true;
    if (strncmp(cs, "DATAPAD-", 8) == 0) return true;
    return false;
}
int score = 0;          // credits or reputation — starts at 0
char scoreSuffix[4] = "CR";  // "CR" or "RP" — loaded from config
char planetName[24] = "Unknown";
lv_obj_t *homeScoreLbl = NULL;
lv_obj_t *homeMissionBtn = NULL,  *homeMissionBadge = NULL;
lv_obj_t *homeBountyBtn  = NULL,  *homeBountyBadge  = NULL;
lv_obj_t *homeCommBtn    = NULL,  *homeCommBadge    = NULL;
void refreshScoreLabel() {
    if (!homeScoreLbl) return;
    char b[24];
    snprintf(b, sizeof(b), "%d %s", score, scoreSuffix);
    lv_label_set_text(homeScoreLbl, b);
}
void refreshHomeBadges();   // forward decl, defined after styles exist
int activeBountyCount();    // forward decl, defined alongside the bounty pool
int xp = 0;
int activeMissions = 0, inventoryItems = 0, unreadComms = 0, totalScans = 0;

// ── Player state forward decls (full impl lives near the SD loaders) ──
extern volatile bool playerDirty;
extern unsigned long playerLastSave;
void playerMarkCommRead(const char *id);
void playerMarkBountyWon(const char *id);
bool playerHasReadComm(const char *id);
bool playerHasWonBounty(const char *id);

// ── Mission catalog (loaded from /SWTS/missions.json on boot) ──
MissionDef ALL_MISSIONS[MAX_MISSIONS];
int NUM_MISSIONS = 0;
NfcTrigger NFC_TRIGGERS[MAX_TRIGGERS];
int NUM_TRIGGERS = 0;

// ── Mission runtime state ──
ActiveMission msnSlots[MAX_ACTIVE];
bool msnUnlocked[MAX_MISSIONS];

void initMissions() {
    for (int i = 0; i < MAX_ACTIVE; i++) msnSlots[i].def_idx = -1;
    for (int i = 0; i < MAX_MISSIONS; i++) msnUnlocked[i] = false;
}

// Find mission def index by id string. Returns -1 if not found.
int findMissionByIdStr(const char *id) {
    for (int i = 0; i < NUM_MISSIONS; i++)
        if (strcmp(ALL_MISSIONS[i].id, id) == 0) return i;
    return -1;
}

int findActive(uint8_t defIdx) {
    for (int i = 0; i < MAX_ACTIVE; i++)
        if (msnSlots[i].def_idx == defIdx && !msnSlots[i].complete) return i;
    return -1;
}

void triggerCommsPrefix(const char *prefix, const char *value);  // forward decl
void showObjectiveToast(const char *title, const char *body);    // forward decl
bool debriefPending = false;   // set when the endgame mission completes; loop shows debrief

bool startMission(uint8_t defIdx) {
    if (defIdx >= NUM_MISSIONS) return false;
    if (!msnUnlocked[defIdx]) return false;
    if (findActive(defIdx) >= 0) return false; // Already active
    for (int i = 0; i < MAX_ACTIVE; i++) {
        if (msnSlots[i].def_idx == -1) {
            msnSlots[i].def_idx = defIdx;
            msnSlots[i].current_step = 0;
            msnSlots[i].complete = false;
            activeMissions++;
            playerDirty = true;
            playerLastSave = 0;
            refreshHomeBadges();
            S.printf("Mission started: %s\n", ALL_MISSIONS[defIdx].title);
            triggerCommsPrefix("mission_start", ALL_MISSIONS[defIdx].id);
            return true;
        }
    }
    return false; // No slots
}

// Start every unlocked, never-touched mission. Called once after boot (missions +
// player state + comms pool all loaded), so day-one missions appear automatically
// and their briefing comms fire. Completed/active slots block a restart.
void autoStartMissions() {
    for (int i = 0; i < NUM_MISSIONS; i++) {
        if (!msnUnlocked[i] || !ALL_MISSIONS[i].starts_unlocked) continue;
        bool seen = false;
        for (int s2 = 0; s2 < MAX_ACTIVE; s2++)
            if (msnSlots[s2].def_idx == i) { seen = true; break; }
        if (!seen) startMission(i);
    }
}

bool advanceStep(uint8_t defIdx, uint8_t stepIdx) {
    int slot = findActive(defIdx);
    if (slot < 0) return false;
    if (msnSlots[slot].current_step != stepIdx) return false; // Wrong step
    const MissionDef &m = ALL_MISSIONS[defIdx];
    xp += m.steps[stepIdx].xp_reward;
    msnSlots[slot].current_step++;
    playerDirty = true;
    playerLastSave = 0;
    S.printf("Step complete: %s step %d\n", m.title, stepIdx);
    // Check if mission complete
    if (msnSlots[slot].current_step >= m.num_steps) {
        msnSlots[slot].complete = true;
        activeMissions--;
        score += m.reward_credits;
        xp += m.reward_xp;
        refreshScoreLabel();
        refreshHomeBadges();
        playerDirty = true;
        // Unlock (and immediately start) the next mission in the chain
        if (m.unlocks_id[0]) {
            for (int i = 0; i < NUM_MISSIONS; i++) {
                if (strcmp(ALL_MISSIONS[i].id, m.unlocks_id) == 0) {
                    msnUnlocked[i] = true;
                    startMission(i);
                    showObjectiveToast("NEW MISSION", ALL_MISSIONS[i].title);
                    break;
                }
            }
        }
        S.printf("Mission COMPLETE: %s (+%d cr, +%d xp)\n", m.title, m.reward_credits, m.reward_xp);
        if (m.is_endgame) debriefPending = true;   // loop() shows the debrief screen
        return true;
    }
    return true;
}

// ── Game events ──
// Event ids arrive from the mesh (MSG_EVENT, possibly on the WiFi task) and from
// prop HTTP responses ("game_event" field). They are queued here and drained in
// loop() — the LVGL-safe context — where they fire comms and advance missions.
#define MAX_PENDING_EVENTS 6
char pendingEvents[MAX_PENDING_EVENTS][40];
volatile int pendingEventCount = 0;
portMUX_TYPE evMux = portMUX_INITIALIZER_UNLOCKED;

void queueGameEvent(const char *id) {
    portENTER_CRITICAL(&evMux);
    if (pendingEventCount < MAX_PENDING_EVENTS)
        strlcpy(pendingEvents[pendingEventCount++], id, sizeof(pendingEvents[0]));
    portEXIT_CRITICAL(&evMux);
}

// Advance any active mission whose current step waits on this event id
void advanceEventSteps(const char *eventId) {
    for (int s2 = 0; s2 < MAX_ACTIVE; s2++) {
        if (msnSlots[s2].def_idx < 0 || msnSlots[s2].complete) continue;
        uint8_t di = msnSlots[s2].def_idx;
        uint8_t st = msnSlots[s2].current_step;
        const MissionDef &m = ALL_MISSIONS[di];
        if (st >= m.num_steps) continue;
        if (m.steps[st].obj_type != OBJ_EVENT) continue;
        if (strcmp(m.steps[st].target, eventId) != 0) continue;

        bool wasLast = (st == m.num_steps - 1);
        if (advanceStep(di, st)) {
            buzzerScanOk();
            if (wasLast)
                showObjectiveToast("MISSION COMPLETE", m.title);
            else
                showObjectiveToast("OBJECTIVE COMPLETE", m.steps[st].title);
        }
    }
}

void processGameEvents() {
    while (true) {
        char ev[40];
        portENTER_CRITICAL(&evMux);
        if (pendingEventCount == 0) { portEXIT_CRITICAL(&evMux); break; }
        strlcpy(ev, pendingEvents[--pendingEventCount], sizeof(ev));
        portEXIT_CRITICAL(&evMux);
        S.printf("[GAME] Event: %s\n", ev);
        triggerCommsPrefix("event", ev);
        advanceEventSteps(ev);
    }
}

// Check NFC scan against trigger table
enum ScanResult { SCAN_NONE, SCAN_MISSION_START, SCAN_STEP_DONE, SCAN_MISSION_COMPLETE };
struct ScanOutcome {
    ScanResult result;
    uint8_t mission_idx;
    uint8_t step_idx;
};

ScanOutcome checkMissionTrigger(const char *token) {
    for (int i = 0; i < NUM_TRIGGERS; i++) {
        if (strcmp(token, NFC_TRIGGERS[i].match_token) == 0) {
            const NfcTrigger &t = NFC_TRIGGERS[i];
            if (t.action == TRIG_START) {
                if (startMission(t.mission_idx))
                    return {SCAN_MISSION_START, (uint8_t)t.mission_idx, 0};
            } else { // TRIG_STEP
                int slot = findActive(t.mission_idx);
                if (slot >= 0 && msnSlots[slot].current_step == t.step_idx) {
                    bool wasLast = (msnSlots[slot].current_step == ALL_MISSIONS[t.mission_idx].num_steps - 1);
                    advanceStep(t.mission_idx, t.step_idx);
                    return {wasLast ? SCAN_MISSION_COMPLETE : SCAN_STEP_DONE, (uint8_t)t.mission_idx, t.step_idx};
                }
            }
        }
    }
    return {SCAN_NONE, 0, 0};
}

// ═══════════════════════════════════════
//  SCREEN OBJECTS
// ═══════════════════════════════════════
lv_obj_t *scrHome = NULL, *scrDatacard = NULL, *scrNearby = NULL;
lv_obj_t *scrMissions = NULL, *scrBounty = NULL, *scrCargo = NULL, *scrComms = NULL;
lv_obj_t *scrProp = NULL;
lv_obj_t *scrSlice = NULL;  // Slice minigame screen
lv_obj_t *scrSimon = NULL;  // Simon Says (pattern lock) minigame screen
lv_obj_t *scrPurge = NULL;  // Core Purge (memory defrag) minigame screen
lv_obj_t *dcSpinner = NULL, *dcPrompt = NULL, *dcResult = NULL;
lv_obj_t *cgSpinner = NULL, *cgPrompt = NULL, *cgResult = NULL;
bool cgScanning = true;

// True when an NDEF text payload looks like a cargo crate id (e.g. CARGO_01).
inline bool isCargoTag(const char *text) {
    if (!text) return false;
    return (strncasecmp(text, "CARGO_", 6) == 0) || (strncasecmp(text, "CARGO-", 6) == 0);
}
lv_obj_t *nbList = NULL, *nbStatus = NULL, *nbSpinner = NULL;
bool dcScanning = true;

// ── Discovered props (stored for tap-to-connect) ──
#define MAX_PROPS 8
struct FoundProp {
    char ssid[33];
    char name[24];
    char faction[12];
    int rssi;
};
FoundProp foundProps[MAX_PROPS];
int foundCount = 0;

// ── Prop connection state ──
bool propConnected = false;
char propName[32] = "";
char propFaction[12] = "";
char propId[16] = "";
bool propHasMinigame = false;
char propMinigameType[16] = "";

void buildHomeScreen();
void buildDatacardScreen();
void buildNearbyScreen();

// ═══════════════════════════════════════
//  COMMON: back button builder
// ═══════════════════════════════════════
lv_obj_t* makeBackBtn(lv_obj_t *par, lv_event_cb_t cb) {
    lv_obj_t *b = lv_btn_create(par);
    lv_obj_set_size(b, 72, 30);
    lv_obj_align(b, LV_ALIGN_LEFT_MID, -6, 0);
    lv_obj_set_style_bg_color(b, C_BG, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, C_PNL2, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, C_AMB_DIM, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, "< BACK");
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, C_AMB, 0);
    lv_obj_center(l);
    return b;
}

// ═══════════════════════════════════════
//  COMMON: sub-screen header
// ═══════════════════════════════════════
void makeSubHeader(lv_obj_t *scr, const char *title, lv_event_cb_t backCb) {
    // Header background (non-clickable so children get events)
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, W, 46);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_add_style(hdr, &s_hdr, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Title label on header
    lv_obj_t *t = lv_label_create(hdr);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t, C_AMB_BRT, 0);
    lv_obj_align(t, LV_ALIGN_RIGHT_MID, 0, 0);

    // Back button directly on screen (not inside header) so it's always clickable
    lv_obj_t *b = lv_btn_create(scr);
    lv_obj_set_size(b, 80, 36);
    lv_obj_set_pos(b, 6, 5);
    lv_obj_set_style_bg_color(b, C_BG, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, C_PNL2, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, C_AMB_DIM, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_add_event_cb(b, backCb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(b);
    lv_label_set_text(bl, "< BACK");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bl, C_AMB, 0);
    lv_obj_center(bl);

    hline(scr, 46, C_AMB_DIM, 2);
    hline(scr, 48, C_FRM, 1);
}

// ═══════════════════════════════════════
//  HOME SCREEN
// ═══════════════════════════════════════
static void ev_dc(lv_event_t *e)       { lv_scr_load_anim(scrDatacard, LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false); }
static void ev_nb(lv_event_t *e)       { lv_scr_load_anim(scrNearby, LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false); }
void refreshMissionList();   // rebuilds the list from live state
static void ev_missions(lv_event_t *e) { refreshMissionList(); lv_scr_load_anim(scrMissions, LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false); }
static void ev_bounty(lv_event_t *e)   { lv_scr_load_anim(scrBounty, LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false); }
static void ev_cargo(lv_event_t *e)    { lv_scr_load_anim(scrCargo, LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false); }
static void ev_comms(lv_event_t *e)    { lv_scr_load_anim(scrComms, LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false); }

struct GBtn { const char *lbl, *sub; const lv_img_dsc_t *icon; lv_color_t col; int *cnt; lv_event_cb_t cb; };
GBtn gbtns[6];

void buildHomeScreen() {
    gbtns[0] = {"SLICE",      "HACK ACCESS",  &ico_slice,    C_AMB, NULL,            ev_nb};
    gbtns[1] = {"MISSIONS",   "BRIEFING",     &ico_missions, C_AMB, &activeMissions, ev_missions};
    gbtns[2] = {"DATACARD",   "INSERT CARD",  &ico_datacard, C_AMB, NULL,            ev_dc};
    gbtns[3] = {"BOUNTY",     "WANTED",       &ico_bounty,   C_AMB, NULL,            ev_bounty};
    gbtns[4] = {"CARGO INTEL","LOCATE CRATE", &ico_cargo,    C_AMB, &inventoryItems, ev_cargo};
    gbtns[5] = {"COMMS",      "INBOX",        &ico_comms,    C_AMB, &unreadComms,    ev_comms};

    scrHome = lv_obj_create(NULL);
    lv_obj_add_style(scrHome, &s_scr, 0);

    // ── Header ──
    lv_obj_t *hdr = lv_obj_create(scrHome);
    lv_obj_set_size(hdr, W, 50);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_add_style(hdr, &s_hdr, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    // Planet name (left side of header)
    lv_obj_t *cs = lv_label_create(hdr);
    lv_label_set_text(cs, planetName);
    lv_obj_set_style_text_font(cs, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(cs, C_AMB_BRT, 0);
    lv_obj_set_style_text_letter_space(cs, 2, 0);
    lv_obj_align(cs, LV_ALIGN_LEFT_MID, 0, 0);

    // Score (credits or reputation) — exposed so we can refresh it
    extern lv_obj_t *homeScoreLbl;
    char cbuf[24];
    snprintf(cbuf, sizeof(cbuf), "%d %s", score, scoreSuffix);
    homeScoreLbl = lv_label_create(hdr);
    lv_label_set_text(homeScoreLbl, cbuf);
    lv_obj_set_style_text_font(homeScoreLbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(homeScoreLbl, C_AMB, 0);
    lv_obj_align(homeScoreLbl, LV_ALIGN_RIGHT_MID, 0, 0);

    hline(scrHome, 50, C_AMB, 2);
    hline(scrHome, 52, C_FRM, 1);

    // ── Grid ──
    int x0 = 6, y0 = 60, gw = (W - x0 * 2 - 6) / 2, gap = 6;
    int rowH[] = {124, 124, 104};

    for (int i = 0; i < 6; i++) {
        int c = i % 2, r = i / 2;
        int bx = x0 + c * (gw + gap);
        int by = y0;
        for (int rr = 0; rr < r; rr++) by += rowH[rr] + gap;
        int bh = rowH[r];

        lv_obj_t *btn = lv_obj_create(scrHome);
        lv_obj_set_size(btn, gw, bh);
        lv_obj_set_pos(btn, bx, by);
        lv_obj_add_style(btn, &s_btn, 0);
        lv_obj_add_style(btn, &s_btn_pr, LV_STATE_PRESSED);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, gbtns[i].cb, LV_EVENT_CLICKED, NULL);

        if (i == 1) homeMissionBtn = btn;   // remember tiles for live badges
        if (i == 3) homeBountyBtn  = btn;
        if (i == 5) homeCommBtn    = btn;

        // Accent bar
        accentBar(btn, gbtns[i].col, 5);

        // Icon image (alpha-8bit, tinted amber)
        lv_obj_t *ico = lv_img_create(btn);
        lv_img_set_src(ico, gbtns[i].icon);
        lv_obj_set_style_img_recolor(ico, C_AMB_BRT, 0);
        lv_obj_set_style_img_recolor_opa(ico, LV_OPA_COVER, 0);
        lv_obj_set_pos(ico, 18, 14);

        // Label (bold, centered vertically)
        lv_obj_t *l = lv_label_create(btn);
        lv_label_set_text(l, gbtns[i].lbl);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(l, C_WHITE, 0);
        lv_obj_set_style_text_letter_space(l, 1, 0);
        lv_obj_set_pos(l, 18, bh / 2 + 2);

        // Sublabel
        lv_obj_t *sl = lv_label_create(btn);
        lv_label_set_text(sl, gbtns[i].sub);
        lv_obj_set_style_text_font(sl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(sl, C_DIM, 0);
        lv_obj_set_pos(sl, 18, bh / 2 + 22);

        // Badges for MISSIONS/BOUNTY/COMMS are rendered live in refreshHomeBadges()
        bool liveBadge = (i == 1 || i == 3 || i == 5);
        if (!liveBadge && gbtns[i].cnt && *gbtns[i].cnt > 0) {
            char bv[4]; snprintf(bv, sizeof(bv), "%d", *gbtns[i].cnt);
            lv_obj_t *bg = lv_label_create(btn);
            lv_label_set_text(bg, bv);
            lv_obj_add_style(bg, &s_badge, 0);
            lv_obj_set_style_bg_color(bg, gbtns[i].col, 0);
            lv_obj_set_style_text_color(bg, C_BG, 0);
            lv_obj_align(bg, LV_ALIGN_TOP_RIGHT, -8, 8);
        }
    }
    refreshHomeBadges();

    // ── Footer ──
    hline(scrHome, H - 30, C_FRM, 1);
    lv_obj_t *ftr = lv_obj_create(scrHome);
    lv_obj_set_size(ftr, W, 29);
    lv_obj_set_pos(ftr, 0, H - 29);
    lv_obj_add_style(ftr, &s_ftr, 0);
    lv_obj_clear_flag(ftr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *sys = lv_label_create(ftr);
    lv_label_set_text(sys, "SYSTEMS ACTIVE");
    lv_obj_set_style_text_font(sys, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sys, C_AMB_DIM, 0);
    lv_obj_align(sys, LV_ALIGN_LEFT_MID, 0, 0);

    // SD status (small checkmark or X in footer)
    lv_obj_t *sdIco = lv_label_create(ftr);
    lv_label_set_text(sdIco, sdOk ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(sdIco, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sdIco, sdOk ? C_AMB_DIM : lv_color_hex(0x882200), 0);
    lv_obj_align(sdIco, LV_ALIGN_RIGHT_MID, 0, 0);
}

// ═══════════════════════════════════════
//  DATACARD SCREEN
// ═══════════════════════════════════════
static void ev_dc_back(lv_event_t *e) { lv_scr_load_anim(scrHome, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 180, 0, false); }

void buildDatacardScreen() {
    scrDatacard = lv_obj_create(NULL);
    lv_obj_add_style(scrDatacard, &s_scr, 0);
    makeSubHeader(scrDatacard, "DATACARD", ev_dc_back);

    // Scanner ring
    dcSpinner = lv_spinner_create(scrDatacard, 1000, 60);
    lv_obj_set_size(dcSpinner, 140, 140);
    lv_obj_align(dcSpinner, LV_ALIGN_CENTER, 0, -50);
    lv_obj_set_style_arc_color(dcSpinner, C_AMB, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(dcSpinner, C_FRM, LV_PART_MAIN);
    lv_obj_set_style_arc_width(dcSpinner, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(dcSpinner, 3, LV_PART_MAIN);

    // Center dot
    lv_obj_t *dot = lv_obj_create(scrDatacard);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 16, 16);
    lv_obj_align(dot, LV_ALIGN_CENTER, 0, -50);
    lv_obj_set_style_bg_color(dot, C_AMB_BRT, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);

    // (outer ring removed)

    // Prompt
    dcPrompt = lv_label_create(scrDatacard);
    lv_label_set_text(dcPrompt, "PRESENT DATACARD\nTO READER PORT");
    lv_obj_set_style_text_font(dcPrompt, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(dcPrompt, C_TXT, 0);
    lv_obj_set_style_text_align(dcPrompt, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(dcPrompt, 1, 0);
    lv_obj_align(dcPrompt, LV_ALIGN_CENTER, 0, 60);

    lv_obj_t *sub = lv_label_create(scrDatacard);
    lv_label_set_text(sub, "READER STANDING BY");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub, C_DIM, 0);
    lv_obj_set_style_text_letter_space(sub, 2, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 100);

    // Result panel
    dcResult = lv_obj_create(scrDatacard);
    lv_obj_set_size(dcResult, W - 16, 240);
    lv_obj_align(dcResult, LV_ALIGN_CENTER, 0, 30);
    lv_obj_add_style(dcResult, &s_pnl, 0);
    lv_obj_set_style_border_width(dcResult, 2, 0);
    lv_obj_add_flag(dcResult, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(dcResult, LV_OBJ_FLAG_SCROLLABLE);

    // Footer
    hline(scrDatacard, H - 28, C_FRM, 1);
    lv_obj_t *ftr = lv_obj_create(scrDatacard);
    lv_obj_set_size(ftr, W, 27); lv_obj_set_pos(ftr, 0, H - 27);
    lv_obj_add_style(ftr, &s_ftr, 0);
    lv_obj_clear_flag(ftr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *fl = lv_label_create(ftr);
    lv_label_set_text(fl, nfcOk ? "READER PORT ACTIVE" : "READER PORT OFFLINE");
    lv_obj_set_style_text_font(fl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(fl, nfcOk ? C_AMB : C_AMB_DIM, 0);
    lv_obj_align(fl, LV_ALIGN_LEFT_MID, 0, 0);
}

// Read NDEF text record from NTAG2xx
char ndefText[128] = "";

void readNdefText() {
    ndefText[0] = 0;
    uint8_t buf[128];
    int pos = 0;

    // Read pages 4-35 (128 bytes of user data)
    for (int page = 4; page < 36 && pos < 120; page++) {
        uint8_t data[4];
        if (!nfc.ntag2xx_ReadPage(page, data)) break;
        for (int i = 0; i < 4 && pos < 127; i++) buf[pos++] = data[i];
    }

    // Walk TLV blocks to find NDEF message (type 0x03)
    // TLV format: type(1) + length(1) + data(length)
    // Type 0x00 = NULL (skip), 0x01 = Lock Control, 0x03 = NDEF, 0xFE = Terminator
    int i = 0;
    while (i < pos) {
        uint8_t tlvType = buf[i];
        if (tlvType == 0x00) { i++; continue; }          // NULL TLV
        if (tlvType == 0xFE) break;                        // Terminator
        if (i + 1 >= pos) break;
        uint8_t tlvLen = buf[i + 1];
        int dataStart = i + 2;

        if (tlvType == 0x03 && tlvLen > 0) {
            // Found NDEF message TLV
            // Parse NDEF record: flags(1) + type_len(1) + payload_len(1) + type + payload
            int r = dataStart;
            if (r + 3 >= pos) break;
            uint8_t flags = buf[r];
            uint8_t typeLen = buf[r + 1];
            uint8_t payLen = buf[r + 2];
            int typeOff = r + 3;

            if (typeLen == 1 && typeOff < pos && buf[typeOff] == 0x54) {
                // Text record ('T' = 0x54)
                int payOff = typeOff + typeLen;
                if (payOff >= pos) break;
                uint8_t langLen = buf[payOff] & 0x3F;
                int textOff = payOff + 1 + langLen;
                int textLen = payLen - 1 - langLen;
                if (textOff + textLen <= pos && textLen > 0 && textLen < 120) {
                    memcpy(ndefText, &buf[textOff], textLen);
                    ndefText[textLen] = 0;
                    return;
                }
            }
            break; // Only check first NDEF record
        }

        // Skip this TLV
        i = dataStart + tlvLen;
    }

    // Fallback: show raw hex
    ndefText[0] = 0;
    for (int j = 0; j < min(pos, 16); j++) {
        char hex[4]; snprintf(hex, sizeof(hex), "%02X ", buf[j]);
        strcat(ndefText, hex);
    }
}

void showCardResult(uint8_t *uid, uint8_t len) {
    totalScans++;

    // If a cargo crate was placed on this reader by mistake, redirect the player.
    if (isCargoTag(ndefText)) {
        lv_obj_add_flag(dcSpinner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(dcPrompt, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(dcResult, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clean(dcResult);
        lv_obj_set_style_border_color(dcResult, C_AMB_BRT, 0);

        lv_obj_t *hd = lv_label_create(dcResult);
        lv_label_set_text(hd, "WRONG READER");
        lv_obj_set_style_text_font(hd, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(hd, C_AMB_BRT, 0);
        lv_obj_set_pos(hd, 10, 8);

        lv_obj_t *nm = lv_label_create(dcResult);
        char nb[64]; snprintf(nb, sizeof(nb), "Detected: %s", ndefText);
        lv_label_set_text(nm, nb);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(nm, C_TXT, 0);
        lv_obj_set_pos(nm, 10, 40);

        lv_obj_t *body = lv_label_create(dcResult);
        lv_label_set_text(body, "This is a supply crate.\n\nReturn to home and use\nCARGO INTEL to scan.");
        lv_obj_set_style_text_font(body, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(body, C_AMB, 0);
        lv_obj_set_width(body, W - 40);
        lv_obj_set_pos(body, 10, 84);

        buzzerScanFail();
        dcScanning = false;
        return;
    }

    // Token = the tag's NDEF text (INTEL_01, EXTRACT_01, ...). No UID-hash
    // fallback: a failed read must never map onto a real mission token.
    const char *token = ndefText;

    // Deliver any comms keyed to this scan ("scan:<token>")
    triggerCommsPrefix("scan", token);

    lv_obj_add_flag(dcSpinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(dcPrompt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(dcResult, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(dcResult);

    ScanOutcome outcome = checkMissionTrigger(token);
    if (outcome.result == SCAN_MISSION_START) {
        // ── MISSION STARTED ──
        const MissionDef &m = ALL_MISSIONS[outcome.mission_idx];
        lv_obj_set_style_border_color(dcResult, C_AMB_BRT, 0);
        accentBar(dcResult, C_AMB_BRT, 5);

        // Badge: "NEW MISSION"
        lv_obj_t *bdg = lv_label_create(dcResult);
        lv_label_set_text(bdg, "NEW MISSION");
        lv_obj_set_style_text_font(bdg, &lv_font_montserrat_12, 0);
        lv_obj_set_style_bg_color(bdg, C_AMB_BRT, 0);
        lv_obj_set_style_bg_opa(bdg, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(bdg, C_BG, 0);
        lv_obj_set_style_radius(bdg, 3, 0);
        lv_obj_set_style_pad_hor(bdg, 8, 0);
        lv_obj_set_style_pad_ver(bdg, 3, 0);
        lv_obj_set_pos(bdg, 14, 0);

        // Title
        lv_obj_t *nm = lv_label_create(dcResult);
        lv_label_set_text(nm, m.title);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(nm, C_AMB_BRT, 0);
        lv_obj_set_pos(nm, 14, 26);

        // Subtitle
        lv_obj_t *sub = lv_label_create(dcResult);
        lv_label_set_text(sub, m.subtitle);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(sub, C_TXT, 0);
        lv_obj_set_pos(sub, 14, 58);
        lv_obj_set_width(sub, W - 60);

        // First objective
        char obuf[64];
        snprintf(obuf, sizeof(obuf), "STEP 1: %s", m.steps[0].title);
        lv_obj_t *obj = lv_label_create(dcResult);
        lv_label_set_text(obj, obuf);
        lv_obj_set_style_text_font(obj, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(obj, C_AMB, 0);
        lv_obj_set_pos(obj, 14, 110);

        // Faction + difficulty
        char fbuf[32];
        snprintf(fbuf, sizeof(fbuf), "%s // DIFFICULTY %d", m.faction, m.difficulty);
        lv_obj_t *fc = lv_label_create(dcResult);
        lv_label_set_text(fc, fbuf);
        lv_obj_set_style_text_font(fc, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(fc, C_DIM, 0);
        lv_obj_set_pos(fc, 14, 140);

        // Status
        lv_obj_t *st = lv_label_create(dcResult);
        lv_label_set_text(st, "MISSION ACCEPTED");
        lv_obj_set_style_text_font(st, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(st, C_AMB_BRT, 0);
        lv_obj_set_pos(st, 14, 170);

        dcScanning = false;
        return;
    }

    if (outcome.result == SCAN_STEP_DONE || outcome.result == SCAN_MISSION_COMPLETE) {
        // ── STEP / MISSION COMPLETE ──
        const MissionDef &m = ALL_MISSIONS[outcome.mission_idx];
        bool fullComplete = (outcome.result == SCAN_MISSION_COMPLETE);
        lv_obj_set_style_border_color(dcResult, C_AMB_BRT, 0);
        accentBar(dcResult, C_AMB_BRT, 5);

        // Badge
        lv_obj_t *bdg = lv_label_create(dcResult);
        lv_label_set_text(bdg, fullComplete ? "MISSION COMPLETE" : "OBJECTIVE COMPLETE");
        lv_obj_set_style_text_font(bdg, &lv_font_montserrat_12, 0);
        lv_obj_set_style_bg_color(bdg, C_AMB_BRT, 0);
        lv_obj_set_style_bg_opa(bdg, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(bdg, C_BG, 0);
        lv_obj_set_style_radius(bdg, 3, 0);
        lv_obj_set_style_pad_hor(bdg, 8, 0);
        lv_obj_set_style_pad_ver(bdg, 3, 0);
        lv_obj_set_pos(bdg, 14, 0);

        // Mission title
        lv_obj_t *nm = lv_label_create(dcResult);
        lv_label_set_text(nm, m.title);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(nm, C_AMB_BRT, 0);
        lv_obj_set_pos(nm, 14, 26);

        // Step completed
        char sbuf[48];
        snprintf(sbuf, sizeof(sbuf), "STEP %d: %s", outcome.step_idx + 1,
                 m.steps[outcome.step_idx].title);
        lv_obj_t *stp = lv_label_create(dcResult);
        lv_label_set_text(stp, sbuf);
        lv_obj_set_style_text_font(stp, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(stp, C_TXT, 0);
        lv_obj_set_pos(stp, 14, 52);

        // XP reward
        char xbuf[32];
        int stepXp = m.steps[outcome.step_idx].xp_reward;
        snprintf(xbuf, sizeof(xbuf), "+%d XP", stepXp);
        lv_obj_t *xpl = lv_label_create(dcResult);
        lv_label_set_text(xpl, xbuf);
        lv_obj_set_style_text_font(xpl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(xpl, C_AMB_BRT, 0);
        lv_obj_set_pos(xpl, 14, 80);

        if (fullComplete) {
            char rbuf[48];
            snprintf(rbuf, sizeof(rbuf), "+%d CREDITS  +%d XP BONUS", m.reward_credits, m.reward_xp);
            lv_obj_t *rw = lv_label_create(dcResult);
            lv_label_set_text(rw, rbuf);
            lv_obj_set_style_text_font(rw, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(rw, C_AMB_BRT, 0);
            lv_obj_set_pos(rw, 14, 110);

            if (m.reward_item[0]) {
                char ibuf[48];
                snprintf(ibuf, sizeof(ibuf), "ACQUIRED: %s", m.reward_item);
                lv_obj_t *it = lv_label_create(dcResult);
                lv_label_set_text(it, ibuf);
                lv_obj_set_style_text_font(it, &lv_font_montserrat_12, 0);
                lv_obj_set_style_text_color(it, C_AMB, 0);
                lv_obj_set_pos(it, 14, 134);
            }
        } else {
            // Show next step
            int slot = findActive(outcome.mission_idx);
            if (slot >= 0) {
                uint8_t nextStep = msnSlots[slot].current_step;
                char nbuf[64];
                snprintf(nbuf, sizeof(nbuf), "NEXT: %s", m.steps[nextStep].title);
                lv_obj_t *nx = lv_label_create(dcResult);
                lv_label_set_text(nx, nbuf);
                lv_obj_set_style_text_font(nx, &lv_font_montserrat_14, 0);
                lv_obj_set_style_text_color(nx, C_AMB, 0);
                lv_obj_set_pos(nx, 14, 110);
            }
        }

        dcScanning = false;
        return;
    }

    // ── FALLBACK: regular datacard (no mission trigger matched) ──
    uint8_t h = uid[0] ^ uid[1] ^ (len > 2 ? uid[2] : 0);
    int t = h % 6;

    const char* tnames[] = {"ACCESS KEY","INTELLIGENCE","MISSION DATA","ARCHIVE","CREDIT CHIP","BOUNTY PUCK"};
    lv_color_t tcols[] = {C_AMB_BRT, C_AMB, C_AMB, C_AMB_DIM, C_AMB_BRT, C_AMB};
    const char* names[] = {"GALEN'S CIPHER","ECHO MANIFEST","PHANTOM BRIEF","SHADOW ARCHIVE","KESTREL CHIP","IMPERIAL WARRANT"};
    const char* descs[] = {
        "Security clearance authorization\nfor restricted sectors",
        "Encrypted fleet intelligence\nfrom Imperial relay",
        "Encoded mission parameters\nfor operative deployment",
        "Historical archive fragment\nfrom abandoned outpost",
        "Loaded credit transfer chip\nvalid at authorized terminals",
        "Active acquisition warrant\nfrom Imperial authority"
    };

    lv_obj_add_flag(dcSpinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(dcPrompt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(dcResult, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(dcResult);
    lv_obj_set_style_border_color(dcResult, tcols[t], 0);

    // Accent bar
    accentBar(dcResult, tcols[t], 5);

    // Type badge
    lv_obj_t *bdg = lv_label_create(dcResult);
    lv_label_set_text(bdg, tnames[t]);
    lv_obj_set_style_text_font(bdg, &lv_font_montserrat_12, 0);
    lv_obj_set_style_bg_color(bdg, tcols[t], 0);
    lv_obj_set_style_bg_opa(bdg, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(bdg, C_BG, 0);
    lv_obj_set_style_radius(bdg, 3, 0);
    lv_obj_set_style_pad_hor(bdg, 8, 0);
    lv_obj_set_style_pad_ver(bdg, 3, 0);
    lv_obj_set_pos(bdg, 14, 0);

    // Name
    lv_obj_t *nm = lv_label_create(dcResult);
    lv_label_set_text(nm, names[t]);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(nm, C_AMB_BRT, 0);
    lv_obj_set_style_text_letter_space(nm, 1, 0);
    lv_obj_set_pos(nm, 14, 26);

    // UID
    char ub[40];
    if (len == 4) snprintf(ub, sizeof(ub), "IDENT  %02X%02X-%02X%02X", uid[0],uid[1],uid[2],uid[3]);
    else snprintf(ub, sizeof(ub), "IDENT  %02X%02X-%02X%02X-%02X%02X-%02X", uid[0],uid[1],uid[2],uid[3],uid[4],uid[5],uid[6]);
    lv_obj_t *id = lv_label_create(dcResult);
    lv_label_set_text(id, ub);
    lv_obj_set_style_text_font(id, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(id, C_AMB_DIM, 0);
    lv_obj_set_style_text_letter_space(id, 1, 0);
    lv_obj_set_pos(id, 14, 58);

    // Separator
    lv_obj_t *sep = lv_obj_create(dcResult);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, W - 60, 1);
    lv_obj_set_pos(sep, 14, 80);
    lv_obj_set_style_bg_color(sep, C_FRM, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

    // Description
    lv_obj_t *ds = lv_label_create(dcResult);
    lv_label_set_text(ds, descs[t]);
    lv_obj_set_style_text_font(ds, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ds, C_TXT, 0);
    lv_obj_set_style_text_line_space(ds, 4, 0);
    lv_obj_set_pos(ds, 14, 90);
    lv_obj_set_width(ds, W - 60);

    // Status
    char sb[32]; snprintf(sb, sizeof(sb), "SCAN #%d REGISTERED", totalScans);
    lv_obj_t *st = lv_label_create(dcResult);
    lv_label_set_text(st, sb);
    lv_obj_set_style_text_font(st, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(st, C_AMB_BRT, 0);
    lv_obj_set_pos(st, 14, 165);

    dcScanning = false;
}

void resetDcScan() {
    lv_obj_clear_flag(dcSpinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(dcPrompt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(dcResult, LV_OBJ_FLAG_HIDDEN);
    dcScanning = true;
}

// ═══════════════════════════════════════
//  NEARBY SCREEN
// ═══════════════════════════════════════
static void ev_nb_back(lv_event_t *e) { lv_scr_load_anim(scrHome, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 180, 0, false); }
static void ev_rescan(lv_event_t *e);

void buildNearbyScreen() {
    scrNearby = lv_obj_create(NULL);
    lv_obj_add_style(scrNearby, &s_scr, 0);

    // Status label at top (below header line)
    nbStatus = lv_label_create(scrNearby);
    lv_label_set_text(nbStatus, "SCANNING...");
    lv_obj_set_style_text_font(nbStatus, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(nbStatus, C_DIM, 0);
    lv_obj_set_style_text_letter_space(nbStatus, 1, 0);
    lv_obj_set_pos(nbStatus, 12, 56);

    hline(scrNearby, 70, C_FRM, 1);

    // Scrollable list
    nbList = lv_obj_create(scrNearby);
    lv_obj_set_size(nbList, W - 8, H - 132);
    lv_obj_set_pos(nbList, 4, 78);
    lv_obj_set_style_bg_opa(nbList, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(nbList, 0, 0);
    lv_obj_set_style_radius(nbList, 0, 0);
    lv_obj_set_style_pad_all(nbList, 0, 0);
    lv_obj_set_flex_flow(nbList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(nbList, 6, 0);

    // Scanning spinner — created AFTER list so it renders on top
    nbSpinner = lv_spinner_create(scrNearby, 1000, 60);
    lv_obj_set_size(nbSpinner, 100, 100);
    lv_obj_align(nbSpinner, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_arc_color(nbSpinner, C_AMB, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(nbSpinner, C_FRM, LV_PART_MAIN);
    lv_obj_set_style_arc_width(nbSpinner, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(nbSpinner, 3, LV_PART_MAIN);
    lv_obj_add_flag(nbSpinner, LV_OBJ_FLAG_HIDDEN);  // hidden until scan starts

    // Rescan button
    hline(scrNearby, H - 52, C_FRM, 1);
    lv_obj_t *rs = lv_btn_create(scrNearby);
    lv_obj_set_size(rs, W - 40, 38);
    lv_obj_set_pos(rs, 20, H - 48);
    lv_obj_set_style_bg_color(rs, C_BG, 0);
    lv_obj_set_style_bg_opa(rs, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(rs, C_PNL2, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(rs, C_AMB_DIM, 0);
    lv_obj_set_style_border_width(rs, 1, 0);
    lv_obj_set_style_radius(rs, 2, 0);
    lv_obj_set_style_shadow_width(rs, 0, 0);
    lv_obj_add_event_cb(rs, ev_rescan, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(rs);
    lv_label_set_text(rl, ">> RESCAN <<");
    lv_obj_set_style_text_color(rl, C_AMB, 0);
    lv_obj_set_style_text_letter_space(rl, 1, 0);
    lv_obj_center(rl);

    // Header bar — on top of everything (created after list)
    lv_obj_t *hdr = lv_obj_create(scrNearby);
    lv_obj_set_size(hdr, W, 46);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, C_BG, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Title — right side, clear of back button
    lv_obj_t *tt = lv_label_create(hdr);
    lv_label_set_text(tt, "SLICE");
    lv_obj_set_style_text_font(tt, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(tt, C_AMB_BRT, 0);
    lv_obj_set_pos(tt, W - 70, 13);

    hline(scrNearby, 46, C_AMB_DIM, 2);

    // Back button — very last, on top of header
    lv_obj_t *bb = lv_btn_create(scrNearby);
    lv_obj_set_size(bb, 80, 36);
    lv_obj_set_pos(bb, 6, 5);
    lv_obj_set_style_bg_color(bb, C_BG, 0);
    lv_obj_set_style_bg_opa(bb, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bb, C_PNL2, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(bb, C_AMB_DIM, 0);
    lv_obj_set_style_border_width(bb, 1, 0);
    lv_obj_set_style_radius(bb, 2, 0);
    lv_obj_set_style_shadow_width(bb, 0, 0);
    lv_obj_set_style_pad_all(bb, 0, 0);
    lv_obj_add_event_cb(bb, ev_nb_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bbl = lv_label_create(bb);
    lv_label_set_text(bbl, "< BACK");
    lv_obj_set_style_text_font(bbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bbl, C_AMB, 0);
    lv_obj_center(bbl);
}

void openPropScreen(int propIdx);  // forward decl
unsigned long nbResultsTime = 0;   // when results appeared (debounce taps)
int pendingPropConnect = -1;       // deferred connection (set by tap, executed in loop)

// Event: tap a discovered prop card — just sets a flag, actual connect in loop
static void ev_tap_prop(lv_event_t *e) {
    if (millis() - nbResultsTime < 500) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < foundCount) {
        pendingPropConnect = idx;
    }
}

bool nbScanDone = false;

void doFullScan() {
    lv_label_set_text(nbStatus, "SCANNING...");
    lv_obj_clean(nbList);
    lv_obj_clear_flag(nbSpinner, LV_OBJ_FLAG_HIDDEN);
    lv_timer_handler();

    // Start async scan so spinner keeps spinning
    WiFi.mode(WIFI_STA);
    WiFi.scanNetworks(true);

    // Pump LVGL while waiting for results (spinner animates)
    int n = -1;
    unsigned long t0 = millis();
    while (millis() - t0 < 8000) {
        lv_timer_handler();
        delay(20);
        n = WiFi.scanComplete();
        if (n >= 0) break;
        if (n == -2) { // failed, retry once
            delay(200);
            WiFi.scanNetworks(true);
        }
    }
    if (n < 0) n = 0;
    S.printf("[SCAN] Found %d total networks\n", n);

    foundCount = 0;

    for (int i = 0; i < n && foundCount < MAX_PROPS; i++) {
        String ssid = WiFi.SSID(i);
        if (!ssid.startsWith("SWTS_")) continue;

        FoundProp &fp = foundProps[foundCount];
        strlcpy(fp.ssid, ssid.c_str(), sizeof(fp.ssid));
        fp.rssi = WiFi.RSSI(i);
        const char *rest = ssid.c_str() + 5;

        // In-universe names for known prop SSIDs
        if (strncmp(rest,"TERM_",5)==0) {
            int num = atoi(rest+5);
            const char* tnames[] = {"Imperial Comm Relay","Security Terminal","Decrypt Station"};
            strlcpy(fp.name, tnames[num > 0 && num <= 3 ? num-1 : 0], 24);
        } else if (strncmp(rest,"PANEL_",6)==0) {
            int num = atoi(rest+6);
            const char* pnames[] = {"Outpost Data Terminal","Rebel Intel Drop","Imperial Wanted Board","Ship Schematics Bay","Jedi Lore Archive","Faction Scoreboard"};
            strlcpy(fp.name, pnames[num > 0 && num <= 6 ? num-1 : 0], 24);
        } else if (strncmp(rest,"DROID_",6)==0) {
            if (strstr(rest,"R5")) strlcpy(fp.name,"R5-D4 Astromech",24);
            else if (strstr(rest,"GNK")) strlcpy(fp.name,"GNK Power Droid",24);
            else snprintf(fp.name,24,"Droid %s",rest+6);
        } else {
            strlcpy(fp.name,rest,24);
        }

        // Create card (NOT clickable yet — enabled after delay to prevent phantom taps)
        lv_obj_t *card = lv_obj_create(nbList);
        lv_obj_set_size(card, W - 20, 56);
        lv_obj_add_style(card, &s_pnl, 0);
        lv_obj_set_style_border_color(card, C_AMB, LV_STATE_DEFAULT);
        lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(card, 4, 0);
        lv_obj_set_style_bg_color(card, C_PNL2, LV_STATE_PRESSED);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);  // disabled initially
        lv_obj_add_event_cb(card, ev_tap_prop, LV_EVENT_CLICKED, (void*)(intptr_t)foundCount);

        // Name (full in-universe name)
        lv_obj_t *nl = lv_label_create(card);
        lv_label_set_text(nl, fp.name);
        lv_obj_set_style_text_font(nl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(nl, C_AMB_BRT, 0);
        lv_obj_set_pos(nl, 4, -2);
        lv_obj_clear_flag(nl, LV_OBJ_FLAG_CLICKABLE);

        // Signal strength as text
        char sigTxt[16];
        int bars = fp.rssi > -50 ? 5 : fp.rssi > -60 ? 4 : fp.rssi > -70 ? 3 : fp.rssi > -80 ? 2 : 1;
        snprintf(sigTxt, sizeof(sigTxt), "SIGNAL: %d/5", bars);
        lv_obj_t *il = lv_label_create(card);
        lv_label_set_text(il, sigTxt);
        lv_obj_set_style_text_font(il, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(il, C_DIM, 0);
        lv_obj_set_pos(il, 4, 22);
        lv_obj_clear_flag(il, LV_OBJ_FLAG_CLICKABLE);

        foundCount++;
    }

    WiFi.scanDelete(); WiFi.mode(WIFI_OFF);
    lv_obj_add_flag(nbSpinner, LV_OBJ_FLAG_HIDDEN);
    nbResultsTime = millis();  // cards become clickable 1s after this
    char buf[32];
    snprintf(buf, 32, foundCount ? "%d SIGNAL%s DETECTED" : "NO SIGNALS IN RANGE", foundCount, foundCount>1?"S":"");
    lv_label_set_text(nbStatus, buf);
}

static void ev_rescan(lv_event_t *e) { doFullScan(); }

// ═══════════════════════════════════════
//  PROP INTERACTION — connect + interact
// ═══════════════════════════════════════

void buildPropScreen();

bool connectToProp(const char* ssid) {
    S.printf("[PROP] Connecting to %s...\n", ssid);
    WiFi.begin(ssid);

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
        lv_timer_handler();
        delay(50);
    }
    if (WiFi.status() == WL_CONNECTED) {
        S.printf("[PROP] Connected! IP=%s\n", WiFi.localIP().toString().c_str());
        delay(200);
        return true;
    }
    S.printf("[PROP] Failed (status=%d)\n", WiFi.status());
    WiFi.disconnect();
    return false;
}

bool fetchPropInfo() {
    HTTPClient http;
    http.setTimeout(5000);
    http.begin("http://192.168.4.1/api/info");
    int code = http.GET();
    if (code == 200) {
        JsonDocument doc;
        deserializeJson(doc, http.getString());
        strlcpy(propName, doc["name"] | "UNKNOWN", sizeof(propName));
        strlcpy(propFaction, doc["faction"] | "NEUTRAL", sizeof(propFaction));
        strlcpy(propId, doc["prop_id"] | "??", sizeof(propId));
        propHasMinigame = doc["features"]["has_minigame"] | false;
        strlcpy(propMinigameType, doc["features"]["minigame_type"] | "", sizeof(propMinigameType));
        S.printf("[PROP] Info: %s (%s) minigame=%d\n", propName, propFaction, propHasMinigame);
        http.end();
        return true;
    }
    S.printf("[PROP] /api/info failed: %d\n", code);
    http.end();
    return false;
}

String postInteract(const char* action) {
    HTTPClient http;
    http.setTimeout(5000);
    http.begin("http://192.168.4.1/api/interact");
    http.addHeader("Content-Type", "application/json");

    JsonDocument req;
    req["action"] = action;
    JsonObject p = req["player"].to<JsonObject>();
    p["callsign"] = callsign;
    p["faction"] = "REBEL";

    String body;
    serializeJson(req, body);
    int code = http.POST(body);
    String resp = "";
    if (code == 200) resp = http.getString();
    else S.printf("[PROP] interact failed: %d\n", code);
    http.end();
    return resp;
}

void disconnectProp() {
    WiFi.disconnect();
    propConnected = false;
    S.println("[PROP] Disconnected");
}

// Prop screen UI elements
lv_obj_t *propContent = NULL;  // scrollable content area

static void ev_prop_back(lv_event_t *e) {
    disconnectProp();
    lv_scr_load_anim(scrNearby, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 180, 0, false);
}

// ═══════════════════════════════════════
//  SLICE MINIGAME — timing bar hacking game
// ═══════════════════════════════════════

// Game state
#define SLICE_BAR_X  20
#define SLICE_BAR_Y  220
#define SLICE_BAR_W  280
#define SLICE_BAR_H  40
#define MAX_ZONES    3

struct SliceZone { float start; float end; int points; bool hit; };
SliceZone sliceZones[MAX_ZONES];
int sliceZoneCount = 0;
float sliceCursorPos = 0.0f;   // 0.0 to 1.0
float sliceCursorDir = 1.0f;   // +1 or -1
float sliceCursorSpeed = 0.012f;
int sliceScore = 0;
int sliceRound = 0;
int sliceMaxRounds = 3;
int sliceTimeLimit = 45;
bool sliceActive = false;
bool sliceWon = false;
unsigned long sliceStartTime = 0;
unsigned long sliceEndTime = 0;   // millis() when the game ended (0 = still running)

// LVGL objects for the game
lv_obj_t *sliceBarBg = NULL;
lv_obj_t *sliceCursor = NULL;
lv_obj_t *sliceZoneObjs[MAX_ZONES];
lv_obj_t *sliceTitle = NULL;
lv_obj_t *sliceStatus = NULL;
lv_obj_t *sliceTimer = NULL;
lv_obj_t *sliceRoundLbl = NULL;
lv_obj_t *sliceScoreLbl = NULL;
lv_obj_t *sliceHintLbl = NULL;

void buildSliceScreen();

// Tap handler — lock the cursor
static void ev_slice_tap(lv_event_t *e) {
    if (!sliceActive) return;

    // Check if cursor is in any zone
    bool hitAny = false;
    for (int i = 0; i < sliceZoneCount; i++) {
        if (!sliceZones[i].hit && sliceCursorPos >= sliceZones[i].start && sliceCursorPos <= sliceZones[i].end) {
            sliceZones[i].hit = true;
            sliceScore += sliceZones[i].points;
            hitAny = true;
            // Flash the zone green briefly
            lv_obj_set_style_bg_color(sliceZoneObjs[i], C_AMB_BRT, 0);
            break;
        }
    }

    if (!hitAny) {
        // Miss — flash cursor red, penalty
        lv_obj_set_style_bg_color(sliceCursor, lv_color_hex(0x880000), 0);
        sliceScore = max(0, sliceScore - 20);
    }

    // Check if all zones hit
    bool allHit = true;
    for (int i = 0; i < sliceZoneCount; i++) {
        if (!sliceZones[i].hit) { allHit = false; break; }
    }

    if (allHit) {
        sliceRound++;
        if (sliceRound >= sliceMaxRounds) {
            // WIN — result is reported to the panel from loop() after the
            // end-screen delay (a bare postInteract here would read as won=false)
            sliceActive = false;
            sliceWon = true;
            sliceEndTime = millis();
            lv_label_set_text(sliceStatus, "SYSTEM BREACHED");
            lv_obj_set_style_text_color(sliceStatus, C_AMB_BRT, 0);
            lv_label_set_text(sliceHintLbl, "ACCESS GRANTED");
        } else {
            // Next round — regenerate zones (harder)
            sliceCursorSpeed += 0.002f;
            for (int i = 0; i < sliceZoneCount; i++) {
                sliceZones[i].hit = false;
                // Shrink zones slightly each round
                float center = (sliceZones[i].start + sliceZones[i].end) / 2.0f;
                float halfW = (sliceZones[i].end - sliceZones[i].start) / 2.0f * 0.9f;
                sliceZones[i].start = max(0.0f, center - halfW);
                sliceZones[i].end = min(1.0f, center + halfW);
            }
            // Update zone visuals
            for (int i = 0; i < sliceZoneCount; i++) {
                int zx = SLICE_BAR_X + (int)(sliceZones[i].start * SLICE_BAR_W);
                int zw = (int)((sliceZones[i].end - sliceZones[i].start) * SLICE_BAR_W);
                lv_obj_set_pos(sliceZoneObjs[i], zx, SLICE_BAR_Y);
                lv_obj_set_size(sliceZoneObjs[i], zw, SLICE_BAR_H);
                lv_obj_set_style_bg_color(sliceZoneObjs[i], C_AMB_DIM, 0);
            }
            char rbuf[16];
            snprintf(rbuf, sizeof(rbuf), "ROUND %d/%d", sliceRound + 1, sliceMaxRounds);
            lv_label_set_text(sliceRoundLbl, rbuf);
        }
    }

    // Update score
    char sbuf[16];
    snprintf(sbuf, sizeof(sbuf), "SCORE: %d", sliceScore);
    lv_label_set_text(sliceScoreLbl, sbuf);
}

void startSliceGame(int difficulty) {
    sliceScore = 0;
    sliceRound = 0;
    sliceMaxRounds = 1 + difficulty;
    sliceCursorPos = 0.0f;
    sliceCursorDir = 1.0f;
    sliceCursorSpeed = 0.005f + difficulty * 0.002f;
    sliceActive = true;
    sliceWon = false;
    sliceStartTime = millis();
    sliceEndTime = 0;
    sliceTimeLimit = 60 - difficulty * 5;

    // Generate zones — wider and easier
    sliceZoneCount = min(MAX_ZONES, 1 + difficulty / 3);
    float pos = 0.12f;
    for (int i = 0; i < sliceZoneCount; i++) {
        float width = 0.25f - difficulty * 0.03f;
        if (width < 0.10f) width = 0.10f;
        sliceZones[i].start = pos;
        sliceZones[i].end = pos + width;
        sliceZones[i].points = 50 + i * 30;
        sliceZones[i].hit = false;
        pos += width + 0.15f + (i * 0.05f);
    }
}

void buildSliceScreen() {
    scrSlice = lv_obj_create(NULL);
    lv_obj_add_style(scrSlice, &s_scr, 0);
    lv_obj_add_flag(scrSlice, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scrSlice, ev_slice_tap, LV_EVENT_CLICKED, NULL);

    // Title
    sliceTitle = lv_label_create(scrSlice);
    lv_label_set_text(sliceTitle, "SLICING SYSTEM");
    lv_obj_set_style_text_font(sliceTitle, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(sliceTitle, C_AMB_BRT, 0);
    lv_obj_set_style_text_letter_space(sliceTitle, 2, 0);
    lv_obj_align(sliceTitle, LV_ALIGN_TOP_MID, 0, 16);

    // Status
    sliceStatus = lv_label_create(scrSlice);
    lv_label_set_text(sliceStatus, "BYPASS ENCRYPTION");
    lv_obj_set_style_text_font(sliceStatus, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sliceStatus, C_AMB, 0);
    lv_obj_align(sliceStatus, LV_ALIGN_TOP_MID, 0, 48);

    // Round indicator
    sliceRoundLbl = lv_label_create(scrSlice);
    lv_label_set_text(sliceRoundLbl, "ROUND 1/3");
    lv_obj_set_style_text_font(sliceRoundLbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sliceRoundLbl, C_DIM, 0);
    lv_obj_set_pos(sliceRoundLbl, 20, 80);

    // Timer
    sliceTimer = lv_label_create(scrSlice);
    lv_label_set_text(sliceTimer, "TIME: 45s");
    lv_obj_set_style_text_font(sliceTimer, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sliceTimer, C_DIM, 0);
    lv_obj_set_pos(sliceTimer, W - 100, 80);

    // Score
    sliceScoreLbl = lv_label_create(scrSlice);
    lv_label_set_text(sliceScoreLbl, "SCORE: 0");
    lv_obj_set_style_text_font(sliceScoreLbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(sliceScoreLbl, C_AMB, 0);
    lv_obj_align(sliceScoreLbl, LV_ALIGN_TOP_MID, 0, 100);

    // Decorative lines above bar
    hline(scrSlice, 130, C_FRM, 1);
    hline(scrSlice, 200, C_FRM, 1);

    // "ENCRYPTION LAYER" labels
    for (int i = 0; i < 3; i++) {
        lv_obj_t *el = lv_label_create(scrSlice);
        char ebuf[24];
        snprintf(ebuf, sizeof(ebuf), "LAYER %d", i + 1);
        lv_label_set_text(el, ebuf);
        lv_obj_set_style_text_font(el, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(el, C_MUT, 0);
        lv_obj_set_pos(el, 20 + i * 100, 140);
    }

    // Visualization: vertical bars (fake "code" above the game bar)
    for (int i = 0; i < 28; i++) {
        lv_obj_t *vb = lv_obj_create(scrSlice);
        lv_obj_remove_style_all(vb);
        int bh = 10 + (i * 7 + 3) % 40;
        lv_obj_set_size(vb, 6, bh);
        lv_obj_set_pos(vb, 20 + i * 10, 200 - bh);
        lv_obj_set_style_bg_color(vb, C_FRM, 0);
        lv_obj_set_style_bg_opa(vb, LV_OPA_COVER, 0);
    }

    // ── THE GAME BAR ──
    // Background bar
    sliceBarBg = lv_obj_create(scrSlice);
    lv_obj_remove_style_all(sliceBarBg);
    lv_obj_set_size(sliceBarBg, SLICE_BAR_W, SLICE_BAR_H);
    lv_obj_set_pos(sliceBarBg, SLICE_BAR_X, SLICE_BAR_Y);
    lv_obj_set_style_bg_color(sliceBarBg, C_PNL, 0);
    lv_obj_set_style_bg_opa(sliceBarBg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(sliceBarBg, C_AMB_DIM, 0);
    lv_obj_set_style_border_width(sliceBarBg, 2, 0);

    // Target zones (created dynamically in startSliceGame, but pre-create objects)
    for (int i = 0; i < MAX_ZONES; i++) {
        sliceZoneObjs[i] = lv_obj_create(scrSlice);
        lv_obj_remove_style_all(sliceZoneObjs[i]);
        lv_obj_set_size(sliceZoneObjs[i], 40, SLICE_BAR_H);
        lv_obj_set_pos(sliceZoneObjs[i], SLICE_BAR_X, SLICE_BAR_Y);
        lv_obj_set_style_bg_color(sliceZoneObjs[i], C_AMB_DIM, 0);
        lv_obj_set_style_bg_opa(sliceZoneObjs[i], LV_OPA_COVER, 0);
        lv_obj_add_flag(sliceZoneObjs[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Cursor (bright amber vertical line)
    sliceCursor = lv_obj_create(scrSlice);
    lv_obj_remove_style_all(sliceCursor);
    lv_obj_set_size(sliceCursor, 4, SLICE_BAR_H + 12);
    lv_obj_set_pos(sliceCursor, SLICE_BAR_X, SLICE_BAR_Y - 6);
    lv_obj_set_style_bg_color(sliceCursor, C_AMB_BRT, 0);
    lv_obj_set_style_bg_opa(sliceCursor, LV_OPA_COVER, 0);

    // Tick marks along bottom of bar
    for (int i = 0; i <= 10; i++) {
        lv_obj_t *tick = lv_obj_create(scrSlice);
        lv_obj_remove_style_all(tick);
        lv_obj_set_size(tick, 1, (i % 5 == 0) ? 10 : 5);
        lv_obj_set_pos(tick, SLICE_BAR_X + i * (SLICE_BAR_W / 10), SLICE_BAR_Y + SLICE_BAR_H + 4);
        lv_obj_set_style_bg_color(tick, C_FRM_HI, 0);
        lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
    }

    // Hint text
    sliceHintLbl = lv_label_create(scrSlice);
    lv_label_set_text(sliceHintLbl, "TAP WHEN CURSOR\nIS IN THE TARGET ZONE");
    lv_obj_set_style_text_font(sliceHintLbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(sliceHintLbl, C_AMB, 0);
    lv_obj_set_style_text_align(sliceHintLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(sliceHintLbl, W - 40);
    lv_obj_align(sliceHintLbl, LV_ALIGN_BOTTOM_MID, 0, -60);

    // Bottom decorative line
    hline(scrSlice, H - 40, C_AMB_DIM, 2);

    lv_obj_t *ftr = lv_label_create(scrSlice);
    lv_label_set_text(ftr, "CZERKA INTRUSION SUITE v2.1");
    lv_obj_set_style_text_font(ftr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ftr, C_MUT, 0);
    lv_obj_align(ftr, LV_ALIGN_BOTTOM_MID, 0, -16);
}

void launchSliceGame(int difficulty) {
    startSliceGame(difficulty);

    // Position zones
    for (int i = 0; i < MAX_ZONES; i++) {
        if (i < sliceZoneCount) {
            int zx = SLICE_BAR_X + (int)(sliceZones[i].start * SLICE_BAR_W);
            int zw = (int)((sliceZones[i].end - sliceZones[i].start) * SLICE_BAR_W);
            lv_obj_set_pos(sliceZoneObjs[i], zx, SLICE_BAR_Y);
            lv_obj_set_size(sliceZoneObjs[i], zw, SLICE_BAR_H);
            lv_obj_set_style_bg_color(sliceZoneObjs[i], C_AMB_DIM, 0);
            lv_obj_clear_flag(sliceZoneObjs[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(sliceZoneObjs[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Reset cursor
    lv_obj_set_pos(sliceCursor, SLICE_BAR_X, SLICE_BAR_Y - 6);
    lv_obj_set_style_bg_color(sliceCursor, C_AMB_BRT, 0);

    // Reset labels
    char rbuf[16]; snprintf(rbuf, sizeof(rbuf), "ROUND 1/%d", sliceMaxRounds);
    lv_label_set_text(sliceRoundLbl, rbuf);
    lv_label_set_text(sliceScoreLbl, "SCORE: 0");
    lv_label_set_text(sliceStatus, "BYPASS ENCRYPTION");
    lv_obj_set_style_text_color(sliceStatus, C_AMB, 0);
    lv_label_set_text(sliceHintLbl, "TAP WHEN CURSOR\nIS IN THE TARGET ZONE");
    char tbuf[16]; snprintf(tbuf, sizeof(tbuf), "TIME: %ds", sliceTimeLimit);
    lv_label_set_text(sliceTimer, tbuf);

    lv_scr_load_anim(scrSlice, LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false);
}

// Update cursor each frame
void updateSliceGame() {
    if (!sliceActive) return;

    // Move cursor
    sliceCursorPos += sliceCursorSpeed * sliceCursorDir;
    if (sliceCursorPos >= 1.0f) { sliceCursorPos = 1.0f; sliceCursorDir = -1.0f; }
    if (sliceCursorPos <= 0.0f) { sliceCursorPos = 0.0f; sliceCursorDir = 1.0f; }

    // Position cursor
    int cx = SLICE_BAR_X + (int)(sliceCursorPos * SLICE_BAR_W) - 2;
    lv_obj_set_pos(sliceCursor, cx, SLICE_BAR_Y - 6);

    // Reset cursor color (in case it was red from a miss)
    lv_obj_set_style_bg_color(sliceCursor, C_AMB_BRT, 0);

    // Check if cursor is over a zone — highlight it
    for (int i = 0; i < sliceZoneCount; i++) {
        if (!sliceZones[i].hit && sliceCursorPos >= sliceZones[i].start && sliceCursorPos <= sliceZones[i].end) {
            lv_obj_set_style_bg_color(sliceZoneObjs[i], C_AMB, 0);
        } else if (!sliceZones[i].hit) {
            lv_obj_set_style_bg_color(sliceZoneObjs[i], C_AMB_DIM, 0);
        }
    }

    // Timer
    int elapsed = (millis() - sliceStartTime) / 1000;
    int remaining = sliceTimeLimit - elapsed;
    if (remaining <= 0) {
        // Time's up — fail
        sliceActive = false;
        sliceWon = false;
        sliceEndTime = millis();
        lv_label_set_text(sliceStatus, "SLICE FAILED");
        lv_obj_set_style_text_color(sliceStatus, C_AMB_DIM, 0);
        lv_label_set_text(sliceHintLbl, "SYSTEM LOCKED OUT");
        lv_label_set_text(sliceTimer, "TIME: 0s");
    } else {
        char tbuf[16]; snprintf(tbuf, sizeof(tbuf), "TIME: %ds", remaining);
        lv_label_set_text(sliceTimer, tbuf);
    }
}

// ═══════════════════════════════════════
//  SIMON SAYS MINIGAME — "PATTERN LOCK"
//  Uses the three physical arcade buttons (Blue / White / Red) and their
//  LEDs + buzzer. The panel plays back a growing color sequence on the
//  LEDs; the player repeats it on the buttons. One extra step is revealed
//  each round. Clear `simonRounds` rounds to breach the lock.
//
//  Number of rounds comes from the panel's start_simon JSON ("rounds"),
//  defaulting to SIMON_DEFAULT_ROUNDS.
// ═══════════════════════════════════════
#define SIMON_MAX_ROUNDS     16
#define SIMON_DEFAULT_ROUNDS 3

// Playback timing (non-blocking, driven from loop())
#define SIMON_ON_MS     460   // LED lit per step
#define SIMON_OFF_MS    220   // gap between steps
#define SIMON_START_MS  650   // pause before a round's playback begins
#define SIMON_INPUT_MS  10000 // per-round input inactivity timeout

// Per-color playback tones (match the boot-time button tones)
static const int SIMON_TONE[3] = { 600, 1500, 3000 };  // BLUE, WHITE, RED

enum SimonPhase : uint8_t {
    SP_PRE,        // short pause, then playback
    SP_PLAY,       // flashing the sequence back
    SP_INPUT,      // waiting for the player to repeat it
    SP_ROUND_OK,   // brief success flash before next round
    SP_DONE        // won or lost — waiting to return to prop screen
};

uint8_t  simonSeq[SIMON_MAX_ROUNDS];
int      simonRounds   = SIMON_DEFAULT_ROUNDS;  // total rounds (from JSON)
int      simonLen      = 0;   // steps revealed this round (== round number)
int      simonPlayIdx  = 0;   // playback cursor
int      simonInputPos = 0;   // correct presses so far this round
bool     simonLedOn    = false;
SimonPhase simonPhase  = SP_PRE;
unsigned long simonPhaseTime = 0;
unsigned long simonInputTime = 0;
unsigned long simonEndTime   = 0;   // millis() when the game ended (0 = still running)
bool     simonActive = false;
bool     simonWon    = false;

// LVGL objects
lv_obj_t *simonTitle   = NULL;
lv_obj_t *simonStatus  = NULL;
lv_obj_t *simonRoundLbl= NULL;
lv_obj_t *simonHintLbl = NULL;
lv_obj_t *simonPad[3]  = { NULL, NULL, NULL };  // on-screen mirror of the LEDs

static const char *SIMON_PAD_LBL[3] = { "BLU", "WHT", "RED" };

// On-screen column for each color, mirroring the physical button layout
// on the board (left to right: WHITE, BLUE, RED).
static const int SIMON_PAD_COL[3] = { 1, 0, 2 };   // BLUE→mid, WHITE→left, RED→right

void buildSimonScreen();

// Light/darken the on-screen pad for a color (mirrors the physical LED)
static void simonPadSet(int c, bool on) {
    if (!simonPad[c]) return;
    lv_obj_set_style_bg_color(simonPad[c], on ? C_AMB_BRT : C_PNL, 0);
    lv_obj_set_style_border_color(simonPad[c], on ? C_AMB_BRT : C_AMB_DIM, 0);
}
static void simonAllPadsOff() { for (int i = 0; i < 3; i++) simonPadSet(i, false); }

// Turn a color's button LED + on-screen pad on/off together
static void simonShow(int c, bool on) {
    if (on) { digitalWrite(BUTTONS[c].led, HIGH); btnState[c].ledHeld = true; btnState[c].ledUntil = 0; }
    else    ledOff((BtnColor)c);
    simonPadSet(c, on);
}

static void simonSetRoundLabel() {
    char rbuf[20];
    snprintf(rbuf, sizeof(rbuf), "ROUND %d/%d", simonLen, simonRounds);
    lv_label_set_text(simonRoundLbl, rbuf);
}

void startSimonGame(int rounds) {
    simonRounds = rounds;
    if (simonRounds < 1) simonRounds = 1;
    if (simonRounds > SIMON_MAX_ROUNDS) simonRounds = SIMON_MAX_ROUNDS;

    // Pre-generate the full random sequence; reveal one more step each round.
    for (int i = 0; i < simonRounds; i++) simonSeq[i] = (uint8_t)(esp_random() % 3);

    simonLen      = 1;
    simonPlayIdx  = 0;
    simonInputPos = 0;
    simonLedOn    = false;
    simonActive   = true;
    simonWon      = false;
    simonEndTime  = 0;
    simonPhase    = SP_PRE;
    simonPhaseTime = millis();
}

void buildSimonScreen() {
    scrSimon = lv_obj_create(NULL);
    lv_obj_add_style(scrSimon, &s_scr, 0);
    lv_obj_clear_flag(scrSimon, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    simonTitle = lv_label_create(scrSimon);
    lv_label_set_text(simonTitle, "PATTERN LOCK");
    lv_obj_set_style_text_font(simonTitle, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(simonTitle, C_AMB_BRT, 0);
    lv_obj_set_style_text_letter_space(simonTitle, 2, 0);
    lv_obj_align(simonTitle, LV_ALIGN_TOP_MID, 0, 16);

    // Status line
    simonStatus = lv_label_create(scrSimon);
    lv_label_set_text(simonStatus, "SLICING SECURITY ICE");
    lv_obj_set_style_text_font(simonStatus, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(simonStatus, C_AMB, 0);
    lv_obj_align(simonStatus, LV_ALIGN_TOP_MID, 0, 48);

    // Round counter
    simonRoundLbl = lv_label_create(scrSimon);
    lv_label_set_text(simonRoundLbl, "ROUND 1/3");
    lv_obj_set_style_text_font(simonRoundLbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(simonRoundLbl, C_DIM, 0);
    lv_obj_align(simonRoundLbl, LV_ALIGN_TOP_MID, 0, 74);

    hline(scrSimon, 110, C_FRM, 1);

    // Three pads mirroring the physical buttons (BLU / WHT / RED order)
    const int padW = 84, padH = 120, gap = 14;
    const int totalW = padW * 3 + gap * 2;
    const int x0 = (W - totalW) / 2;
    const int padY = 150;
    for (int i = 0; i < 3; i++) {
        lv_obj_t *pad = lv_obj_create(scrSimon);
        lv_obj_remove_style_all(pad);
        lv_obj_set_size(pad, padW, padH);
        lv_obj_set_pos(pad, x0 + SIMON_PAD_COL[i] * (padW + gap), padY);
        lv_obj_set_style_bg_color(pad, C_PNL, 0);
        lv_obj_set_style_bg_opa(pad, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(pad, C_AMB_DIM, 0);
        lv_obj_set_style_border_width(pad, 2, 0);
        lv_obj_set_style_radius(pad, 4, 0);
        lv_obj_clear_flag(pad, LV_OBJ_FLAG_SCROLLABLE);
        simonPad[i] = pad;

        lv_obj_t *pl = lv_label_create(pad);
        lv_label_set_text(pl, SIMON_PAD_LBL[i]);
        lv_obj_set_style_text_font(pl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(pl, C_DIM, 0);
        lv_obj_align(pl, LV_ALIGN_BOTTOM_MID, 0, -6);
    }

    // Hint / prompt at the bottom
    simonHintLbl = lv_label_create(scrSimon);
    lv_label_set_text(simonHintLbl, "WATCH THE SEQUENCE");
    lv_obj_set_style_text_font(simonHintLbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(simonHintLbl, C_AMB, 0);
    lv_obj_set_style_text_align(simonHintLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(simonHintLbl, W - 40);
    lv_obj_align(simonHintLbl, LV_ALIGN_BOTTOM_MID, 0, -70);

    hline(scrSimon, H - 40, C_AMB_DIM, 2);
    lv_obj_t *ftr = lv_label_create(scrSimon);
    lv_label_set_text(ftr, "USE THE COLORED BUTTONS");
    lv_obj_set_style_text_font(ftr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ftr, C_AMB_DIM, 0);
    lv_obj_align(ftr, LV_ALIGN_BOTTOM_MID, 0, -14);
}

void launchSimonGame(int rounds) {
    startSimonGame(rounds);

    // Suppress the default press-tone handler while the game runs — its
    // per-button tones are the same ones used as pattern cues, so stray
    // presses during playback would corrupt the audio pattern. Restored
    // by loop() when the game returns to the prop screen.
    btnHandler = [](BtnColor) {};

    simonAllPadsOff();
    ledAllOff();
    simonSetRoundLabel();
    lv_label_set_text(simonStatus, "SLICING SECURITY ICE");
    lv_obj_set_style_text_color(simonStatus, C_AMB, 0);
    lv_label_set_text(simonHintLbl, "WATCH THE SEQUENCE");
    lv_obj_set_style_text_color(simonHintLbl, C_AMB, 0);

    // Flush any stray button edges so old presses don't count as input
    for (int i = 0; i < 3; i++) btnState[i].pressed = false;

    lv_scr_load_anim(scrSimon, LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false);
}

static void simonFail() {
    simonActive = false;
    simonWon = false;
    simonPhase = SP_DONE;
    simonEndTime = millis();
    simonAllPadsOff();
    ledAllOff();
    buzzerFail();
    lv_label_set_text(simonStatus, "SEQUENCE REJECTED");
    lv_obj_set_style_text_color(simonStatus, C_AMB_DIM, 0);
    lv_label_set_text(simonHintLbl, "SYSTEM LOCKED OUT");
    lv_obj_set_style_text_color(simonHintLbl, C_AMB_DIM, 0);
}

static void simonWin() {
    simonActive = false;
    simonWon = true;
    simonPhase = SP_DONE;
    simonEndTime = millis();
    // Victory flash: all pads + button LEDs bright (cleared on return to prop)
    for (int i = 0; i < 3; i++) { simonPadSet(i, true); ledOn((BtnColor)i); }
    buzzerSuccess();
    lv_label_set_text(simonStatus, "LOCK BREACHED");
    lv_obj_set_style_text_color(simonStatus, C_AMB_BRT, 0);
    lv_label_set_text(simonHintLbl, "ACCESS GRANTED");
    lv_obj_set_style_text_color(simonHintLbl, C_AMB_BRT, 0);
}

// Advance the game one frame. Called from loop() while scrSimon is active.
void updateSimonGame() {
    if (!simonActive) return;
    unsigned long now = millis();

    switch (simonPhase) {

    case SP_PRE:
        // Short pause, then start replaying the sequence
        if (now - simonPhaseTime >= SIMON_START_MS) {
            simonPlayIdx = 0;
            simonLedOn = false;
            simonPhase = SP_PLAY;
            simonPhaseTime = now;
            lv_label_set_text(simonHintLbl, "WATCH THE SEQUENCE");
            lv_obj_set_style_text_color(simonHintLbl, C_AMB, 0);
        }
        break;

    case SP_PLAY:
        // simonPhaseTime marks the last on/off transition. A lit step lasts
        // SIMON_ON_MS; the dark gap between steps lasts SIMON_OFF_MS.
        if (simonLedOn) {
            if (now - simonPhaseTime >= SIMON_ON_MS) {
                simonShow(simonSeq[simonPlayIdx], false);
                simonPlayIdx++;
                simonLedOn = false;
                simonPhaseTime = now;
            }
        } else if (now - simonPhaseTime >= SIMON_OFF_MS) {
            if (simonPlayIdx >= simonLen) {
                // Sequence finished — hand off to the player
                simonAllPadsOff();
                simonInputPos = 0;
                simonInputTime = now;
                simonPhase = SP_INPUT;
                for (int i = 0; i < 3; i++) btnState[i].pressed = false;  // flush
                lv_label_set_text(simonHintLbl, "REPEAT THE SEQUENCE");
                lv_obj_set_style_text_color(simonHintLbl, C_AMB_BRT, 0);
                break;
            }
            int c = simonSeq[simonPlayIdx];
            simonShow(c, true);
            buzzerTone(SIMON_TONE[c], SIMON_ON_MS - 40);
            simonLedOn = true;
            simonPhaseTime = now;
        }
        break;

    case SP_INPUT: {
        BtnColor pressed;
        if (buttonAnyConsume(&pressed)) {
            simonInputTime = now;
            int expected = simonSeq[simonInputPos];
            // Visual + audible feedback for the press
            simonPadSet(pressed, true);
            ledPulse(pressed, 160);
            if ((int)pressed == expected) {
                buzzerTone(SIMON_TONE[pressed], 120);
                simonInputPos++;
                // brief pad flash off scheduled implicitly by ledPulse; clear pad next frame
                if (simonInputPos >= simonLen) {
                    // Round cleared
                    if (simonLen >= simonRounds) {
                        simonWin();
                    } else {
                        simonLen++;
                        simonSetRoundLabel();
                        simonPhase = SP_ROUND_OK;
                        simonPhaseTime = now;
                        buzzerTone(1046, 90);
                        lv_label_set_text(simonHintLbl, "SEQUENCE ACCEPTED");
                        lv_obj_set_style_text_color(simonHintLbl, C_AMB_BRT, 0);
                    }
                }
            } else {
                simonFail();
            }
        } else {
            // Clear any pad lit by a press once its LED pulse has expired
            for (int i = 0; i < 3; i++)
                if (!btnState[i].ledHeld && btnState[i].ledUntil == 0)
                    simonPadSet(i, false);
            // Input timeout
            if (now - simonInputTime >= SIMON_INPUT_MS) simonFail();
        }
        break;
    }

    case SP_ROUND_OK:
        simonAllPadsOff();
        if (now - simonPhaseTime >= 650) {
            simonPhase = SP_PRE;
            simonPhaseTime = now;
        }
        break;

    case SP_DONE:
        break;
    }
}

// ═══════════════════════════════════════
//  CORE PURGE MINIGAME — "MEMORY DEFRAG"
//  Touchscreen whack-a-mole on a 4x4 grid of memory blocks. Corrupted
//  blocks (bright amber ERR) flash up briefly — tap them before they
//  vanish. Protected SYS blocks appear as decoys; tapping one is a fault,
//  and PURGE_MAX_STRIKES faults locks you out. Purge the target count
//  before the timer expires to win.
//
//  Targets / time limit come from the prop's start_purge JSON
//  ("targets", "time_limit"), with defaults below.
// ═══════════════════════════════════════
#define PURGE_COLS    4
#define PURGE_ROWS    4
#define PURGE_CELLS   (PURGE_COLS * PURGE_ROWS)
#define PURGE_DEFAULT_TARGETS 12
#define PURGE_DEFAULT_TIME_S  35
#define PURGE_MAX_STRIKES     3
#define PURGE_SPAWN_MS  750    // interval between block spawns
#define PURGE_LIFE_MS   1400   // how long a block stays tappable
#define PURGE_DECOY_PCT 25     // % of spawns that are protected SYS blocks
#define PURGE_MAX_ALIVE 3      // max blocks on screen at once

enum PurgeCellState : uint8_t { PC_EMPTY, PC_CORRUPT, PC_DECOY };

PurgeCellState purgeCell[PURGE_CELLS];
unsigned long  purgeCellDie[PURGE_CELLS];   // millis when this block expires
int  purgeTargets = PURGE_DEFAULT_TARGETS;
int  purgeTimeS   = PURGE_DEFAULT_TIME_S;
int  purgePurged  = 0;
int  purgeStrikes = 0;
bool purgeActive  = false;
bool purgeWon     = false;
unsigned long purgeStartTime = 0;
unsigned long purgeSpawnTime = 0;
unsigned long purgeEndTime   = 0;   // millis() when the game ended (0 = still running)

// LVGL objects
lv_obj_t *purgeStatus   = NULL;
lv_obj_t *purgeProgLbl  = NULL;
lv_obj_t *purgeFaultLbl = NULL;
lv_obj_t *purgeTimerLbl = NULL;
lv_obj_t *purgeHintLbl  = NULL;
lv_obj_t *purgeCellObj[PURGE_CELLS];
lv_obj_t *purgeCellLbl[PURGE_CELLS];

static void purgeSetCell(int i, PurgeCellState st) {
    purgeCell[i] = st;
    switch (st) {
    case PC_CORRUPT:
        lv_obj_set_style_bg_color(purgeCellObj[i], C_AMB, 0);
        lv_obj_set_style_border_color(purgeCellObj[i], C_AMB_BRT, 0);
        lv_label_set_text(purgeCellLbl[i], "ERR");
        lv_obj_set_style_text_color(purgeCellLbl[i], C_BG, 0);
        break;
    case PC_DECOY:
        lv_obj_set_style_bg_color(purgeCellObj[i], C_PNL2, 0);
        lv_obj_set_style_border_color(purgeCellObj[i], C_DIM, 0);
        lv_label_set_text(purgeCellLbl[i], "SYS");
        lv_obj_set_style_text_color(purgeCellLbl[i], C_DIM, 0);
        break;
    default:
        lv_obj_set_style_bg_color(purgeCellObj[i], C_PNL, 0);
        lv_obj_set_style_border_color(purgeCellObj[i], C_FRM, 0);
        lv_label_set_text(purgeCellLbl[i], "");
        break;
    }
}

static void purgeClearGrid() {
    for (int i = 0; i < PURGE_CELLS; i++) { purgeSetCell(i, PC_EMPTY); purgeCellDie[i] = 0; }
}

static void purgeRefreshLabels() {
    char buf[24];
    snprintf(buf, sizeof(buf), "PURGED %d/%d", purgePurged, purgeTargets);
    lv_label_set_text(purgeProgLbl, buf);
    snprintf(buf, sizeof(buf), "FAULTS %d/%d", purgeStrikes, PURGE_MAX_STRIKES);
    lv_label_set_text(purgeFaultLbl, buf);
}

static void purgeWin() {
    purgeActive = false;
    purgeWon = true;
    purgeEndTime = millis();
    purgeClearGrid();
    buzzerSuccess();
    lv_label_set_text(purgeStatus, "CORE STABILIZED");
    lv_obj_set_style_text_color(purgeStatus, C_AMB_BRT, 0);
    lv_label_set_text(purgeHintLbl, "ACCESS GRANTED");
    lv_obj_set_style_text_color(purgeHintLbl, C_AMB_BRT, 0);
}

static void purgeFail(const char *why) {
    purgeActive = false;
    purgeWon = false;
    purgeEndTime = millis();
    purgeClearGrid();
    buzzerFail();
    lv_label_set_text(purgeStatus, why);
    lv_obj_set_style_text_color(purgeStatus, C_AMB_DIM, 0);
    lv_label_set_text(purgeHintLbl, "SYSTEM LOCKED OUT");
    lv_obj_set_style_text_color(purgeHintLbl, C_AMB_DIM, 0);
}

static void ev_purge_cell(lv_event_t *e) {
    if (!purgeActive) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);

    if (purgeCell[i] == PC_CORRUPT) {
        purgeSetCell(i, PC_EMPTY);
        purgePurged++;
        buzzerTone(1800, 30);
        purgeRefreshLabels();
        if (purgePurged >= purgeTargets) purgeWin();
    } else if (purgeCell[i] == PC_DECOY) {
        purgeSetCell(i, PC_EMPTY);
        purgeStrikes++;
        buzzerTone(300, 120);
        purgeRefreshLabels();
        if (purgeStrikes >= PURGE_MAX_STRIKES) purgeFail("SYS FILES DAMAGED");
    }
    // Tapping an empty cell does nothing — decoys are the mash deterrent
}

void buildPurgeScreen() {
    scrPurge = lv_obj_create(NULL);
    lv_obj_add_style(scrPurge, &s_scr, 0);
    lv_obj_clear_flag(scrPurge, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scrPurge);
    lv_label_set_text(title, "CORE PURGE");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, C_AMB_BRT, 0);
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    purgeStatus = lv_label_create(scrPurge);
    lv_label_set_text(purgeStatus, "MEMORY DEFRAG IN PROGRESS");
    lv_obj_set_style_text_font(purgeStatus, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(purgeStatus, C_AMB, 0);
    lv_obj_align(purgeStatus, LV_ALIGN_TOP_MID, 0, 48);

    // Progress / faults / timer row
    purgeProgLbl = lv_label_create(scrPurge);
    lv_obj_set_style_text_font(purgeProgLbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(purgeProgLbl, C_AMB, 0);
    lv_obj_align(purgeProgLbl, LV_ALIGN_TOP_LEFT, 20, 76);

    purgeFaultLbl = lv_label_create(scrPurge);
    lv_obj_set_style_text_font(purgeFaultLbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(purgeFaultLbl, C_DIM, 0);
    lv_obj_align(purgeFaultLbl, LV_ALIGN_TOP_MID, 0, 76);

    purgeTimerLbl = lv_label_create(scrPurge);
    lv_obj_set_style_text_font(purgeTimerLbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(purgeTimerLbl, C_AMB, 0);
    lv_obj_align(purgeTimerLbl, LV_ALIGN_TOP_RIGHT, -20, 76);

    hline(scrPurge, 104, C_FRM, 1);

    // 4x4 grid of memory blocks
    const int cellSz = 64, gap = 8;
    const int gridW = PURGE_COLS * cellSz + (PURGE_COLS - 1) * gap;
    const int x0 = (W - gridW) / 2;
    const int y0 = 122;
    for (int i = 0; i < PURGE_CELLS; i++) {
        int cx = x0 + (i % PURGE_COLS) * (cellSz + gap);
        int cy = y0 + (i / PURGE_COLS) * (cellSz + gap);
        lv_obj_t *cell = lv_obj_create(scrPurge);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, cellSz, cellSz);
        lv_obj_set_pos(cell, cx, cy);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cell, 2, 0);
        lv_obj_set_style_radius(cell, 4, 0);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(cell, ev_purge_cell, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        purgeCellObj[i] = cell;

        lv_obj_t *lbl = lv_label_create(cell);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_center(lbl);
        purgeCellLbl[i] = lbl;

        purgeSetCell(i, PC_EMPTY);
    }

    purgeHintLbl = lv_label_create(scrPurge);
    lv_label_set_text(purgeHintLbl, "TAP ERR BLOCKS -- AVOID SYS FILES");
    lv_obj_set_style_text_font(purgeHintLbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(purgeHintLbl, C_AMB, 0);
    lv_obj_set_style_text_align(purgeHintLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(purgeHintLbl, W - 40);
    lv_obj_align(purgeHintLbl, LV_ALIGN_BOTTOM_MID, 0, -56);

    hline(scrPurge, H - 40, C_AMB_DIM, 2);
    lv_obj_t *ftr = lv_label_create(scrPurge);
    lv_label_set_text(ftr, "MEMORY CORE DIAGNOSTIC");
    lv_obj_set_style_text_font(ftr, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ftr, C_AMB_DIM, 0);
    lv_obj_align(ftr, LV_ALIGN_BOTTOM_MID, 0, -14);
}

void launchPurgeGame(int targets, int timeS) {
    purgeTargets = constrain(targets, 1, 99);
    purgeTimeS   = constrain(timeS, 5, 300);
    purgePurged  = 0;
    purgeStrikes = 0;
    purgeActive  = true;
    purgeWon     = false;
    purgeEndTime = 0;
    purgeStartTime = millis();
    purgeSpawnTime = millis();

    purgeClearGrid();
    purgeRefreshLabels();
    char tbuf[16];
    snprintf(tbuf, sizeof(tbuf), "TIME %ds", purgeTimeS);
    lv_label_set_text(purgeTimerLbl, tbuf);
    lv_label_set_text(purgeStatus, "MEMORY DEFRAG IN PROGRESS");
    lv_obj_set_style_text_color(purgeStatus, C_AMB, 0);
    lv_label_set_text(purgeHintLbl, "TAP ERR BLOCKS -- AVOID SYS FILES");
    lv_obj_set_style_text_color(purgeHintLbl, C_AMB, 0);

    lv_scr_load_anim(scrPurge, LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false);
}

// Advance the game one frame. Called from loop() while scrPurge is active.
void updatePurgeGame() {
    if (!purgeActive) return;
    unsigned long now = millis();

    // Countdown
    int remaining = purgeTimeS - (int)((now - purgeStartTime) / 1000);
    if (remaining < 0) remaining = 0;
    char tbuf[16];
    snprintf(tbuf, sizeof(tbuf), "TIME %ds", remaining);
    lv_label_set_text(purgeTimerLbl, tbuf);
    if (remaining == 0) { purgeFail("PURGE TIMED OUT"); return; }

    // Expire blocks that outlived their window
    int alive = 0;
    for (int i = 0; i < PURGE_CELLS; i++) {
        if (purgeCell[i] == PC_EMPTY) continue;
        if (now >= purgeCellDie[i]) purgeSetCell(i, PC_EMPTY);
        else alive++;
    }

    // Spawn a new block
    if (alive < PURGE_MAX_ALIVE && now - purgeSpawnTime >= PURGE_SPAWN_MS) {
        purgeSpawnTime = now;
        for (int tries = 0; tries < 8; tries++) {
            int i = esp_random() % PURGE_CELLS;
            if (purgeCell[i] != PC_EMPTY) continue;
            bool decoy = (esp_random() % 100) < PURGE_DECOY_PCT;
            purgeSetCell(i, decoy ? PC_DECOY : PC_CORRUPT);
            purgeCellDie[i] = now + PURGE_LIFE_MS;
            break;
        }
    }
}

static void ev_prop_slice(lv_event_t *e) {
    // Request minigame start from prop
    String resp = postInteract("start_minigame");
    if (resp.length() == 0) return;

    // Parse difficulty from response
    JsonDocument doc;
    deserializeJson(doc, resp);
    int diff = doc["difficulty"] | 2;

    launchSliceGame(diff);
}

static void ev_prop_simon(lv_event_t *e) {
    // Request Simon Says (pattern lock) start from the panel.
    // The panel supplies the round count in its JSON; default to 3.
    String resp = postInteract("start_simon");
    if (resp.length() == 0) return;

    JsonDocument doc;
    deserializeJson(doc, resp);
    int rounds = doc["rounds"] | SIMON_DEFAULT_ROUNDS;

    launchSimonGame(rounds);
}

static void ev_prop_purge(lv_event_t *e) {
    // Request Core Purge (memory defrag) start from the prop.
    // The prop supplies targets + time limit in its JSON.
    String resp = postInteract("start_purge");
    if (resp.length() == 0) return;

    JsonDocument doc;
    deserializeJson(doc, resp);
    int targets = doc["targets"]    | PURGE_DEFAULT_TARGETS;
    int timeS   = doc["time_limit"] | PURGE_DEFAULT_TIME_S;

    launchPurgeGame(targets, timeS);
}

static void ev_prop_logs(lv_event_t *e) {
    String resp = postInteract("read_logs");
    if (resp.length() == 0) return;

    JsonDocument doc;
    deserializeJson(doc, resp);

    lv_obj_clean(propContent);

    // Title
    const char* title = doc["title"] | "DATA LOG";
    const char* cls = doc["classification"] | "";
    const char* content = doc["content"] | "";

    lv_obj_t *t = lv_label_create(propContent);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(t, C_AMB_BRT, 0);
    lv_obj_set_pos(t, 10, 4);

    if (strlen(cls) > 0) {
        lv_obj_t *c = lv_label_create(propContent);
        lv_label_set_text(c, cls);
        lv_obj_set_style_text_font(c, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(c, C_DIM, 0);
        lv_obj_set_pos(c, 10, 30);
    }

    // Separator
    lv_obj_t *sep = lv_obj_create(propContent);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, W - 60, 1); lv_obj_set_pos(sep, 10, 48);
    lv_obj_set_style_bg_color(sep, C_FRM, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

    // Content
    lv_obj_t *ct = lv_label_create(propContent);
    lv_label_set_text(ct, content);
    lv_obj_set_style_text_font(ct, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ct, C_AMB, 0);
    lv_obj_set_style_text_line_space(ct, 4, 0);
    lv_obj_set_width(ct, W - 60);
    lv_obj_set_pos(ct, 10, 56);
}

void renderPropDialogue(JsonDocument &doc);   // forward decl

void showPropGreeting() {
    S.println("[PROP] Fetching greeting...");
    String resp = postInteract("greet");
    S.printf("[PROP] Response len=%d\n", resp.length());
    if (resp.length() > 0) S.println(resp.substring(0, 200).c_str());
    if (resp.length() == 0) {
        lv_obj_t *err = lv_label_create(propContent);
        lv_label_set_text(err, "CONNECTION LOST");
        lv_obj_set_style_text_font(err, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(err, C_AMB_DIM, 0);
        lv_obj_set_pos(err, 10, 10);
        return;
    }

    JsonDocument doc;
    deserializeJson(doc, resp);
    renderPropDialogue(doc);
}

// Generic dialogue actions (anything that isn't a minigame/logs/disconnect)
// keep their action string here; buttons carry an index into this table.
static char propChoiceActions[8][24];
static int  propChoiceCount = 0;

static void ev_prop_action(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= propChoiceCount) return;
    char action[24];
    strlcpy(action, propChoiceActions[idx], sizeof(action));

    String resp = postInteract(action);
    if (resp.length() == 0) return;

    JsonDocument doc;
    deserializeJson(doc, resp);

    // Props may attach a game event to a dialogue response (mission hook)
    const char *gev = doc["game_event"].as<const char*>();
    if (gev && gev[0]) queueGameEvent(gev);

    renderPropDialogue(doc);
}

// Render a dialogue response (lines + choice buttons) into propContent
void renderPropDialogue(JsonDocument &doc) {
    lv_obj_clean(propContent);
    propChoiceCount = 0;

    // Dialogue lines — advance by each label's actual rendered height so
    // wrapped multi-line texts don't get drawn over by the next line.
    JsonArray lines = doc["lines"].as<JsonArray>();
    int y = 4;
    for (JsonObject line : lines) {
        const char* text = line["text"] | "";
        const char* style = line["style"] | "speech";

        lv_obj_t *l = lv_label_create(propContent);
        lv_obj_set_width(l, W - 60);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_label_set_text(l, text);
        lv_obj_set_pos(l, 10, y);

        if (strcmp(style, "narration") == 0) {
            lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(l, C_DIM, 0);
        } else if (strcmp(style, "system") == 0) {
            lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(l, C_AMB_BRT, 0);
        } else {
            lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(l, C_AMB, 0);
        }
        lv_obj_update_layout(l);
        int h = lv_obj_get_height(l);
        y += (h > 0 ? h : 20) + 6;   // measured height + small gap
    }

    // Separator before choices
    lv_obj_t *sep = lv_obj_create(propContent);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, W - 60, 1); lv_obj_set_pos(sep, 10, y + 4);
    lv_obj_set_style_bg_color(sep, C_FRM, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    y += 16;

    // Choice buttons
    JsonArray choices = doc["choices"].as<JsonArray>();
    for (JsonObject choice : choices) {
        const char* label = choice["label"] | "...";
        String actStr = choice["next_action"].as<String>();
        S.printf("[PROP] Choice: '%s' -> '%s'\n", label, actStr.c_str());

        lv_obj_t *btn = lv_btn_create(propContent);
        lv_obj_set_size(btn, W - 30, 42);
        lv_obj_set_pos(btn, 4, y);
        lv_obj_set_style_bg_color(btn, C_PNL, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(btn, C_PNL2, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, C_AMB_DIM, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 2, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);

        lv_obj_t *bl = lv_label_create(btn);
        lv_label_set_text(bl, label);
        lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(bl, C_AMB, 0);
        lv_obj_center(bl);

        // Wire action based on response
        if (actStr == "start_minigame") {
            lv_obj_add_event_cb(btn, ev_prop_slice, LV_EVENT_CLICKED, NULL);
        } else if (actStr == "start_simon") {
            lv_obj_add_event_cb(btn, ev_prop_simon, LV_EVENT_CLICKED, NULL);
        } else if (actStr == "start_purge") {
            lv_obj_add_event_cb(btn, ev_prop_purge, LV_EVENT_CLICKED, NULL);
        } else if (actStr == "read_logs") {
            lv_obj_add_event_cb(btn, ev_prop_logs, LV_EVENT_CLICKED, NULL);
        } else if (actStr.length() > 0 && actStr != "null" && propChoiceCount < 8) {
            // Generic dialogue action (deliver_intel, send_signal, ...) —
            // posts the action and renders whatever dialogue comes back
            strlcpy(propChoiceActions[propChoiceCount], actStr.c_str(),
                    sizeof(propChoiceActions[0]));
            lv_obj_add_event_cb(btn, ev_prop_action, LV_EVENT_CLICKED,
                                (void*)(intptr_t)propChoiceCount);
            propChoiceCount++;
        } else {
            // Disconnect / null
            lv_obj_add_event_cb(btn, ev_prop_back, LV_EVENT_CLICKED, NULL);
        }

        y += 46;
    }
}

// Report a finished minigame to the panel, award points on a win, and return
// to the prop screen with a fresh greeting. Game-specific result fields
// (score, rounds, ...) go into `req` before calling.
static void finishMinigameAndReturn(JsonDocument &req, bool won) {
    req["action"] = "minigame_result";
    req["won"] = won;
    JsonObject p = req["player"].to<JsonObject>();
    p["callsign"] = callsign;

    HTTPClient http;
    http.setTimeout(5000);
    http.begin("http://192.168.4.1/api/interact");
    http.addHeader("Content-Type", "application/json");
    String body; serializeJson(req, body);
    http.POST(body);
    http.end();

    if (won) {
        score += 50;
        xp += 50;
        refreshScoreLabel();
        playerDirty = true;
        playerLastSave = 0;
    }

    lv_obj_clean(propContent);
    showPropGreeting();
    lv_scr_load_anim(scrProp, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 180, 0, false);
}

// Labels we update dynamically on connect
lv_obj_t *propNameLbl = NULL;
lv_obj_t *propSubLbl = NULL;

void buildPropScreen() {
    scrProp = lv_obj_create(NULL);
    lv_obj_add_style(scrProp, &s_scr, 0);

    // Scrollable content area for dialogue + choices (created first, behind header)
    propContent = lv_obj_create(scrProp);
    lv_obj_set_size(propContent, W - 8, H - 108);
    lv_obj_set_pos(propContent, 4, 78);
    lv_obj_set_style_bg_color(propContent, C_BG, 0);
    lv_obj_set_style_bg_opa(propContent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(propContent, 0, 0);
    lv_obj_set_style_radius(propContent, 0, 0);
    lv_obj_set_style_pad_all(propContent, 0, 0);
    lv_obj_clear_flag(propContent, LV_OBJ_FLAG_CLICKABLE);  // don't eat child clicks

    // Footer
    hline(scrProp, H - 28, C_FRM, 1);
    lv_obj_t *ftr = lv_obj_create(scrProp);
    lv_obj_set_size(ftr, W, 27); lv_obj_set_pos(ftr, 0, H - 27);
    lv_obj_set_style_bg_color(ftr, C_BG, 0);
    lv_obj_set_style_bg_opa(ftr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ftr, 0, 0);
    lv_obj_set_style_radius(ftr, 0, 0);
    lv_obj_set_style_pad_left(ftr, 12, 0);
    lv_obj_clear_flag(ftr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *fl = lv_label_create(ftr);
    lv_label_set_text(fl, "LINK ACTIVE");
    lv_obj_set_style_text_font(fl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(fl, C_AMB_DIM, 0);
    lv_obj_align(fl, LV_ALIGN_LEFT_MID, 0, 0);

    // Header area — opaque black, covers content behind it
    lv_obj_t *hdr = lv_obj_create(scrProp);
    lv_obj_set_size(hdr, W, 74);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, C_BG, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Prop name (large, updated on connect) — positioned clear of back button
    propNameLbl = lv_label_create(hdr);
    lv_label_set_text(propNameLbl, "CONNECTING...");
    lv_obj_set_style_text_font(propNameLbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(propNameLbl, C_AMB_BRT, 0);
    lv_obj_set_pos(propNameLbl, 12, 42);

    // Sub info (faction / id)
    propSubLbl = lv_label_create(hdr);
    lv_label_set_text(propSubLbl, "");
    lv_obj_set_style_text_font(propSubLbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(propSubLbl, C_DIM, 0);
    lv_obj_set_pos(propSubLbl, 12, 60);

    hline(scrProp, 74, C_AMB_DIM, 2);
    hline(scrProp, 76, C_FRM, 1);

    // Back button — on very top
    lv_obj_t *bb = lv_btn_create(scrProp);
    lv_obj_set_size(bb, 80, 36); lv_obj_set_pos(bb, 6, 3);
    lv_obj_set_style_bg_color(bb, C_BG, 0);
    lv_obj_set_style_bg_opa(bb, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bb, C_PNL2, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(bb, C_AMB_DIM, 0);
    lv_obj_set_style_border_width(bb, 1, 0);
    lv_obj_set_style_radius(bb, 2, 0);
    lv_obj_set_style_shadow_width(bb, 0, 0);
    lv_obj_set_style_pad_all(bb, 0, 0);
    lv_obj_add_event_cb(bb, ev_prop_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bbl = lv_label_create(bb);
    lv_label_set_text(bbl, "< BACK");
    lv_obj_set_style_text_font(bbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bbl, C_AMB, 0);
    lv_obj_center(bbl);
}

void openPropScreen(int propIdx) {
    if (propIdx < 0 || propIdx >= foundCount) return;
    FoundProp &fp = foundProps[propIdx];

    // Show connecting status
    lv_obj_clean(nbList);  // clear cards
    lv_obj_clear_flag(nbSpinner, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(nbStatus, "CONNECTING...");
    lv_timer_handler();

    if (!connectToProp(fp.ssid)) {
        lv_obj_add_flag(nbSpinner, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(nbStatus, "CONNECTION FAILED");
        return;
    }

    if (!fetchPropInfo()) {
        lv_obj_add_flag(nbSpinner, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(nbStatus, "HANDSHAKE FAILED");
        disconnectProp();
        return;
    }

    propConnected = true;

    // Update prop screen labels
    lv_label_set_text(propNameLbl, propName);
    lv_label_set_text(propSubLbl, "LINK ESTABLISHED");

    // Fetch greeting NOW while WiFi is definitely connected
    lv_obj_clean(propContent);
    showPropGreeting();

    // Clear slice screen and transition
    lv_obj_add_flag(nbSpinner, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(nbStatus, "");
    lv_scr_load_anim(scrProp, LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false);
}

// ═══════════════════════════════════════
//  STUB SCREENS — Missions, Bounty, Cargo, Comms
// ═══════════════════════════════════════
static void ev_back_home(lv_event_t *e) { lv_scr_load_anim(scrHome, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 180, 0, false); }

// Generic stub builder: header + icon + title + flavor text + back button on top
lv_obj_t* buildStubScreen(const char *title, const lv_img_dsc_t *icon,
                          const char *line1, const char *line2, const char *line3) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_add_style(scr, &s_scr, 0);

    // Header bg
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, W, 46); lv_obj_set_pos(hdr, 0, 0);
    lv_obj_add_style(hdr, &s_hdr, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *tt = lv_label_create(hdr);
    lv_label_set_text(tt, title);
    lv_obj_set_style_text_font(tt, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(tt, C_AMB_BRT, 0);
    lv_obj_align(tt, LV_ALIGN_RIGHT_MID, 0, 0);
    hline(scr, 46, C_AMB_DIM, 2);
    hline(scr, 48, C_FRM, 1);

    // Large icon centered
    lv_obj_t *img = lv_img_create(scr);
    lv_img_set_src(img, icon);
    lv_img_set_zoom(img, 512); // 2x scale (256 = 1x)
    lv_obj_set_style_img_recolor(img, C_AMB_DIM, 0);
    lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_align(img, LV_ALIGN_CENTER, 0, -80);

    // Title
    lv_obj_t *t = lv_label_create(scr);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(t, C_AMB_BRT, 0);
    lv_obj_set_style_text_letter_space(t, 2, 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -20);

    // Flavor lines
    lv_obj_t *l1 = lv_label_create(scr);
    lv_label_set_text(l1, line1);
    lv_obj_set_style_text_font(l1, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l1, C_TXT, 0);
    lv_obj_set_style_text_align(l1, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l1, W - 40);
    lv_obj_align(l1, LV_ALIGN_CENTER, 0, 20);

    lv_obj_t *l2 = lv_label_create(scr);
    lv_label_set_text(l2, line2);
    lv_obj_set_style_text_font(l2, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l2, C_DIM, 0);
    lv_obj_set_style_text_align(l2, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l2, W - 40);
    lv_obj_align(l2, LV_ALIGN_CENTER, 0, 50);

    lv_obj_t *l3 = lv_label_create(scr);
    lv_label_set_text(l3, line3);
    lv_obj_set_style_text_font(l3, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l3, C_MUT, 0);
    lv_obj_set_style_text_align(l3, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l3, W - 40);
    lv_obj_align(l3, LV_ALIGN_CENTER, 0, 74);

    // Footer
    hline(scr, H - 30, C_FRM, 1);
    lv_obj_t *ftr = lv_obj_create(scr);
    lv_obj_set_size(ftr, W, 29); lv_obj_set_pos(ftr, 0, H - 29);
    lv_obj_add_style(ftr, &s_ftr, 0);
    lv_obj_clear_flag(ftr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *fl = lv_label_create(ftr);
    lv_label_set_text(fl, "CZERKA DP-47");
    lv_obj_set_style_text_font(fl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(fl, C_MUT, 0);
    lv_obj_align(fl, LV_ALIGN_RIGHT_MID, 0, 0);

    // Back button — on top of everything
    lv_obj_t *bb = lv_btn_create(scr);
    lv_obj_set_size(bb, 80, 36); lv_obj_set_pos(bb, 6, 5);
    lv_obj_set_style_bg_color(bb, C_BG, 0);
    lv_obj_set_style_bg_opa(bb, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bb, C_PNL2, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(bb, C_AMB_DIM, 0);
    lv_obj_set_style_border_width(bb, 1, 0);
    lv_obj_set_style_radius(bb, 2, 0);
    lv_obj_set_style_shadow_width(bb, 0, 0);
    lv_obj_set_style_pad_all(bb, 0, 0);
    lv_obj_add_event_cb(bb, ev_back_home, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bbl = lv_label_create(bb);
    lv_label_set_text(bbl, "< BACK");
    lv_obj_set_style_text_font(bbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bbl, C_AMB, 0);
    lv_obj_center(bbl);

    return scr;
}

lv_obj_t *msnListContainer = NULL;

// Rebuild the mission list from current state. Locked missions stay hidden
// so the endgame isn't spoiled before it unlocks.
void refreshMissionList() {
    if (!msnListContainer) return;
    lv_obj_clean(msnListContainer);

    int shown = 0;
    for (int i = 0; i < NUM_MISSIONS; i++) {
        // Status: active slot (running or complete), else unlocked-available, else hidden
        int slot = -1;
        for (int s2 = 0; s2 < MAX_ACTIVE; s2++)
            if (msnSlots[s2].def_idx == i) { slot = s2; break; }
        if (slot < 0 && !msnUnlocked[i]) continue;

        const MissionDef &m = ALL_MISSIONS[i];
        bool complete = (slot >= 0 && msnSlots[slot].complete);
        bool running  = (slot >= 0 && !complete);
        shown++;

        lv_obj_t *card = lv_obj_create(msnListContainer);
        lv_obj_remove_style_all(card);
        lv_obj_set_width(card, lv_pct(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, C_PNL, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card, complete ? C_FRM : C_AMB_DIM, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_radius(card, 2, 0);
        lv_obj_set_style_pad_all(card, 10, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        // Title + status badge
        lv_obj_t *t = lv_label_create(card);
        lv_label_set_text(t, m.title);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(t, complete ? C_DIM : C_AMB_BRT, 0);
        lv_obj_set_pos(t, 0, 0);

        lv_obj_t *st = lv_label_create(card);
        char sbuf[24];
        if (complete)      snprintf(sbuf, sizeof(sbuf), "COMPLETE");
        else if (running)  snprintf(sbuf, sizeof(sbuf), "STEP %d/%d",
                                    msnSlots[slot].current_step + 1, m.num_steps);
        else               snprintf(sbuf, sizeof(sbuf), "AVAILABLE");
        lv_label_set_text(st, sbuf);
        lv_obj_set_style_text_font(st, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(st, complete ? C_DIM : C_AMB, 0);
        lv_obj_align(st, LV_ALIGN_TOP_RIGHT, 0, 4);

        // Subtitle
        lv_obj_t *sub = lv_label_create(card);
        lv_label_set_text(sub, m.subtitle);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(sub, W - 50);
        lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(sub, C_DIM, 0);
        lv_obj_set_pos(sub, 0, 26);

        // Current objective (running missions only)
        if (running) {
            const MissionStep &stp = m.steps[msnSlots[slot].current_step];
            char obuf[176];
            snprintf(obuf, sizeof(obuf), "> %s\n%s", stp.title, stp.description);
            lv_obj_t *o = lv_label_create(card);
            lv_label_set_text(o, obuf);
            lv_label_set_long_mode(o, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(o, W - 50);
            lv_obj_set_style_text_font(o, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(o, C_AMB, 0);
            lv_obj_set_style_text_line_space(o, 3, 0);
            lv_obj_set_pos(o, 0, 58);
        }
    }

    if (shown == 0) {
        lv_obj_t *empty = lv_label_create(msnListContainer);
        lv_label_set_text(empty, "NO ACTIVE ASSIGNMENTS\n\nAwait orders from Command.");
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(empty, C_DIM, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    }
}

void buildMissionsScreen() {
    scrMissions = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scrMissions, C_BG, 0);
    lv_obj_set_style_pad_all(scrMissions, 0, 0);
    lv_obj_clear_flag(scrMissions, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scrMissions);
    lv_label_set_text(title, "MISSIONS");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, C_AMB_BRT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *sub = lv_label_create(scrMissions);
    lv_label_set_text(sub, "ACTIVE OPERATIONS");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub, C_DIM, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 40);

    hline(scrMissions, 60, C_FRM, 1);

    msnListContainer = lv_obj_create(scrMissions);
    lv_obj_set_size(msnListContainer, W - 12, H - 100);
    lv_obj_set_pos(msnListContainer, 6, 66);
    lv_obj_set_style_bg_opa(msnListContainer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(msnListContainer, 0, 0);
    lv_obj_set_style_pad_all(msnListContainer, 0, 0);
    lv_obj_set_flex_flow(msnListContainer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(msnListContainer, 8, 0);

    lv_obj_t *bb = lv_btn_create(scrMissions);
    lv_obj_set_size(bb, 80, 36); lv_obj_set_pos(bb, 6, 5);
    lv_obj_set_style_bg_color(bb, C_BG, 0);
    lv_obj_set_style_bg_opa(bb, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bb, C_PNL2, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(bb, C_AMB_DIM, 0);
    lv_obj_set_style_border_width(bb, 1, 0);
    lv_obj_set_style_radius(bb, 2, 0);
    lv_obj_set_style_shadow_width(bb, 0, 0);
    lv_obj_set_style_pad_all(bb, 0, 0);
    lv_obj_add_event_cb(bb, ev_back_home, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bbl = lv_label_create(bb);
    lv_label_set_text(bbl, "< BACK");
    lv_obj_set_style_text_font(bbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bbl, C_AMB, 0);
    lv_obj_center(bbl);

    refreshMissionList();
}

// ═══════════════════════════════════════
//  DEBRIEF — endgame victory screen
//  Shown when a mission flagged "endgame": true completes.
// ═══════════════════════════════════════
lv_obj_t *scrDebrief = NULL;

static const char* debriefRank() {
    if (score >= 900) return "GHOST OF OUTPOST 77";
    if (score >= 600) return "FIELD AGENT";
    if (score >= 300) return "OPERATIVE";
    return "RECRUIT";
}

void buzzerFanfare() {
    // Extended victory chime — one-time blocking is fine here
    buzzerTone(523, 90);  delay(100);
    buzzerTone(659, 90);  delay(100);
    buzzerTone(784, 90);  delay(100);
    buzzerTone(1046, 220); delay(240);
    buzzerTone(784, 80);  delay(90);
    buzzerTone(1046, 320);
}

void showDebrief() {
    // Built fresh each time so the stats are current
    if (scrDebrief) { lv_obj_del(scrDebrief); scrDebrief = NULL; }
    scrDebrief = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scrDebrief, C_BG, 0);
    lv_obj_clear_flag(scrDebrief, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hd = lv_label_create(scrDebrief);
    lv_label_set_text(hd, "EXTRACTION COMPLETE");
    lv_obj_set_style_text_font(hd, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hd, C_AMB, 0);
    lv_obj_set_style_text_letter_space(hd, 2, 0);
    lv_obj_align(hd, LV_ALIGN_TOP_MID, 0, 42);

    lv_obj_t *t = lv_label_create(scrDebrief);
    lv_label_set_text(t, "MISSION\nACCOMPLISHED");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(t, C_AMB_BRT, 0);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(t, 2, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 72);

    hline(scrDebrief, 148, C_AMB_DIM, 2);

    lv_obj_t *rl = lv_label_create(scrDebrief);
    lv_label_set_text(rl, "SERVICE RANK AWARDED");
    lv_obj_set_style_text_font(rl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(rl, C_DIM, 0);
    lv_obj_align(rl, LV_ALIGN_TOP_MID, 0, 166);

    lv_obj_t *rk = lv_label_create(scrDebrief);
    lv_label_set_text(rk, debriefRank());
    lv_obj_set_style_text_font(rk, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(rk, C_AMB_BRT, 0);
    lv_obj_set_style_text_letter_space(rk, 1, 0);
    lv_obj_align(rk, LV_ALIGN_TOP_MID, 0, 186);

    // Final stats
    char sbuf[120];
    snprintf(sbuf, sizeof(sbuf),
             "OPERATIVE  %s\n\nSCORE  %d %s\nXP  %d\nSCANS LOGGED  %d",
             callsign, score, scoreSuffix, xp, totalScans);
    lv_obj_t *st = lv_label_create(scrDebrief);
    lv_label_set_text(st, sbuf);
    lv_obj_set_style_text_font(st, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(st, C_AMB, 0);
    lv_obj_set_style_text_align(st, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(st, 6, 0);
    lv_obj_align(st, LV_ALIGN_TOP_MID, 0, 236);

    lv_obj_t *ft = lv_label_create(scrDebrief);
    lv_label_set_text(ft, "DEBRIEF TRANSMITTED TO\nALLIANCE COMMAND");
    lv_obj_set_style_text_font(ft, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ft, C_DIM, 0);
    lv_obj_set_style_text_align(ft, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ft, LV_ALIGN_BOTTOM_MID, 0, -78);

    lv_obj_t *btn = lv_btn_create(scrDebrief);
    lv_obj_set_size(btn, W - 60, 42);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(btn, C_PNL, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, C_PNL2, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, C_AMB_DIM, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 2, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, ev_back_home, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "RETURN TO DATAPAD");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bl, C_AMB, 0);
    lv_obj_center(bl);

    lv_scr_load_anim(scrDebrief, LV_SCR_LOAD_ANIM_FADE_ON, 400, 0, false);

    // Tell the GM (and any listening props) that this operative finished
    swts::gmTriggerEvent("game_complete", "Operative extracted", 0, callsign);
    buzzerFanfare();
}

// ═══════════════════════════════════════
//  BOUNTY SYSTEM
// ═══════════════════════════════════════
#define MAX_ACTIVE_BOUNTIES 8
#define MAX_BOUNTY_CLUES    5

struct ActiveBounty {
    char id[24];
    char target_name[24];
    char description[60];
    int  reward;
    char clues[MAX_BOUNTY_CLUES][160];
    uint8_t clueCount;
    bool inUse;
    bool closed;        // claimed by someone (not me) or cancelled
    char closedBy[24];  // empty if cancelled, otherwise claimer
    bool wonByMe;       // true → celebration shown then removed
};

ActiveBounty bountyPool[MAX_ACTIVE_BOUNTIES];

lv_obj_t *bountyListContainer = NULL;
lv_obj_t *scrBountyDetail = NULL;
ActiveBounty *currentBounty = NULL;

ActiveBounty* findBounty(const char *id) {
    for (int i = 0; i < MAX_ACTIVE_BOUNTIES; i++)
        if (bountyPool[i].inUse && strcmp(bountyPool[i].id, id) == 0) return &bountyPool[i];
    return NULL;
}

ActiveBounty* allocBounty() {
    for (int i = 0; i < MAX_ACTIVE_BOUNTIES; i++)
        if (!bountyPool[i].inUse) { memset(&bountyPool[i], 0, sizeof(ActiveBounty)); return &bountyPool[i]; }
    return NULL;
}

int activeBountyCount() {
    int n = 0;
    for (int i = 0; i < MAX_ACTIVE_BOUNTIES; i++)
        if (bountyPool[i].inUse && !bountyPool[i].closed) n++;
    return n;
}

void refreshBountyList();
void buildBountyDetailScreen();
void openBountyDetail(ActiveBounty *b);

static void ev_bounty_card(lv_event_t *e) {
    ActiveBounty *b = (ActiveBounty *)lv_event_get_user_data(e);
    if (b) openBountyDetail(b);
}

static void ev_bounty_detail_back(lv_event_t *e) {
    lv_scr_load_anim(scrBounty, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 180, 0, false);
}

void buildBountyScreen() {
    scrBounty = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scrBounty, C_BG, 0);
    lv_obj_set_style_pad_all(scrBounty, 0, 0);
    lv_obj_clear_flag(scrBounty, LV_OBJ_FLAG_SCROLLABLE);

    // Title bar
    lv_obj_t *title = lv_label_create(scrBounty);
    lv_label_set_text(title, "WANTED");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, C_AMB_BRT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *sub = lv_label_create(scrBounty);
    lv_label_set_text(sub, "ACTIVE TARGETS");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub, C_DIM, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 40);

    // Divider
    hline(scrBounty, 60, C_FRM, 1);

    // List container — scrollable
    bountyListContainer = lv_obj_create(scrBounty);
    lv_obj_set_size(bountyListContainer, W - 12, H - 100);
    lv_obj_set_pos(bountyListContainer, 6, 66);
    lv_obj_set_style_bg_opa(bountyListContainer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bountyListContainer, 0, 0);
    lv_obj_set_style_pad_all(bountyListContainer, 0, 0);
    lv_obj_set_flex_flow(bountyListContainer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(bountyListContainer, 8, 0);

    // Back button
    lv_obj_t *bb = lv_btn_create(scrBounty);
    lv_obj_set_size(bb, 80, 36); lv_obj_set_pos(bb, 6, 5);
    lv_obj_set_style_bg_color(bb, C_BG, 0);
    lv_obj_set_style_bg_opa(bb, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bb, C_PNL2, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(bb, C_AMB_DIM, 0);
    lv_obj_set_style_border_width(bb, 1, 0);
    lv_obj_set_style_radius(bb, 2, 0);
    lv_obj_set_style_shadow_width(bb, 0, 0);
    lv_obj_set_style_pad_all(bb, 0, 0);
    lv_obj_add_event_cb(bb, ev_back_home, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bbl = lv_label_create(bb);
    lv_label_set_text(bbl, "< BACK");
    lv_obj_set_style_text_font(bbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bbl, C_AMB, 0);
    lv_obj_center(bbl);

    refreshBountyList();
}

void refreshBountyList() {
    refreshHomeBadges();
    if (!bountyListContainer) return;
    lv_obj_clean(bountyListContainer);

    int active = 0;
    for (int i = 0; i < MAX_ACTIVE_BOUNTIES; i++) {
        if (!bountyPool[i].inUse) continue;
        ActiveBounty *b = &bountyPool[i];

        lv_obj_t *card = lv_btn_create(bountyListContainer);
        lv_obj_set_size(card, lv_pct(100), 92);
        lv_obj_set_style_bg_color(card, C_PNL, 0);
        lv_obj_set_style_bg_color(card, C_PNL2, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(card, b->closed ? C_DIM : C_AMB, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_radius(card, 2, 0);
        lv_obj_set_style_pad_all(card, 8, 0);
        lv_obj_set_style_shadow_width(card, 0, 0);
        lv_obj_add_event_cb(card, ev_bounty_card, LV_EVENT_CLICKED, b);

        lv_obj_t *st = lv_label_create(card);
        lv_label_set_text(st, b->closed ? "CLOSED" : "WANTED");
        lv_obj_set_style_text_color(st, b->closed ? C_DIM : C_AMB_BRT, 0);
        lv_obj_set_style_text_font(st, &lv_font_montserrat_12, 0);
        lv_obj_set_pos(st, 0, 0);

        lv_obj_t *nm = lv_label_create(card);
        lv_label_set_text(nm, b->target_name);
        lv_obj_set_style_text_color(nm, C_AMB_BRT, 0);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_18, 0);
        lv_obj_set_pos(nm, 0, 16);

        char rw[40]; snprintf(rw, sizeof(rw), "%d %s   //   %d CLUE%s", b->reward, scoreSuffix, b->clueCount, b->clueCount == 1 ? "" : "S");
        lv_obj_t *ds = lv_label_create(card);
        lv_label_set_text(ds, rw);
        lv_obj_set_style_text_color(ds, C_DIM, 0);
        lv_obj_set_style_text_font(ds, &lv_font_montserrat_12, 0);
        lv_obj_set_pos(ds, 0, 44);

        if (b->closed && b->closedBy[0]) {
            char wb[60]; snprintf(wb, sizeof(wb), "CLAIMED BY %s", b->closedBy);
            lv_obj_t *cl = lv_label_create(card);
            lv_label_set_text(cl, wb);
            lv_obj_set_style_text_color(cl, C_DIM, 0);
            lv_obj_set_style_text_font(cl, &lv_font_montserrat_12, 0);
            lv_obj_set_pos(cl, 0, 62);
        } else {
            lv_obj_t *tap = lv_label_create(card);
            lv_label_set_text(tap, "TAP FOR DETAILS >");
            lv_obj_set_style_text_color(tap, C_AMB_DIM, 0);
            lv_obj_set_style_text_font(tap, &lv_font_montserrat_12, 0);
            lv_obj_set_pos(tap, 0, 62);
        }
        active++;
    }

    if (active == 0) {
        lv_obj_t *e = lv_label_create(bountyListContainer);
        lv_label_set_text(e, "NO ACTIVE BOUNTIES\n\nMonitor mission control\nfor new wanted notices.");
        lv_obj_set_style_text_align(e, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(e, C_DIM, 0);
        lv_obj_set_style_text_font(e, &lv_font_montserrat_14, 0);
        lv_obj_set_width(e, lv_pct(100));
    }
}

void openBountyDetail(ActiveBounty *b) {
    currentBounty = b;
    if (scrBountyDetail) { lv_obj_del(scrBountyDetail); scrBountyDetail = NULL; }
    buildBountyDetailScreen();
    lv_scr_load_anim(scrBountyDetail, LV_SCR_LOAD_ANIM_MOVE_LEFT, 180, 0, false);
}

void buildBountyDetailScreen() {
    if (!currentBounty) return;
    ActiveBounty *b = currentBounty;

    scrBountyDetail = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scrBountyDetail, C_BG, 0);
    lv_obj_set_style_pad_all(scrBountyDetail, 0, 0);
    lv_obj_clear_flag(scrBountyDetail, LV_OBJ_FLAG_SCROLLABLE);

    // Header
    lv_obj_t *wanted = lv_label_create(scrBountyDetail);
    lv_label_set_text(wanted, "WANTED");
    lv_obj_set_style_text_color(wanted, C_AMB_BRT, 0);
    lv_obj_set_style_text_font(wanted, &lv_font_montserrat_24, 0);
    lv_obj_align(wanted, LV_ALIGN_TOP_MID, 0, 12);

    hline(scrBountyDetail, 60, C_FRM, 1);

    // Target name
    lv_obj_t *nm = lv_label_create(scrBountyDetail);
    lv_label_set_text(nm, b->target_name);
    lv_obj_set_style_text_color(nm, C_AMB_BRT, 0);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(nm, W - 20);
    lv_obj_align(nm, LV_ALIGN_TOP_MID, 0, 72);

    // Description
    lv_obj_t *ds = lv_label_create(scrBountyDetail);
    lv_label_set_text(ds, b->description);
    lv_obj_set_style_text_color(ds, C_TXT, 0);
    lv_obj_set_style_text_font(ds, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(ds, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ds, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(ds, W - 30);
    lv_obj_align(ds, LV_ALIGN_TOP_MID, 0, 108);

    // Reward
    char rb[40]; snprintf(rb, sizeof(rb), "REWARD: %d %s", b->reward, scoreSuffix);
    lv_obj_t *rw = lv_label_create(scrBountyDetail);
    lv_label_set_text(rw, rb);
    lv_obj_set_style_text_color(rw, C_AMB_BRT, 0);
    lv_obj_set_style_text_font(rw, &lv_font_montserrat_18, 0);
    lv_obj_align(rw, LV_ALIGN_TOP_MID, 0, 168);

    // Status
    const char *statTxt;
    lv_color_t statCol;
    if (b->closed && b->closedBy[0]) { statTxt = "STATUS: CLAIMED"; statCol = C_DIM; }
    else if (b->closed)              { statTxt = "STATUS: CLOSED";  statCol = C_DIM; }
    else                             { statTxt = "STATUS: ACTIVE";  statCol = C_AMB; }
    lv_obj_t *stl = lv_label_create(scrBountyDetail);
    lv_label_set_text(stl, statTxt);
    lv_obj_set_style_text_color(stl, statCol, 0);
    lv_obj_set_style_text_font(stl, &lv_font_montserrat_14, 0);
    lv_obj_align(stl, LV_ALIGN_TOP_MID, 0, 196);

    hline(scrBountyDetail, 222, C_FRM, 1);

    // Clues label
    char ch[24]; snprintf(ch, sizeof(ch), "CLUES (%d)", b->clueCount);
    lv_obj_t *ch1 = lv_label_create(scrBountyDetail);
    lv_label_set_text(ch1, ch);
    lv_obj_set_style_text_color(ch1, C_AMB, 0);
    lv_obj_set_style_text_font(ch1, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(ch1, 12, 230);

    // Clue scroll area
    lv_obj_t *cont = lv_obj_create(scrBountyDetail);
    lv_obj_set_size(cont, W - 12, 196);
    lv_obj_set_pos(cont, 6, 252);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 4, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(cont, 6, 0);

    if (b->clueCount == 0) {
        lv_obj_t *e = lv_label_create(cont);
        lv_label_set_text(e, "Awaiting intel from\nmission control...");
        lv_obj_set_style_text_align(e, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(e, C_DIM, 0);
        lv_obj_set_style_text_font(e, &lv_font_montserrat_14, 0);
        lv_obj_set_width(e, lv_pct(100));
    }

    for (int i = 0; i < b->clueCount; i++) {
        lv_obj_t *row = lv_obj_create(cont);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, C_PNL, 0);
        lv_obj_set_style_border_color(row, C_AMB_DIM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_radius(row, 1, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        char hd[12]; snprintf(hd, sizeof(hd), "// %d", i + 1);
        lv_obj_t *h = lv_label_create(row);
        lv_label_set_text(h, hd);
        lv_obj_set_style_text_color(h, C_AMB, 0);
        lv_obj_set_style_text_font(h, &lv_font_montserrat_12, 0);
        lv_obj_set_pos(h, 0, 0);

        lv_obj_t *t = lv_label_create(row);
        lv_label_set_text(t, b->clues[i]);
        lv_obj_set_style_text_color(t, C_TXT, 0);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_14, 0);
        lv_obj_set_width(t, lv_pct(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(t, 0, 16);
    }

    // Back button
    lv_obj_t *bb = lv_btn_create(scrBountyDetail);
    lv_obj_set_size(bb, 80, 36); lv_obj_set_pos(bb, 6, 5);
    lv_obj_set_style_bg_color(bb, C_BG, 0);
    lv_obj_set_style_bg_opa(bb, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bb, C_PNL2, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(bb, C_AMB_DIM, 0);
    lv_obj_set_style_border_width(bb, 1, 0);
    lv_obj_set_style_radius(bb, 2, 0);
    lv_obj_set_style_shadow_width(bb, 0, 0);
    lv_obj_set_style_pad_all(bb, 0, 0);
    lv_obj_add_event_cb(bb, ev_bounty_detail_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bbl = lv_label_create(bb);
    lv_label_set_text(bbl, "< BACK");
    lv_obj_set_style_text_font(bbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bbl, C_AMB, 0);
    lv_obj_center(bbl);
}

// ═══════════════════════════════════════
//  CARGO INTEL SCREEN — NFC reader for CARGO_## crate tags
// ═══════════════════════════════════════
static void ev_cg_back(lv_event_t *e) { lv_scr_load_anim(scrHome, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 180, 0, false); }

void buildCargoScreen() {
    scrCargo = lv_obj_create(NULL);
    lv_obj_add_style(scrCargo, &s_scr, 0);
    makeSubHeader(scrCargo, "CARGO INTEL", ev_cg_back);

    cgSpinner = lv_spinner_create(scrCargo, 1000, 60);
    lv_obj_set_size(cgSpinner, 140, 140);
    lv_obj_align(cgSpinner, LV_ALIGN_CENTER, 0, -50);
    lv_obj_set_style_arc_color(cgSpinner, C_AMB, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(cgSpinner, C_FRM, LV_PART_MAIN);
    lv_obj_set_style_arc_width(cgSpinner, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(cgSpinner, 3, LV_PART_MAIN);

    lv_obj_t *dot = lv_obj_create(scrCargo);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 16, 16);
    lv_obj_align(dot, LV_ALIGN_CENTER, 0, -50);
    lv_obj_set_style_bg_color(dot, C_AMB_BRT, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);

    cgPrompt = lv_label_create(scrCargo);
    lv_label_set_text(cgPrompt, "PRESENT CRATE TAG\nTO READER PORT");
    lv_obj_set_style_text_font(cgPrompt, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(cgPrompt, C_TXT, 0);
    lv_obj_set_style_text_align(cgPrompt, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(cgPrompt, 1, 0);
    lv_obj_align(cgPrompt, LV_ALIGN_CENTER, 0, 60);

    lv_obj_t *sub = lv_label_create(scrCargo);
    lv_label_set_text(sub, "MANIFEST SCANNER STANDING BY");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sub, C_DIM, 0);
    lv_obj_set_style_text_letter_space(sub, 2, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 100);

    cgResult = lv_obj_create(scrCargo);
    lv_obj_set_size(cgResult, W - 16, 240);
    lv_obj_align(cgResult, LV_ALIGN_CENTER, 0, 30);
    lv_obj_add_style(cgResult, &s_pnl, 0);
    lv_obj_set_style_border_width(cgResult, 2, 0);
    lv_obj_add_flag(cgResult, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(cgResult, LV_OBJ_FLAG_SCROLLABLE);

    hline(scrCargo, H - 28, C_FRM, 1);
    lv_obj_t *ftr = lv_obj_create(scrCargo);
    lv_obj_set_size(ftr, W, 27); lv_obj_set_pos(ftr, 0, H - 27);
    lv_obj_add_style(ftr, &s_ftr, 0);
    lv_obj_clear_flag(ftr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *fl = lv_label_create(ftr);
    lv_label_set_text(fl, nfcOk ? "MANIFEST PORT ACTIVE" : "MANIFEST PORT OFFLINE");
    lv_obj_set_style_text_font(fl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(fl, nfcOk ? C_AMB : C_AMB_DIM, 0);
    lv_obj_align(fl, LV_ALIGN_LEFT_MID, 0, 0);
}

void showCargoResult(uint8_t *uid, uint8_t len) {
    totalScans++;
    lv_obj_add_flag(cgSpinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(cgPrompt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(cgResult, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clean(cgResult);

    // If this isn't a CARGO_* tag, redirect player to the Datacard reader.
    if (!isCargoTag(ndefText)) {
        lv_obj_set_style_border_color(cgResult, C_AMB_BRT, 0);

        lv_obj_t *hd = lv_label_create(cgResult);
        lv_label_set_text(hd, "WRONG READER");
        lv_obj_set_style_text_font(hd, &lv_font_montserrat_18, 0);
        lv_obj_set_style_text_color(hd, C_AMB_BRT, 0);
        lv_obj_set_pos(hd, 10, 8);

        lv_obj_t *nm = lv_label_create(cgResult);
        char nb[64];
        if (ndefText[0]) snprintf(nb, sizeof(nb), "Detected: %s", ndefText);
        else             snprintf(nb, sizeof(nb), "Detected: (no payload)");
        lv_label_set_text(nm, nb);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(nm, C_TXT, 0);
        lv_obj_set_pos(nm, 10, 40);

        lv_obj_t *body = lv_label_create(cgResult);
        lv_label_set_text(body, "This is not a cargo crate.\n\nReturn to home and use\nDATACARD reader.");
        lv_obj_set_style_text_font(body, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(body, C_AMB, 0);
        lv_obj_set_width(body, W - 40);
        lv_obj_set_pos(body, 10, 84);

        buzzerScanFail();
        cgScanning = false;
        return;
    }

    // Real cargo identification
    lv_obj_set_style_border_color(cgResult, C_AMB, 0);

    lv_obj_t *hd = lv_label_create(cgResult);
    lv_label_set_text(hd, "CRATE IDENTIFIED");
    lv_obj_set_style_text_font(hd, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(hd, C_AMB_BRT, 0);
    lv_obj_set_pos(hd, 10, 8);

    lv_obj_t *nm = lv_label_create(cgResult);
    lv_label_set_text(nm, ndefText);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(nm, C_AMB, 0);
    lv_obj_set_pos(nm, 10, 40);

    char uidStr[24];
    if (len == 4) snprintf(uidStr, sizeof(uidStr), "UID: %02X:%02X:%02X:%02X", uid[0], uid[1], uid[2], uid[3]);
    else snprintf(uidStr, sizeof(uidStr), "UID: %02X:%02X:%02X:%02X:%02X:%02X:%02X",
                  uid[0], uid[1], uid[2], uid[3], uid[4], uid[5], uid[6]);
    lv_obj_t *ub = lv_label_create(cgResult);
    lv_label_set_text(ub, uidStr);
    lv_obj_set_style_text_font(ub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ub, C_DIM, 0);
    lv_obj_set_pos(ub, 10, 80);

    lv_obj_t *body = lv_label_create(cgResult);
    lv_label_set_text(body, "Manifest logged.\n\nReport contents to a rebel\ncontact for a reward.");
    lv_obj_set_style_text_font(body, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(body, C_TXT, 0);
    lv_obj_set_width(body, W - 40);
    lv_obj_set_pos(body, 10, 110);

    inventoryItems++;
    refreshHomeBadges();
    buzzerScanOk();
    cgScanning = false;
}

void resetCgScan() {
    if (!cgResult) return;
    lv_obj_add_flag(cgResult, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(cgSpinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(cgPrompt, LV_OBJ_FLAG_HIDDEN);
    cgScanning = true;
}

// ═══════════════════════════════════════
//  COMMS SCREEN — in-universe messages
// ═══════════════════════════════════════
void triggerComms(const char *trigger);  // forward decl
void refreshCommList();                  // forward decl

// UI dirty flags — set from ESPNOW callback context (a different task),
// consumed in loop() so LVGL stays single-threaded.
volatile bool uiDirtyCommList = false;
volatile bool uiDirtyBountyList = false;
volatile bool uiDirtyScore = false;
volatile bool uiDirtyBountyDetail = false;

struct CommMsg {
    char id[24];
    char from[24];
    char subject[40];
    char body[256];
    char trigger[40];   // "boot", "mission_start:ghost_signal", "slice_win", "event:xxx"
    bool loaded;         // in the pool (loaded from SD)
    bool delivered;      // shown to player (in inbox)
    bool read;           // player has opened it
};

#define MAX_COMMS 16
CommMsg commPool[MAX_COMMS];   // all possible messages from SD
int commPoolCount = 0;

CommMsg *commInbox[MAX_COMMS]; // pointers to delivered messages (display order)
int commInboxCount = 0;

// ═══════════════════════════════════════
//  MISSION + NFC TRIGGER LOADER — from /SWTS/missions.json
// ═══════════════════════════════════════
bool loadMissionsFromSD() {
    NUM_MISSIONS = 0;
    NUM_TRIGGERS = 0;
    File f = SD.open("/SWTS/missions.json", FILE_READ);
    if (!f) { S.println("[MSN] /SWTS/missions.json not found"); return false; }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) { S.printf("[MSN] Parse error: %s\n", err.c_str()); return false; }

    // Missions
    for (JsonObject m : doc["missions"].as<JsonArray>()) {
        if (NUM_MISSIONS >= MAX_MISSIONS) break;
        MissionDef &d = ALL_MISSIONS[NUM_MISSIONS];
        memset(&d, 0, sizeof(d));
        strlcpy(d.id,       m["id"]       | "", sizeof(d.id));
        strlcpy(d.title,    m["title"]    | "", sizeof(d.title));
        strlcpy(d.subtitle, m["subtitle"] | "", sizeof(d.subtitle));
        strlcpy(d.faction,  m["faction"]  | "", sizeof(d.faction));
        strlcpy(d.briefing, m["briefing"] | "", sizeof(d.briefing));
        d.difficulty     = m["difficulty"]     | 1;
        d.reward_credits = m["reward_credits"] | 0;
        d.reward_xp      = m["reward_xp"]      | 0;
        strlcpy(d.reward_item, m["reward_item"] | "", sizeof(d.reward_item));
        strlcpy(d.unlocks_id,  m["unlocks"]     | "", sizeof(d.unlocks_id));
        d.starts_unlocked = m["starts_unlocked"] | false;
        d.is_endgame      = m["endgame"]         | false;

        d.num_steps = 0;
        for (JsonObject s : m["steps"].as<JsonArray>()) {
            if (d.num_steps >= MAX_MISSION_STEPS) break;
            MissionStep &st = d.steps[d.num_steps];
            memset(&st, 0, sizeof(st));
            strlcpy(st.title,       s["title"]       | "", sizeof(st.title));
            strlcpy(st.description, s["description"] | "", sizeof(st.description));
            strlcpy(st.target,      s["target"]      | "", sizeof(st.target));
            st.xp_reward = s["xp"] | 0;
            const char *type = s["type"] | "scan_nfc";
            if      (strcmp(type, "connect_prop") == 0) st.obj_type = OBJ_CONNECT_PROP;
            else if (strcmp(type, "event") == 0)        st.obj_type = OBJ_EVENT;
            else                                        st.obj_type = OBJ_SCAN_NFC;
            d.num_steps++;
        }

        // initial unlock state (player.json can override)
        msnUnlocked[NUM_MISSIONS] = d.starts_unlocked;
        NUM_MISSIONS++;
    }

    // NFC triggers
    for (JsonObject t : doc["nfc_triggers"].as<JsonArray>()) {
        if (NUM_TRIGGERS >= MAX_TRIGGERS) break;
        NfcTrigger &tr = NFC_TRIGGERS[NUM_TRIGGERS];
        memset(&tr, 0, sizeof(tr));
        strlcpy(tr.match_token, t["token"] | "", sizeof(tr.match_token));
        const char *action = t["action"] | "step";
        tr.action = (strcmp(action, "start") == 0) ? TRIG_START : TRIG_STEP;
        tr.mission_idx = (int8_t) findMissionByIdStr(t["mission"] | "");
        tr.step_idx = t["step"] | 0;
        if (tr.mission_idx < 0) {
            S.printf("[MSN] trigger %s has unknown mission %s, skipping\n",
                     tr.match_token, (const char*)(t["mission"] | ""));
            continue;
        }
        NUM_TRIGGERS++;
    }

    S.printf("[MSN] Loaded %d missions, %d triggers\n", NUM_MISSIONS, NUM_TRIGGERS);
    return NUM_MISSIONS > 0;
}

// ═══════════════════════════════════════
//  PLAYER STATE — /SWTS/player.json
//  Identity + runtime state. Survives reboot.
//  Swap the SD into a spare datapad and it becomes that player.
// ═══════════════════════════════════════
volatile bool playerDirty = false;
unsigned long playerLastSave = 0;
#define PLAYER_SAVE_DEBOUNCE_MS 1000
bool playerLoadOk = false;       // true = load succeeded OR file did not exist (safe to save)
bool playerFileExisted = false;  // true = file was present at boot

#define MAX_READ_COMMS 32
char playerReadComms[MAX_READ_COMMS][20];
int  playerReadCommCount = 0;

#define MAX_WON_BOUNTIES 16
char playerWonBounties[MAX_WON_BOUNTIES][24];
int  playerWonBountyCount = 0;

bool playerHasReadComm(const char *id) {
    for (int i = 0; i < playerReadCommCount; i++)
        if (strcmp(playerReadComms[i], id) == 0) return true;
    return false;
}
void playerMarkCommRead(const char *id) {
    if (playerHasReadComm(id)) return;
    if (playerReadCommCount >= MAX_READ_COMMS) return;
    strlcpy(playerReadComms[playerReadCommCount++], id, 20);
    playerDirty = true;
    playerLastSave = 0;  // make next save fire immediately, don't wait for debounce
}
bool playerHasWonBounty(const char *id) {
    for (int i = 0; i < playerWonBountyCount; i++)
        if (strcmp(playerWonBounties[i], id) == 0) return true;
    return false;
}
void playerMarkBountyWon(const char *id) {
    if (playerHasWonBounty(id)) return;
    if (playerWonBountyCount >= MAX_WON_BOUNTIES) return;
    strlcpy(playerWonBounties[playerWonBountyCount++], id, 24);
    playerDirty = true;
    playerLastSave = 0;
}

bool savePlayerState() {
    if (!sdOk) { S.println("[PLAYER] save SKIP: SD not mounted"); return false; }
    // Safety: if a file existed at boot but failed to parse, we won't overwrite it
    // (otherwise we'd silently destroy the user's manual edits / corrupted-but-recoverable state).
    if (playerFileExisted && !playerLoadOk) {
        S.println("[PLAYER] save BLOCKED: existing player.json failed to parse on boot");
        return false;
    }
    File f = SD.open("/SWTS/player.json", FILE_WRITE);
    if (!f) { S.println("[PLAYER] save FAIL: SD.open returned null"); return false; }

    JsonDocument doc;
    doc["callsign"]   = callsign;
    doc["score"]      = score;
    doc["xp"]         = xp;
    doc["totalScans"] = totalScans;

    JsonArray msnArr = doc["missions"].to<JsonArray>();
    for (int i = 0; i < NUM_MISSIONS; i++) {
        JsonObject mo = msnArr.add<JsonObject>();
        mo["id"]       = ALL_MISSIONS[i].id;
        mo["unlocked"] = msnUnlocked[i];
        int slot = findActive(i);
        if (slot >= 0) {
            mo["step"]     = msnSlots[slot].current_step;
            mo["complete"] = msnSlots[slot].complete;
        } else {
            // No active slot — check if previously completed by walking finished slots
            bool done = false;
            for (int s = 0; s < MAX_ACTIVE; s++)
                if (msnSlots[s].def_idx == i && msnSlots[s].complete) { done = true; break; }
            mo["complete"] = done;
        }
    }

    JsonArray cr = doc["comms_read"].to<JsonArray>();
    for (int i = 0; i < playerReadCommCount; i++) cr.add(playerReadComms[i]);

    JsonArray bw = doc["bounties_won"].to<JsonArray>();
    for (int i = 0; i < playerWonBountyCount; i++) bw.add(playerWonBounties[i]);

    size_t n = serializeJson(doc, f);
    f.close();
    playerLastSave = millis();
    S.printf("[PLAYER] saved %u bytes: cs=%s score=%d xp=%d msns=%d/read=%d/won=%d\n",
             (unsigned)n, callsign, score, xp,
             activeMissions, playerReadCommCount, playerWonBountyCount);
    return true;
}

bool loadPlayerState() {
    playerLoadOk = false;
    playerFileExisted = false;
    if (!sdOk) { S.println("[PLAYER] load SKIP: SD not mounted"); return false; }
    File f = SD.open("/SWTS/player.json", FILE_READ);
    if (!f) {
        S.println("[PLAYER] /SWTS/player.json not found — first boot, will create on first save");
        defaultCallsignFromMac(callsign, sizeof(callsign));
        playerLoadOk = true;   // safe to write fresh
        playerDirty = true;    // queue an initial save so the file is created
        playerLastSave = 0;
        return false;
    }
    playerFileExisted = true;
    size_t sz = f.size();
    S.printf("[PLAYER] reading /SWTS/player.json (%u bytes)\n", (unsigned)sz);
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        S.printf("[PLAYER] PARSE FAILED: %s — leaving file untouched, using defaults\n", err.c_str());
        return false;
    }
    playerLoadOk = true;

    strlcpy(callsign, doc["callsign"] | "OPERATIVE", sizeof(callsign));
    if (isUnassignedCallsign(callsign)) defaultCallsignFromMac(callsign, sizeof(callsign));
    score      = doc["score"]      | 0;
    xp         = doc["xp"]          | 0;
    totalScans = doc["totalScans"]  | 0;

    // Missions — restore unlock + in-progress step
    activeMissions = 0;
    for (JsonObject mo : doc["missions"].as<JsonArray>()) {
        const char *id = mo["id"] | "";
        int idx = findMissionByIdStr(id);
        if (idx < 0) continue;
        msnUnlocked[idx] = mo["unlocked"] | msnUnlocked[idx];
        bool complete = mo["complete"] | false;
        int step      = mo["step"]     | -1;
        if (complete) {
            // place in a slot as completed (so dedupe in startMission() still works)
            for (int s = 0; s < MAX_ACTIVE; s++) {
                if (msnSlots[s].def_idx == -1) {
                    msnSlots[s].def_idx = idx;
                    msnSlots[s].current_step = ALL_MISSIONS[idx].num_steps;
                    msnSlots[s].complete = true;
                    break;
                }
            }
        } else if (step >= 0 && step < ALL_MISSIONS[idx].num_steps) {
            for (int s = 0; s < MAX_ACTIVE; s++) {
                if (msnSlots[s].def_idx == -1) {
                    msnSlots[s].def_idx = idx;
                    msnSlots[s].current_step = (uint8_t)step;
                    msnSlots[s].complete = false;
                    activeMissions++;
                    break;
                }
            }
        }
    }

    playerReadCommCount = 0;
    for (const char *id : doc["comms_read"].as<JsonArray>()) {
        if (playerReadCommCount >= MAX_READ_COMMS) break;
        strlcpy(playerReadComms[playerReadCommCount++], id ? id : "", 20);
    }

    playerWonBountyCount = 0;
    for (const char *id : doc["bounties_won"].as<JsonArray>()) {
        if (playerWonBountyCount >= MAX_WON_BOUNTIES) break;
        strlcpy(playerWonBounties[playerWonBountyCount++], id ? id : "", 24);
    }

    S.printf("[PLAYER] Loaded: %s score=%d xp=%d (msns=%d, read=%d, won=%d)\n",
             callsign, score, xp, activeMissions, playerReadCommCount, playerWonBountyCount);
    return true;
}

void loadCommsFromSD() {
    commPoolCount = 0;
    commInboxCount = 0;
    File f = SD.open("/SWTS/comms.json", FILE_READ);
    if (!f) {
        S.println("[COMMS] No comms.json on SD");
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, f)) {
        S.println("[COMMS] Parse error");
        f.close();
        return;
    }
    f.close();

    for (JsonObject m : doc["messages"].as<JsonArray>()) {
        if (commPoolCount >= MAX_COMMS) break;
        CommMsg &c = commPool[commPoolCount];
        strlcpy(c.id, m["id"] | "", sizeof(c.id));
        strlcpy(c.from, m["from"] | "UNKNOWN", sizeof(c.from));
        strlcpy(c.subject, m["subject"] | "", sizeof(c.subject));
        strlcpy(c.body, m["body"] | "", sizeof(c.body));
        strlcpy(c.trigger, m["trigger"] | "manual", sizeof(c.trigger));
        c.loaded = true;
        c.delivered = false;
        c.read = false;
        commPoolCount++;
    }

    S.printf("[COMMS] Loaded %d messages from SD\n", commPoolCount);

    // Auto-deliver "boot" triggered messages
    triggerComms("boot");
}

// Deliver all messages matching a trigger string
void triggerComms(const char *trigger) {
    for (int i = 0; i < commPoolCount; i++) {
        if (commPool[i].delivered) continue;
        if (strcmp(commPool[i].trigger, trigger) == 0) {
            commPool[i].delivered = true;
            // Restore read flag from persisted player state
            commPool[i].read = playerHasReadComm(commPool[i].id);
            commInbox[commInboxCount++] = &commPool[i];
            S.printf("[COMMS] Delivered: %s (%s)%s\n",
                     commPool[i].id, trigger, commPool[i].read ? " (was read)" : "");
        }
    }
    // Update unread count
    unreadComms = 0;
    for (int i = 0; i < commInboxCount; i++)
        if (!commInbox[i]->read) unreadComms++;
    uiDirtyCommList = true;
}

// Trigger by prefix match (e.g. "mission_start:ghost_signal" matches "mission_start:ghost_signal")
void triggerCommsPrefix(const char *prefix, const char *value) {
    char trigger[40];
    snprintf(trigger, sizeof(trigger), "%s:%s", prefix, value);
    triggerComms(trigger);
}

// Find a message by ID and deliver it
void deliverCommById(const char *id) {
    for (int i = 0; i < commPoolCount; i++) {
        if (!commPool[i].delivered && strcmp(commPool[i].id, id) == 0) {
            commPool[i].delivered = true;
            commInbox[commInboxCount++] = &commPool[i];
            unreadComms++;
            S.printf("[COMMS] Delivered by ID: %s\n", id);
            uiDirtyCommList = true;
            return;
        }
    }
}

// ═══════════════════════════════════════
//  MESH HANDLER — receives ESPNOW from GM
// ═══════════════════════════════════════
unsigned long lastGmAlertTime = 0;
char lastGmAlertTitle[40] = "";
char lastGmAlertBody[100] = "";
uint8_t lastGmAlertSeverity = 0;
uint16_t lastGmAlertDuration = 5000;

// Local pop-up using the same overlay as GM alerts (objective/mission progress)
void showObjectiveToast(const char *title, const char *body) {
    strlcpy(lastGmAlertTitle, title, sizeof(lastGmAlertTitle));
    strlcpy(lastGmAlertBody, body, sizeof(lastGmAlertBody));
    lastGmAlertSeverity = 0;
    lastGmAlertDuration = 4000;
    lastGmAlertTime = millis();
}

void onMeshMsg(const swts::MeshHeader *hdr, const uint8_t *payload, int len) {
    switch (hdr->type) {
        case swts::MSG_COMM: {
            if (len < (int)sizeof(swts::MeshComm)) return;
            const swts::MeshComm *c = (const swts::MeshComm *)payload;
            // Check if targeted at us or broadcast
            if (strlen(c->target) > 0 && strcmp(c->target, swts::myId) != 0) return;
            extern void addComm(const char *id, const char *from, const char *subject, const char *body);
            addComm(c->comm_id, c->from, c->subject, c->body);
            S.printf("[MESH] Comm received: %s\n", c->subject);
            break;
        }
        case swts::MSG_ALERT: {
            if (len < (int)sizeof(swts::MeshAlert)) return;
            const swts::MeshAlert *a = (const swts::MeshAlert *)payload;
            strlcpy(lastGmAlertTitle, a->title, sizeof(lastGmAlertTitle));
            strlcpy(lastGmAlertBody, a->body, sizeof(lastGmAlertBody));
            lastGmAlertSeverity = a->severity;
            lastGmAlertDuration = a->duration_ms ? a->duration_ms : 5000;
            lastGmAlertTime = millis();
            S.printf("[MESH] Alert: %s\n", a->title);
            break;
        }
        case swts::MSG_BOUNTY: {
            if (len < (int)sizeof(swts::MeshBounty)) return;
            const swts::MeshBounty *b = (const swts::MeshBounty *)payload;
            if (strlen(b->target) > 0 && strcmp(b->target, swts::myId) != 0) return;
            ActiveBounty *ab = findBounty(b->bounty_id);
            // Dedupe: if already in pool, drop. (Already-closed bounty must not re-open.)
            if (ab) {
                S.printf("[MESH] Dup bounty %s, dropping\n", b->bounty_id);
                break;
            }
            ab = allocBounty();
            if (!ab) { S.println("[BOUNTY] pool full"); break; }
            ab->inUse = true;
            strlcpy(ab->id, b->bounty_id, sizeof(ab->id));
            strlcpy(ab->target_name, b->target_name, sizeof(ab->target_name));
            strlcpy(ab->description, b->description, sizeof(ab->description));
            ab->reward = b->reward;
            ab->closed = false;
            ab->closedBy[0] = 0;
            ab->wonByMe = false;
            strlcpy(lastGmAlertTitle, "NEW BOUNTY", sizeof(lastGmAlertTitle));
            snprintf(lastGmAlertBody, sizeof(lastGmAlertBody), "%s — %d %s", b->target_name, b->reward, scoreSuffix);
            lastGmAlertSeverity = 1;
            lastGmAlertDuration = 4000;
            lastGmAlertTime = millis();
            S.printf("[MESH] Bounty posted: %s (%d)\n", b->target_name, b->reward);
            uiDirtyBountyList = true;
            break;
        }
        case swts::MSG_BOUNTY_CLUE: {
            if (len < (int)sizeof(swts::MeshBountyClue)) return;
            const swts::MeshBountyClue *c = (const swts::MeshBountyClue *)payload;
            ActiveBounty *ab = findBounty(c->bounty_id);
            if (!ab) { S.printf("[BOUNTY] clue for unknown %s\n", c->bounty_id); break; }
            uint8_t idx = c->clue_num >= 1 ? c->clue_num - 1 : 0;
            if (idx >= MAX_BOUNTY_CLUES) break;
            // Dedupe: if we already have the same clue text at this slot, drop.
            if (strcmp(ab->clues[idx], c->clue_text) == 0) {
                S.printf("[MESH] Dup clue %d for %s, dropping\n", c->clue_num, c->bounty_id);
                break;
            }
            strlcpy(ab->clues[idx], c->clue_text, 160);
            if (idx + 1 > ab->clueCount) ab->clueCount = idx + 1;
            strlcpy(lastGmAlertTitle, "NEW INTEL", sizeof(lastGmAlertTitle));
            snprintf(lastGmAlertBody, sizeof(lastGmAlertBody), "Clue on %s", ab->target_name);
            lastGmAlertSeverity = 1;
            lastGmAlertDuration = 3500;
            lastGmAlertTime = millis();
            S.printf("[MESH] Clue %d for %s: %s\n", c->clue_num, ab->target_name, c->clue_text);
            uiDirtyBountyList = true;
            if (scrBountyDetail && currentBounty == ab) uiDirtyBountyDetail = true;
            break;
        }
        case swts::MSG_BOUNTY_CLAIM: {
            if (len < (int)sizeof(swts::MeshBountyClaim)) return;
            const swts::MeshBountyClaim *c = (const swts::MeshBountyClaim *)payload;
            ActiveBounty *ab = findBounty(c->bounty_id);
            // Dedupe: if we already processed this claim, drop. (Closed bounty = already handled.)
            // Special-case: ab missing means we never saw the POST — still accept the claim once,
            // but allocate a stub so future resends are deduped.
            if (ab && ab->closed) {
                S.printf("[MESH] Dup claim for %s, dropping\n", c->bounty_id);
                break;
            }
            bool me = (strcmp(c->claimer_id, swts::myId) == 0);
            if (!ab) {
                ab = allocBounty();
                if (ab) {
                    ab->inUse = true;
                    strlcpy(ab->id, c->bounty_id, sizeof(ab->id));
                    strlcpy(ab->target_name, c->bounty_id, sizeof(ab->target_name));
                    ab->reward = c->reward;
                }
            }
            const char *targetName = ab ? ab->target_name : c->bounty_id;
            const char *claimerLabel = c->claimer_name[0] ? c->claimer_name : c->claimer_id;
            if (ab) {
                ab->closed = true;
                strlcpy(ab->closedBy, claimerLabel, sizeof(ab->closedBy));
                if (me) ab->wonByMe = true;
            }
            if (me) {
                score += c->reward;
                uiDirtyScore = true;
                playerMarkBountyWon(c->bounty_id);
                swts::sendScore(score, c->reward, "BOUNTY");
                strlcpy(lastGmAlertTitle, "BOUNTY COLLECTED", sizeof(lastGmAlertTitle));
                snprintf(lastGmAlertBody, sizeof(lastGmAlertBody), "%s   +%d %s", targetName, c->reward, scoreSuffix);
                lastGmAlertSeverity = 0;
                lastGmAlertDuration = 6000;
                S.printf("[MESH] BOUNTY WIN: %s +%d -> score=%d\n", targetName, c->reward, score);
            } else {
                strlcpy(lastGmAlertTitle, "BOUNTY CLOSED", sizeof(lastGmAlertTitle));
                snprintf(lastGmAlertBody, sizeof(lastGmAlertBody), "%s claimed by %s", targetName, claimerLabel);
                lastGmAlertSeverity = 2;
                lastGmAlertDuration = 4000;
                S.printf("[MESH] Bounty %s claimed by %s\n", targetName, c->claimer_id);
            }
            lastGmAlertTime = millis();
            uiDirtyBountyList = true;
            if (scrBountyDetail && currentBounty == ab && ab) uiDirtyBountyDetail = true;
            break;
        }
        case swts::MSG_BOUNTY_CANCEL: {
            if (len < (int)sizeof(swts::MeshBountyCancel)) return;
            const swts::MeshBountyCancel *c = (const swts::MeshBountyCancel *)payload;
            ActiveBounty *ab = findBounty(c->bounty_id);
            if (!ab) break;
            ab->closed = true;
            ab->closedBy[0] = 0;
            S.printf("[MESH] Bounty cancelled: %s\n", ab->id);
            uiDirtyBountyList = true;
            break;
        }
        case swts::MSG_EVENT: {
            if (len < (int)sizeof(swts::MeshEvent)) return;
            const swts::MeshEvent *e = (const swts::MeshEvent *)payload;
            S.printf("[MESH] Event: %s (sev %d)\n", e->event_name, e->severity);
            // Queued, not handled inline: this callback can run on the WiFi task.
            // loop() drains the queue → fires comms + advances mission steps.
            queueGameEvent(e->event_id);
            break;
        }
        case swts::MSG_RESET: {
            S.println("[MESH] Reset command — restarting");
            delay(500);
            ESP.restart();
            break;
        }
        case swts::MSG_ASSIGN: {
            if (len < (int)sizeof(swts::MeshAssign)) return;
            const swts::MeshAssign *a = (const swts::MeshAssign *)payload;
            // Only act if this assignment targets us specifically (by current callsign).
            if (strcmp(a->target, swts::myId) != 0) return;
            if (!a->new_callsign[0]) return;
            S.printf("[MESH] Callsign assignment: %s -> %s\n", callsign, a->new_callsign);
            strlcpy(callsign, a->new_callsign, sizeof(callsign));
            strlcpy(swts::myId, callsign, sizeof(swts::myId));
            playerDirty = true;
            playerLastSave = 0;
            // Send a fresh status with the new ID so the GM can drop the "OPERATIVE/DATAPAD-xxxx" entry
            unsigned long up = millis() / 1000;
            swts::sendStatus(up, score, activeMissions, 0, totalScans, 0);
            break;
        }
        case swts::MSG_SYNC_REQUEST: {
            // GM is asking everyone to re-report their state.
            unsigned long up = millis() / 1000;
            swts::sendStatus(up, score, activeMissions, 0, totalScans, 0);
            S.println("[MESH] Sync request — replied with status");
            break;
        }
        case swts::MSG_SCORE_SET: {
            if (len < 4) return;
            int32_t newScore = *(int32_t*)payload;
            extern int score;
            score = newScore;
            uiDirtyScore = true;
            playerDirty = true;
            S.printf("[MESH] Score forced to %d\n", newScore);
            break;
        }
        default:
            S.printf("[MESH] Unknown type 0x%02X from %s\n", hdr->type, hdr->from_id);
    }
}

// Add a runtime comm (not from SD — from prop interaction etc.)
// Idempotent: if a comm with this id already exists in the pool, drop silently.
// Lets the GM safely resend (e.g. when a packet loss is suspected) without duplicating.
void addComm(const char *id, const char *from, const char *subject, const char *body) {
    if (commPoolCount >= MAX_COMMS) return;
    for (int i = 0; i < commPoolCount; i++) {
        if (strcmp(commPool[i].id, id) == 0) {
            S.printf("[COMMS] Dup id=%s, dropping\n", id);
            return;
        }
    }
    CommMsg &c = commPool[commPoolCount];
    strlcpy(c.id, id, sizeof(c.id));
    strlcpy(c.from, from, sizeof(c.from));
    strlcpy(c.subject, subject, sizeof(c.subject));
    strlcpy(c.body, body, sizeof(c.body));
    strlcpy(c.trigger, "runtime", sizeof(c.trigger));
    c.loaded = true;
    c.delivered = true;
    c.read = false;
    commPoolCount++;
    commInbox[commInboxCount++] = &c;
    unreadComms++;
    uiDirtyCommList = true;
}

lv_obj_t *commList = NULL;
lv_obj_t *commListStatus = NULL;
lv_obj_t *commDetail = NULL;
lv_obj_t *commDetailFrom = NULL;
lv_obj_t *commDetailSubj = NULL;
lv_obj_t *commDetailBody = NULL;
bool commShowingDetail = false;

void refreshCommList();   // forward decl

static void ev_comm_back(lv_event_t *e) {
    if (commShowingDetail) {
        // Back to list — refresh so read state updates
        refreshCommList();
        lv_obj_add_flag(commDetail, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(commList, LV_OBJ_FLAG_HIDDEN);
        commShowingDetail = false;
    } else {
        lv_scr_load_anim(scrHome, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 180, 0, false);
    }
}

static void ev_comm_tap(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= commInboxCount) return;
    CommMsg &m = *commInbox[idx];
    m.read = true;
    playerMarkCommRead(m.id);   // persist read state to player.json

    // Update unread count
    unreadComms = 0;
    for (int i = 0; i < commInboxCount; i++)
        if (!commInbox[i]->read) unreadComms++;

    // Show detail
    lv_label_set_text(commDetailFrom, m.from);
    lv_label_set_text(commDetailSubj, m.subject);
    lv_label_set_text(commDetailBody, m.body);

    lv_obj_add_flag(commList, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(commDetail, LV_OBJ_FLAG_HIDDEN);
    commShowingDetail = true;
}

static lv_obj_t *makeBadge(lv_obj_t *parent, int count) {
    char bv[4]; snprintf(bv, sizeof(bv), "%d", count);
    lv_obj_t *bg = lv_label_create(parent);
    lv_label_set_text(bg, bv);
    lv_obj_add_style(bg, &s_badge, 0);
    lv_obj_set_style_bg_color(bg, C_AMB, 0);
    lv_obj_set_style_text_color(bg, C_BG, 0);
    lv_obj_align(bg, LV_ALIGN_TOP_RIGHT, -8, 8);
    return bg;
}

void refreshHomeBadges() {
    if (homeMissionBadge) { lv_obj_del(homeMissionBadge); homeMissionBadge = NULL; }
    if (homeBountyBadge)  { lv_obj_del(homeBountyBadge);  homeBountyBadge  = NULL; }
    if (homeCommBadge)    { lv_obj_del(homeCommBadge);    homeCommBadge    = NULL; }
    if (homeMissionBtn && activeMissions > 0) homeMissionBadge = makeBadge(homeMissionBtn, activeMissions);
    int bn = activeBountyCount();
    if (homeBountyBtn  && bn > 0)             homeBountyBadge  = makeBadge(homeBountyBtn,  bn);
    if (homeCommBtn    && unreadComms > 0)    homeCommBadge    = makeBadge(homeCommBtn,    unreadComms);
}

void refreshCommList() {
    if (!commList) return;
    lv_obj_clean(commList);

    if (commListStatus) {
        char sbuf[32];
        snprintf(sbuf, sizeof(sbuf), "%d TRANSMISSION%s", commInboxCount, commInboxCount != 1 ? "S" : "");
        lv_label_set_text(commListStatus, sbuf);
    }

    refreshHomeBadges();

    if (commInboxCount == 0) {
        lv_obj_t *empty = lv_label_create(commList);
        lv_label_set_text(empty, "INBOX QUIET\n\nWaiting for transmissions\nfrom command...");
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(empty, C_DIM, 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
        lv_obj_set_width(empty, lv_pct(100));
        return;
    }

    // Newest first
    for (int i = commInboxCount - 1; i >= 0; i--) {
        CommMsg &m = *commInbox[i];

        lv_obj_t *card = lv_obj_create(commList);
        lv_obj_set_size(card, W - 20, 62);
        lv_obj_add_style(card, &s_pnl, 0);
        lv_obj_set_style_border_color(card, m.read ? C_FRM : C_AMB, 0);
        lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(card, m.read ? 2 : 4, 0);
        lv_obj_set_style_bg_color(card, C_PNL2, LV_STATE_PRESSED);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, ev_comm_tap, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        // From
        lv_obj_t *from = lv_label_create(card);
        lv_label_set_text(from, m.from);
        lv_obj_set_style_text_font(from, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(from, m.read ? C_DIM : C_AMB_BRT, 0);
        lv_obj_set_pos(from, 4, -2);
        lv_obj_clear_flag(from, LV_OBJ_FLAG_CLICKABLE);

        // Subject
        lv_obj_t *subj = lv_label_create(card);
        lv_label_set_text(subj, m.subject);
        lv_obj_set_style_text_font(subj, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(subj, m.read ? C_MUT : C_AMB, 0);
        lv_obj_set_pos(subj, 4, 18);
        lv_obj_clear_flag(subj, LV_OBJ_FLAG_CLICKABLE);

        // Unread dot
        if (!m.read) {
            lv_obj_t *dot = lv_obj_create(card);
            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, 8, 8);
            lv_obj_set_style_bg_color(dot, C_AMB_BRT, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            lv_obj_align(dot, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        }
    }
}

void buildCommsScreen() {
    scrComms = lv_obj_create(NULL);
    lv_obj_add_style(scrComms, &s_scr, 0);

    // Status label
    commListStatus = lv_label_create(scrComms);
    char sbuf[32];
    snprintf(sbuf, sizeof(sbuf), "%d TRANSMISSION%s", commInboxCount, commInboxCount != 1 ? "S" : "");
    lv_label_set_text(commListStatus, sbuf);
    lv_obj_set_style_text_font(commListStatus, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(commListStatus, C_DIM, 0);
    lv_obj_set_pos(commListStatus, 12, 56);

    hline(scrComms, 74, C_FRM, 1);

    // Message list (scrollable)
    commList = lv_obj_create(scrComms);
    lv_obj_set_size(commList, W - 8, H - 90);
    lv_obj_set_pos(commList, 4, 78);
    lv_obj_set_style_bg_opa(commList, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(commList, 0, 0);
    lv_obj_set_style_radius(commList, 0, 0);
    lv_obj_set_style_pad_all(commList, 0, 0);
    lv_obj_set_flex_flow(commList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(commList, 6, 0);

    refreshCommList();

    // Detail view (hidden initially)
    commDetail = lv_obj_create(scrComms);
    lv_obj_set_size(commDetail, W - 8, H - 90);
    lv_obj_set_pos(commDetail, 4, 78);
    lv_obj_set_style_bg_color(commDetail, C_BG, 0);
    lv_obj_set_style_bg_opa(commDetail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(commDetail, 0, 0);
    lv_obj_set_style_radius(commDetail, 0, 0);
    lv_obj_set_style_pad_all(commDetail, 8, 0);
    lv_obj_add_flag(commDetail, LV_OBJ_FLAG_HIDDEN);

    commDetailFrom = lv_label_create(commDetail);
    lv_obj_set_style_text_font(commDetailFrom, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(commDetailFrom, C_AMB_BRT, 0);
    lv_obj_set_pos(commDetailFrom, 0, 0);

    commDetailSubj = lv_label_create(commDetail);
    lv_obj_set_style_text_font(commDetailSubj, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(commDetailSubj, C_AMB, 0);
    lv_obj_set_pos(commDetailSubj, 0, 28);

    // Separator
    lv_obj_t *sep = lv_obj_create(commDetail);
    lv_obj_remove_style_all(sep);
    lv_obj_set_size(sep, W - 30, 1);
    lv_obj_set_pos(sep, 0, 50);
    lv_obj_set_style_bg_color(sep, C_FRM, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);

    commDetailBody = lv_label_create(commDetail);
    lv_obj_set_style_text_font(commDetailBody, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(commDetailBody, C_AMB, 0);
    lv_obj_set_style_text_line_space(commDetailBody, 4, 0);
    lv_obj_set_width(commDetailBody, W - 30);
    lv_obj_set_pos(commDetailBody, 0, 60);

    // Header + back button on top of everything
    lv_obj_t *hdr = lv_obj_create(scrComms);
    lv_obj_set_size(hdr, W, 46);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, C_BG, 0);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_set_style_pad_all(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *tt = lv_label_create(hdr);
    lv_label_set_text(tt, "COMMS");
    lv_obj_set_style_text_font(tt, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(tt, C_AMB_BRT, 0);
    lv_obj_set_pos(tt, W - 70, 13);
    hline(scrComms, 46, C_AMB_DIM, 2);

    lv_obj_t *bb = lv_btn_create(scrComms);
    lv_obj_set_size(bb, 80, 36);
    lv_obj_set_pos(bb, 6, 5);
    lv_obj_set_style_bg_color(bb, C_BG, 0);
    lv_obj_set_style_bg_opa(bb, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bb, C_PNL2, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(bb, C_AMB_DIM, 0);
    lv_obj_set_style_border_width(bb, 1, 0);
    lv_obj_set_style_radius(bb, 2, 0);
    lv_obj_set_style_shadow_width(bb, 0, 0);
    lv_obj_set_style_pad_all(bb, 0, 0);
    lv_obj_add_event_cb(bb, ev_comm_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bbl = lv_label_create(bb);
    lv_label_set_text(bbl, "< BACK");
    lv_obj_set_style_text_font(bbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bbl, C_AMB, 0);
    lv_obj_center(bbl);
}

// ═══════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════
void setup() {
    S.begin(115200);
    // Wait for serial monitor to connect
    delay(3000);
    S.println("\n================================");
    S.println("  SWTS DATAPAD — Czerka DP-47");
    S.println("================================");
    // Why did the previous boot end? Critical info for diagnosing "random reboots":
    //   POWERON_RESET (1) = power cycle / USB suspend on host
    //   SW_RESET (3)     = ESP.restart() called by firmware
    //   WDT_RESET (4)    = task watchdog timed out (firmware hang)
    //   INT_WDT (5)      = interrupt watchdog
    //   BROWNOUT_RESET(15) = voltage sagged (power supply too weak)
    //   USB_RESET, RTC_WDT, etc.
    {
        esp_reset_reason_t r = esp_reset_reason();
        const char *names[] = {
            "UNKNOWN", "POWERON", "EXT", "SW", "PANIC", "INT_WDT", "TASK_WDT",
            "WDT", "DEEPSLEEP", "BROWNOUT", "SDIO", "USB", "JTAG", "EFUSE", "PWR_GLITCH", "CPU_LOCKUP"
        };
        const char *name = (r < (sizeof(names)/sizeof(names[0]))) ? names[r] : "?";
        S.printf("[BOOT] reset_reason = %d (%s)\n", (int)r, name);
    }

    // Buzzer
    pinMode(BUZZER_PIN, OUTPUT);

    // Physical buttons + their LEDs (cycle test on boot)
    initButtons();
    // All three lit solid; each one beeps a distinct tone when pressed.
    btnHandler = defaultBtnTone;
    ledOn(BTN_BLUE);
    ledOn(BTN_WHITE);
    ledOn(BTN_RED);

    // Display (must init before SD since they share SPI)
    tft.init(); tft.setRotation(2); tft.setBrightness(255);

    // SD Card — HSPI bus (same pins as panel: CS=15 MO=16 CK=17 MI=18)
    static SPIClass sdSPI(HSPI);
    sdSPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
    if (SD.begin(SD_CS, sdSPI, 4000000)) {
        sdOk = true;
        S.printf("[SD] Mounted %lluMB\n", SD.cardSize() / (1024*1024));

        // TESTING: write embedded gameplay configs to the card (no-op when
        // SWTS_WRITE_TEST_CONFIGS is commented out in swts_test_configs.h)
        swts_test::writeTestConfigs(SD);

        // Load config from SD
        File cfg = SD.open("/SWTS/config.json", FILE_READ);
        if (cfg) {
            JsonDocument doc;
            if (!deserializeJson(doc, cfg)) {
                const char *cur = doc["scenario"]["currency"] | "CR";
                const char *planet = doc["scenario"]["planet"] | "Unknown";
                strlcpy(scoreSuffix, cur, sizeof(scoreSuffix));
                strlcpy(planetName, planet, sizeof(planetName));
                S.printf("[CFG] Planet: %s | Currency: %s\n", planetName, scoreSuffix);
            }
            cfg.close();
        }

        // Load game data from SD — order matters: missions before player state
        initMissions();
        loadMissionsFromSD();
        loadPlayerState();    // restores callsign, score, mission progress, comm-read flags
        loadCommsFromSD();    // loads pool + delivers boot-triggered comms (welcome etc.)
        autoStartMissions();  // day-one missions go live (fires their briefing comms)
    } else {
        S.println("[SD] Mount failed");
    }

    // Touch
    pinMode(TOUCH_RST, OUTPUT); pinMode(TOUCH_INT, INPUT);
    digitalWrite(TOUCH_RST, LOW); delay(50); digitalWrite(TOUCH_RST, HIGH); delay(200);
    Wire.begin(TOUCH_SDA, TOUCH_SCL, 400000); delay(100);
    Wire.beginTransmission(T_ADDR);
    touchOk = (Wire.endTransmission() == 0);

    // NFC
    I2C_NFC.begin(NFC_SDA, NFC_SCL, 100000); delay(50);
    for (uint8_t a = 1; a < 127; a++) {
        I2C_NFC.beginTransmission(a);
        if (I2C_NFC.endTransmission() == 0) { nfc.begin(); uint32_t fw=nfc.getFirmwareVersion(); if(fw){nfcOk=true;nfc.SAMConfig();} break; }
    }

    S.println("--------------------------------");
    S.printf("  Display: SCK=40 MOSI=41 CS=1\n");
    S.printf("  Touch:   SDA=47 SCL=38 — %s\n", touchOk ? "OK" : "FAIL");
    S.printf("  NFC:     SDA=14 SCL=13 — %s\n", nfcOk ? "OK" : "FAIL");
    S.printf("  SD Card: CS=15 MO=16 CK=17 MI=18 — %s\n", sdOk ? "OK" : "FAIL");
    S.printf("  Buzzer:  GPIO 4\n");
    S.println("--------------------------------");

    // LVGL
    lv_init();
    buf1 = (lv_color_t*)heap_caps_malloc(W * 80 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    buf2 = (lv_color_t*)heap_caps_malloc(W * 80 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf1) { buf1 = (lv_color_t*)malloc(W * 40 * sizeof(lv_color_t)); buf2 = NULL; }
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, W * (buf2 ? 80 : 40));

    static lv_disp_drv_t dd; lv_disp_drv_init(&dd);
    dd.hor_res=W; dd.ver_res=H; dd.flush_cb=lvgl_flush; dd.draw_buf=&draw_buf;
    lv_disp_drv_register(&dd);

    static lv_indev_drv_t id; lv_indev_drv_init(&id);
    id.type=LV_INDEV_TYPE_POINTER; id.read_cb=lvgl_touch;
    lv_indev_drv_register(&id);

    initStyles();
    buildHomeScreen();
    buildDatacardScreen();
    buildNearbyScreen();
    buildMissionsScreen();
    buildBountyScreen();
    buildCargoScreen();
    buildCommsScreen();
    buildPropScreen();
    buildSliceScreen();
    buildSimonScreen();
    buildPurgeScreen();
    lv_scr_load(scrHome);

    // ── ESPNOW Mesh ──
    // WiFi must be in STA mode for ESPNOW (it already is after our scan)
    WiFi.mode(WIFI_STA);
    swts::meshInit(callsign, swts::ROLE_DATAPAD, onMeshMsg);
}

// ═══════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════
static unsigned long dcTime = 0;
static unsigned long cgTime = 0;

void loop() {
    lv_timer_handler();

    // Physical button polling — drives the colored LEDs + emits edge events
    pollButtons();

    // Drain mesh-driven UI refreshes (LVGL is not thread-safe — these must run here)
    if (uiDirtyScore)        { uiDirtyScore = false;        refreshScoreLabel(); }
    if (uiDirtyCommList)     { uiDirtyCommList = false;     refreshCommList(); }
    if (uiDirtyBountyList)   { uiDirtyBountyList = false;   refreshBountyList(); }
    if (uiDirtyBountyDetail) {
        uiDirtyBountyDetail = false;
        if (currentBounty) {
            // Explicit delete-then-rebuild so the old screen object is freed.
            // buildBountyDetailScreen() reassigns scrBountyDetail to a fresh lv_obj.
            lv_obj_t *oldScr = scrBountyDetail;
            scrBountyDetail = nullptr;
            buildBountyDetailScreen();
            if (scrBountyDetail) lv_scr_load_anim(scrBountyDetail, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
            if (oldScr) lv_obj_del(oldScr);
        }
    }

    // Debounced player.json save
    if (playerDirty && sdOk && millis() - playerLastSave > PLAYER_SAVE_DEBOUNCE_MS) {
        playerDirty = false;
        if (savePlayerState()) S.println("[PLAYER] state saved");
    }

    lv_obj_t *act = lv_scr_act();
    if (act == scrDatacard && dcScanning && nfcOk) {
        uint8_t uid[7]; uint8_t len;
        if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 80)) {
            readNdefText();
            // showCardResult plays its own buzzer (ok for datacard, fail for cargo redirect)
            if (!isCargoTag(ndefText)) buzzerScanOk();
            showCardResult(uid, len);
            dcTime = millis();
        }
    }
    if (act == scrDatacard && !dcScanning && dcTime && millis() - dcTime > 4000) {
        resetDcScan(); dcTime = 0;
    }

    // Cargo Intel — same flow but filtered for CARGO_* tags
    if (act == scrCargo && cgScanning && nfcOk) {
        uint8_t uid[7]; uint8_t len;
        if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len, 80)) {
            readNdefText();
            showCargoResult(uid, len);   // plays its own buzzer based on outcome
            cgTime = millis();
        }
    }
    if (act == scrCargo && !cgScanning && cgTime && millis() - cgTime > 4000) {
        resetCgScan(); cgTime = 0;
    }

    // Scan on screen entry (once)
    static bool nbDidScan = false;
    static unsigned long nbEnterTime = 0;
    if (act == scrNearby) {
        if (!nbDidScan) {
            if (nbEnterTime == 0) nbEnterTime = millis();
            if (millis() - nbEnterTime > 300) {
                nbDidScan = true;
                doFullScan();
            }
        }
        // Enable card taps 1 second after results
        if (nbResultsTime > 0 && millis() - nbResultsTime > 1000) {
            uint32_t cnt = lv_obj_get_child_cnt(nbList);
            for (uint32_t i = 0; i < cnt; i++)
                lv_obj_add_flag(lv_obj_get_child(nbList, i), LV_OBJ_FLAG_CLICKABLE);
            nbResultsTime = 0;
        }
    }
    if (act != scrNearby) { nbDidScan = false; nbEnterTime = 0; }

    // Deferred prop connection
    if (pendingPropConnect >= 0) {
        int idx = pendingPropConnect;
        pendingPropConnect = -1;
        openPropScreen(idx);
    }

    // Game events (mesh + prop responses): fire comms, advance mission steps
    processGameEvents();

    // Endgame debrief — small delay so the final objective feedback is seen first
    static unsigned long debriefAt = 0;
    if (debriefPending) { debriefPending = false; debriefAt = millis() + 2500; }
    if (debriefAt && millis() > debriefAt) {
        debriefAt = 0;
        showDebrief();
    }

    // Slice minigame: update cursor each frame, report result 2s after it ends
    if (act == scrSlice) {
        updateSliceGame();

        if (!sliceActive && sliceEndTime && millis() - sliceEndTime > 2000) {
            sliceEndTime = 0;
            JsonDocument req;
            req["score"] = sliceScore;
            finishMinigameAndReturn(req, sliceWon);
        }
    }

    // Simon Says minigame: advance state machine each frame, report result
    // 2.5s after it ends
    if (act == scrSimon) {
        updateSimonGame();

        if (!simonActive && simonEndTime && millis() - simonEndTime > 2500) {
            simonEndTime = 0;
            ledAllOff();
            btnHandler = defaultBtnTone;   // restore press tones suppressed during the game

            JsonDocument req;
            req["game"] = "simon";
            req["rounds"] = simonRounds;
            finishMinigameAndReturn(req, simonWon);
        }
    }

    // Core Purge minigame: advance each frame, report result 2.5s after it ends
    if (act == scrPurge) {
        updatePurgeGame();

        if (!purgeActive && purgeEndTime && millis() - purgeEndTime > 2500) {
            purgeEndTime = 0;
            JsonDocument req;
            req["game"] = "purge";
            req["purged"] = purgePurged;
            req["strikes"] = purgeStrikes;
            finishMinigameAndReturn(req, purgeWon);
        }
    }

    // Periodic status heartbeat (serial + mesh to GM)
    static unsigned long lastStatus = 0;
    if (millis() - lastStatus > 5000) {
        lastStatus = millis();
        unsigned long up = millis() / 1000;
        // Heap diagnostics — track minimum-ever-seen so we can see slow leaks
        static uint32_t heapMinEver = 0xFFFFFFFF;
        uint32_t freeHeap = ESP.getFreeHeap();
        uint32_t freePsram = ESP.getFreePsram();
        if (freeHeap < heapMinEver) heapMinEver = freeHeap;
        S.printf("[STATUS] cs=%s up=%lus touch=%s nfc=%s sd=%s %s=%d scans=%d msns=%d read=%d won=%d dirty=%d btn[B6=%d W8=%d R10=%d] heap=%u (min=%u) psram=%u\n",
                 callsign, up, touchOk ? "OK" : "--", nfcOk ? "OK" : "--", sdOk ? "OK" : "--",
                 scoreSuffix, score, totalScans, activeMissions,
                 playerReadCommCount, playerWonBountyCount, (int)playerDirty,
                 digitalRead(BTN_BLUE_PIN), digitalRead(BTN_WHITE_PIN), digitalRead(BTN_RED_PIN),
                 (unsigned)freeHeap, (unsigned)heapMinEver, (unsigned)freePsram);

        // Tell GM we're alive + send full status
        swts::sendStatus(up, score, activeMissions, 0, totalScans, 0);
    }

    delay(5);
}
