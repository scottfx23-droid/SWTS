/*
 * SFGE Actor Tool — a spare datapad flashed for in-game staff
 *
 * An actor scans a player's operative card (PLAYER:<callsign>) and gets an
 * action menu: award/deduct credits, hand over a digital item, or fire a
 * story event — closing the loop on datacards handed to actors without the
 * GM having to notice. Same hardware as the player datapad (ST7796 + FT6336
 * touch + PN532 NFC + SD + buzzer); no player state of its own.
 *
 * Config: /SFGE/actor.json
 *   { "device": {"id": "ACTOR_01"},
 *     "credits": [ {"label": "REWARD +100", "delta": 100}, ... ],
 *     "items":   [ {"id": "JAMMER", "label": "GIVE JAMMER"}, ... ],
 *     "events":  [ {"id": "actor_signal", "label": "FIRE: SIGNAL",
 *                   "severity": 1}, ... ] }
 *
 * Mesh: ROLE_ACTOR — never shows up as a player on the GM roster. Awards go
 * out as MSG_AWARD (3x with a dedupe code); events ride the reliable
 * gameplay-event layer like any prop's.
 */
#include <LovyanGFX.hpp>
#include <lvgl.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_PN532.h>
#include <esp_random.h>

#include "sfge_mesh.h"
#include "sfge_ota.h"
#include "sfge_test_configs.h"
#define S Serial

// ═══════════════════════════════════════
//  PINS — identical to the player datapad
// ═══════════════════════════════════════
#define TOUCH_SDA  47
#define TOUCH_SCL  38
#define NFC_SDA    14
#define NFC_SCL    13
#define SD_CS      15
#define SD_MOSI    16
#define SD_CLK     17
#define SD_MISO    18
#define BUZZER_PIN 4

void buzzerTone(int f, int ms) { tone(BUZZER_PIN, f, ms); }
void buzzOk()   { buzzerTone(880, 60); delay(70); buzzerTone(1320, 80); }
void buzzFail() { buzzerTone(300, 150); delay(160); buzzerTone(200, 200); }
void buzzSent() { buzzerTone(523, 80); delay(90); buzzerTone(784, 120); }

// ═══════════════════════════════════════
//  DISPLAY / TOUCH / LVGL (datapad hardware)
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

#define T_ADDR 0x38
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

// Actor palette: cool cyan — visibly NOT a player datapad
#define C_BG   lv_color_hex(0x000000)
#define C_PNL  lv_color_hex(0x04090C)
#define C_PNL2 lv_color_hex(0x081318)
#define C_FRM  lv_color_hex(0x103038)
#define C_CYN  lv_color_hex(0x00AACC)
#define C_BRT  lv_color_hex(0x33DDFF)
#define C_DIM  lv_color_hex(0x00566A)
#define C_GRN  lv_color_hex(0x33CC66)
#define C_RED  lv_color_hex(0xDD4433)

// ═══════════════════════════════════════
//  NFC
// ═══════════════════════════════════════
TwoWire I2C_NFC = TwoWire(1);
Adafruit_PN532 nfc(-1, -1, &I2C_NFC);
bool nfcOk = false;
char ndefText[128] = "";

void readNdefText() {
    ndefText[0] = 0;
    uint8_t buf[128];
    int pos = 0;
    for (int page = 4; page < 36 && pos < 120; page++) {
        uint8_t data[4];
        if (!nfc.ntag2xx_ReadPage(page, data)) break;
        for (int i = 0; i < 4 && pos < 127; i++) buf[pos++] = data[i];
    }
    int i = 0;
    while (i < pos) {
        uint8_t tlvType = buf[i];
        if (tlvType == 0x00) { i++; continue; }
        if (tlvType == 0xFE) break;
        if (i + 1 >= pos) break;
        uint8_t tlvLen = buf[i + 1];
        int dataStart = i + 2;
        if (tlvType == 0x03 && tlvLen > 0) {
            int r = dataStart;
            if (r + 3 >= pos) break;
            uint8_t typeLen = buf[r + 1];
            uint8_t payLen = buf[r + 2];
            int typeOff = r + 3;
            if (typeLen == 1 && typeOff < pos && buf[typeOff] == 0x54) {
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
            break;
        }
        i = dataStart + tlvLen;
    }
    ndefText[0] = 0;
}

// ═══════════════════════════════════════
//  CONFIG — /SFGE/actor.json
// ═══════════════════════════════════════
#define MAX_ACTIONS 8
struct CreditAction { char label[28]; int delta; };
struct ItemAction   { char id[24]; char label[28]; };
struct EventAction  { char id[32]; char label[28]; uint8_t severity; };

struct {
    char id[16] = "ACTOR_01";
    CreditAction credits[MAX_ACTIONS]; int creditCount = 0;
    ItemAction   items[MAX_ACTIONS];   int itemCount = 0;
    EventAction  events[MAX_ACTIONS];  int eventCount = 0;
} cfg;

bool sdOk = false;

bool loadConfig() {
    File f = SD.open("/SFGE/actor.json", FILE_READ);
    if (!f) { S.println("[ACTOR] actor.json not found"); return false; }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) { S.printf("[ACTOR] JSON error: %s\n", err.c_str()); return false; }

    strlcpy(cfg.id, doc["device"]["id"] | "ACTOR_01", sizeof(cfg.id));
    cfg.creditCount = cfg.itemCount = cfg.eventCount = 0;
    for (JsonObject o : doc["credits"].as<JsonArray>()) {
        if (cfg.creditCount >= MAX_ACTIONS) break;
        CreditAction &a = cfg.credits[cfg.creditCount++];
        strlcpy(a.label, o["label"] | "", sizeof(a.label));
        a.delta = o["delta"] | 0;
    }
    for (JsonObject o : doc["items"].as<JsonArray>()) {
        if (cfg.itemCount >= MAX_ACTIONS) break;
        ItemAction &a = cfg.items[cfg.itemCount++];
        strlcpy(a.id, o["id"] | "", sizeof(a.id));
        strlcpy(a.label, o["label"] | "", sizeof(a.label));
    }
    for (JsonObject o : doc["events"].as<JsonArray>()) {
        if (cfg.eventCount >= MAX_ACTIONS) break;
        EventAction &a = cfg.events[cfg.eventCount++];
        strlcpy(a.id, o["id"] | "", sizeof(a.id));
        strlcpy(a.label, o["label"] | "", sizeof(a.label));
        a.severity = o["severity"] | 1;
    }
    S.printf("[ACTOR] %s: %d credit, %d item, %d event actions\n",
             cfg.id, cfg.creditCount, cfg.itemCount, cfg.eventCount);
    return true;
}

// ═══════════════════════════════════════
//  PLAYER ROSTER — from datapad MSG_STATUS broadcasts
// ═══════════════════════════════════════
#define MAX_ROSTER 12
struct RosterEntry { char cs[16]; char faction[14]; int score; unsigned long heard; };
RosterEntry roster[MAX_ROSTER] = {};

RosterEntry* rosterFind(const char *cs) {
    for (int i = 0; i < MAX_ROSTER; i++)
        if (roster[i].cs[0] && strcasecmp(roster[i].cs, cs) == 0) return &roster[i];
    return nullptr;
}

volatile bool restartRequested = false;

void onMeshMsg(const sfge::MeshHeader *hdr, const uint8_t *payload, int len) {
    if (hdr->type == sfge::MSG_STATUS && hdr->role == sfge::ROLE_DATAPAD &&
        len >= (int)sizeof(sfge::MeshStatus)) {
        const sfge::MeshStatus *st = (const sfge::MeshStatus *)payload;
        RosterEntry *r = rosterFind(hdr->from_id);
        if (!r) {
            unsigned long oldest = 0xFFFFFFFF; int slot = 0;
            for (int i = 0; i < MAX_ROSTER; i++) {
                if (!roster[i].cs[0]) { slot = i; break; }
                if (roster[i].heard < oldest) { oldest = roster[i].heard; slot = i; }
            }
            r = &roster[slot];
            memset(r, 0, sizeof(*r));
            strlcpy(r->cs, hdr->from_id, sizeof(r->cs));
        }
        strlcpy(r->faction, st->faction, sizeof(r->faction));
        r->score = st->score;
        r->heard = millis();
        return;
    }
    if (hdr->type == sfge::MSG_RESET || hdr->type == sfge::MSG_NEW_GAME)
        restartRequested = true;   // applied from loop()
}

// ═══════════════════════════════════════
//  UI — scan screen + action screen
// ═══════════════════════════════════════
lv_obj_t *scrScan = nullptr, *scrAction = nullptr;
lv_obj_t *scanStatus = nullptr, *scanRoster = nullptr;
lv_obj_t *actTitle = nullptr, *actInfo = nullptr, *actList = nullptr, *actToast = nullptr;
char targetCs[16] = "";
unsigned long toastUntil = 0;

void showToast(const char *msg, bool ok) {
    if (!actToast) return;
    lv_label_set_text(actToast, msg);
    lv_obj_set_style_text_color(actToast, ok ? C_GRN : C_RED, 0);
    lv_obj_clear_flag(actToast, LV_OBJ_FLAG_HIDDEN);
    toastUntil = millis() + 2500;
}

static void ev_credit(lv_event_t *e) {
    CreditAction *a = (CreditAction *)lv_event_get_user_data(e);
    sfge::sendAward(targetCs, a->delta, "", a->label);
    char m[64];
    snprintf(m, sizeof(m), "SENT: %+d to %s", a->delta, targetCs);
    showToast(m, true);
    buzzSent();
}

static void ev_item(lv_event_t *e) {
    ItemAction *a = (ItemAction *)lv_event_get_user_data(e);
    sfge::sendAward(targetCs, 0, a->id, a->label);
    char m[64];
    snprintf(m, sizeof(m), "SENT: %s to %s", a->id, targetCs);
    showToast(m, true);
    buzzSent();
}

static void ev_event(lv_event_t *e) {
    EventAction *a = (EventAction *)lv_event_get_user_data(e);
    sfge::gmTriggerEvent(a->id, a->label, a->severity, targetCs);   // reliable layer
    char m[64];
    snprintf(m, sizeof(m), "EVENT FIRED: %s", a->id);
    showToast(m, true);
    buzzSent();
}

static void ev_done(lv_event_t *e) {
    targetCs[0] = 0;
    lv_scr_load(scrScan);
}

lv_obj_t* actionBtn(lv_obj_t *parent, const char *label, lv_color_t col,
                    lv_event_cb_t cb, void *ud) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, W - 36, 48);
    lv_obj_set_style_bg_color(btn, C_PNL2, 0);
    lv_obj_set_style_border_color(btn, col, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_center(l);
    return btn;
}

void openActionScreen(const char *cs) {
    strlcpy(targetCs, cs, sizeof(targetCs));
    lv_label_set_text(actTitle, targetCs);

    RosterEntry *r = rosterFind(cs);
    char info[80];
    if (r && millis() - r->heard < 60000)
        snprintf(info, sizeof(info), "%s   ·   %d CR   ·   online",
                 r->faction[0] ? r->faction : "NO FACTION", r->score);
    else
        strlcpy(info, "(not heard on the mesh recently)", sizeof(info));
    lv_label_set_text(actInfo, info);

    lv_obj_add_flag(actToast, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load(scrAction);
}

void buildUI() {
    // ── Scan screen ──
    scrScan = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scrScan, C_BG, 0);

    lv_obj_t *hdr = lv_label_create(scrScan);
    lv_label_set_text(hdr, "ACTOR TOOL");
    lv_obj_set_style_text_color(hdr, C_BRT, 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_28, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *idl = lv_label_create(scrScan);
    lv_label_set_text(idl, cfg.id);
    lv_obj_set_style_text_color(idl, C_DIM, 0);
    lv_obj_set_style_text_font(idl, &lv_font_montserrat_14, 0);
    lv_obj_align(idl, LV_ALIGN_TOP_MID, 0, 58);

    scanStatus = lv_label_create(scrScan);
    lv_label_set_text(scanStatus, "SCAN OPERATIVE CARD");
    lv_obj_set_style_text_color(scanStatus, C_CYN, 0);
    lv_obj_set_style_text_font(scanStatus, &lv_font_montserrat_20, 0);
    lv_obj_align(scanStatus, LV_ALIGN_CENTER, 0, -40);

    scanRoster = lv_label_create(scrScan);
    lv_label_set_text(scanRoster, "");
    lv_obj_set_style_text_color(scanRoster, C_DIM, 0);
    lv_obj_set_style_text_font(scanRoster, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(scanRoster, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(scanRoster, LV_ALIGN_CENTER, 0, 30);

    // ── Action screen ──
    scrAction = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scrAction, C_BG, 0);

    actTitle = lv_label_create(scrAction);
    lv_obj_set_style_text_color(actTitle, C_BRT, 0);
    lv_obj_set_style_text_font(actTitle, &lv_font_montserrat_28, 0);
    lv_obj_set_pos(actTitle, 16, 14);

    actInfo = lv_label_create(scrAction);
    lv_obj_set_style_text_color(actInfo, C_CYN, 0);
    lv_obj_set_style_text_font(actInfo, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(actInfo, 16, 50);

    actList = lv_obj_create(scrAction);
    lv_obj_set_size(actList, W - 16, H - 150);
    lv_obj_set_pos(actList, 8, 76);
    lv_obj_set_style_bg_opa(actList, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(actList, 0, 0);
    lv_obj_set_style_pad_all(actList, 4, 0);
    lv_obj_set_flex_flow(actList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(actList, 8, 0);

    for (int i = 0; i < cfg.creditCount; i++)
        actionBtn(actList, cfg.credits[i].label,
                  cfg.credits[i].delta >= 0 ? C_GRN : C_RED, ev_credit, &cfg.credits[i]);
    for (int i = 0; i < cfg.itemCount; i++)
        actionBtn(actList, cfg.items[i].label, C_BRT, ev_item, &cfg.items[i]);
    for (int i = 0; i < cfg.eventCount; i++)
        actionBtn(actList, cfg.events[i].label, C_CYN, ev_event, &cfg.events[i]);

    actToast = lv_label_create(scrAction);
    lv_label_set_text(actToast, "");
    lv_obj_set_style_text_font(actToast, &lv_font_montserrat_16, 0);
    lv_obj_align(actToast, LV_ALIGN_BOTTOM_MID, 0, -62);
    lv_obj_add_flag(actToast, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *done = lv_btn_create(scrAction);
    lv_obj_set_size(done, W - 36, 44);
    lv_obj_align(done, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(done, C_PNL2, 0);
    lv_obj_set_style_border_color(done, C_DIM, 0);
    lv_obj_set_style_border_width(done, 1, 0);
    lv_obj_add_event_cb(done, ev_done, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *dl = lv_label_create(done);
    lv_label_set_text(dl, "DONE — NEXT PLAYER");
    lv_obj_set_style_text_color(dl, C_CYN, 0);
    lv_obj_set_style_text_font(dl, &lv_font_montserrat_16, 0);
    lv_obj_center(dl);

    lv_scr_load(scrScan);
}

// ═══════════════════════════════════════
//  SETUP / LOOP
// ═══════════════════════════════════════
void setup() {
    S.begin(115200);
    delay(200);
    S.println("\n================================");
    S.println("  SFGE ACTOR TOOL — starting");
    S.println("================================");

    pinMode(BUZZER_PIN, OUTPUT);

    // SD → OTA → test provisioning → config
    SPIClass *sdSPI = new SPIClass(HSPI);
    sdSPI->begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
    sdOk = SD.begin(SD_CS, *sdSPI, 20000000);
    if (sdOk) {
        sfge_ota::checkAndFlash(SD);
        sfge_test::writeTestConfigs(SD);
        loadConfig();
    } else {
        S.println("[ACTOR] SD mount failed — defaults only");
    }

    // Display + LVGL
    tft.begin();
    tft.setRotation(0);
    tft.setBrightness(255);
    lv_init();
    size_t bufPx = W * 60;
    buf1 = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t), MALLOC_CAP_DMA);
    buf2 = (lv_color_t *)heap_caps_malloc(bufPx * sizeof(lv_color_t), MALLOC_CAP_DMA);
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, bufPx);
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = W; disp_drv.ver_res = H;
    disp_drv.flush_cb = lvgl_flush; disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    Wire.begin(TOUCH_SDA, TOUCH_SCL, 400000);
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch;
    lv_indev_drv_register(&indev_drv);

    // NFC
    I2C_NFC.begin(NFC_SDA, NFC_SCL, 100000);
    nfc.begin();
    uint32_t ver = nfc.getFirmwareVersion();
    if (ver) { nfc.SAMConfig(); nfcOk = true; S.println("[ACTOR] PN532 ready"); }
    else S.println("[ACTOR] PN532 not found");

    // Mesh
    sfge::meshInit(cfg.id, sfge::ROLE_ACTOR, onMeshMsg);

    buildUI();
    buzzOk();
}

void loop() {
    lv_timer_handler();
    sfge::eventRetryService();   // re-fire our story events until confirmed

    if (restartRequested) { restartRequested = false; delay(300); ESP.restart(); }

    if (toastUntil && millis() > toastUntil) {
        toastUntil = 0;
        if (actToast) lv_obj_add_flag(actToast, LV_OBJ_FLAG_HIDDEN);
    }

    // Roster line on the scan screen, refreshed every 2 s
    static unsigned long lastRoster = 0;
    if (millis() - lastRoster > 2000 && lv_scr_act() == scrScan) {
        lastRoster = millis();
        char line[256] = "";
        int n = 0;
        for (int i = 0; i < MAX_ROSTER; i++) {
            if (!roster[i].cs[0] || millis() - roster[i].heard > 60000) continue;
            if (n) strlcat(line, "\n", sizeof(line));
            char row[48];
            snprintf(row, sizeof(row), "%s  ·  %s  ·  %d CR", roster[i].cs,
                     roster[i].faction[0] ? roster[i].faction : "-", roster[i].score);
            strlcat(line, row, sizeof(line));
            n++;
        }
        lv_label_set_text(scanRoster, n ? line : "(no operatives heard yet)");
    }

    // NFC poll — only the scan screen reads cards
    static unsigned long lastNfc = 0;
    if (nfcOk && lv_scr_act() == scrScan && millis() - lastNfc > 250) {
        lastNfc = millis();
        uint8_t uid[7]; uint8_t uidLen;
        if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLen, 80)) {
            readNdefText();
            if (strncasecmp(ndefText, "PLAYER:", 7) == 0 && ndefText[7]) {
                buzzOk();
                openActionScreen(ndefText + 7);
            } else {
                buzzFail();
                lv_label_set_text(scanStatus, ndefText[0]
                    ? "NOT AN OPERATIVE CARD" : "UNREADABLE CARD — TRY AGAIN");
            }
        }
    }

    delay(5);
}
