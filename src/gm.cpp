/*
 * SWTS GM Datapad — Game Master command terminal
 * Target: Waveshare ESP32-S3-Touch-LCD-7 (800x480, GT911 touch, TCA9554 IO expander)
 *
 * Pinout:
 *   RGB Red:    GPIO 1, 2, 42, 41, 40   (R3-R7)
 *   RGB Green:  GPIO 39, 0, 45, 48, 47, 21 (G2-G7)
 *   RGB Blue:   GPIO 14, 38, 18, 17, 10  (B3-B7)
 *   HSYNC=46, VSYNC=3, DE=5, PCLK=7
 *   GT911 Touch I2C: SDA=8, SCL=9, INT=4
 *   IO Expander: TCA9554PWR @ 0x20 on same I2C (SDA=8, SCL=9)
 *     EXIO2 = LCD_BL, EXIO3 = LCD_RST, EXIO1 = TP_RST
 *   SD card: SDMMC interface (cards in slot)
 */

#include <Arduino.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/touch/Touch_GT911.hpp>
#include <lvgl.h>
#include <WiFi.h>
#include <Wire.h>
#include <SPI.h>
#include <SD_MMC.h>
#include <ArduinoJson.h>
#include "swts_mesh.h"

#define S Serial
#define W 800
#define H 480

// ═══════════════════════════════════════════════
//  DISPLAY — LovyanGFX RGB parallel + GT911 touch
// ═══════════════════════════════════════════════
class LGFX : public lgfx::LGFX_Device {
    lgfx::v1::Bus_RGB     _rgbBus;
    lgfx::v1::Panel_RGB   _rgbPanel;
    lgfx::v1::Light_PWM   _bl;
    lgfx::v1::Touch_GT911 _gt911;
public:
    LGFX() {
        {
            auto cfg = _rgbPanel.config();
            cfg.memory_width  = W;
            cfg.memory_height = H;
            cfg.panel_width   = W;
            cfg.panel_height  = H;
            cfg.offset_x      = 0;
            cfg.offset_y      = 0;
            _rgbPanel.config(cfg);
        }
        {
            auto cfg = _rgbPanel.config_detail();
            cfg.use_psram = true;
            _rgbPanel.config_detail(cfg);
        }
        {
            auto cfg = _rgbBus.config();
            cfg.panel = &_rgbPanel;

            // Blue 5 bits — B3..B7
            cfg.pin_d0 = GPIO_NUM_14;
            cfg.pin_d1 = GPIO_NUM_38;
            cfg.pin_d2 = GPIO_NUM_18;
            cfg.pin_d3 = GPIO_NUM_17;
            cfg.pin_d4 = GPIO_NUM_10;
            // Green 6 bits — G2..G7
            cfg.pin_d5 = GPIO_NUM_39;
            cfg.pin_d6 = GPIO_NUM_0;
            cfg.pin_d7 = GPIO_NUM_45;
            cfg.pin_d8 = GPIO_NUM_48;
            cfg.pin_d9 = GPIO_NUM_47;
            cfg.pin_d10 = GPIO_NUM_21;
            // Red 5 bits — R3..R7
            cfg.pin_d11 = GPIO_NUM_1;
            cfg.pin_d12 = GPIO_NUM_2;
            cfg.pin_d13 = GPIO_NUM_42;
            cfg.pin_d14 = GPIO_NUM_41;
            cfg.pin_d15 = GPIO_NUM_40;

            cfg.pin_henable = GPIO_NUM_5;   // DE
            cfg.pin_vsync   = GPIO_NUM_3;
            cfg.pin_hsync   = GPIO_NUM_46;
            cfg.pin_pclk    = GPIO_NUM_7;
            cfg.freq_write  = 12000000;

            // Sunton 7" 800x480 timings (commonly works on Waveshare 7" too)
            cfg.hsync_polarity    = 0;
            cfg.hsync_front_porch = 8;
            cfg.hsync_pulse_width = 4;
            cfg.hsync_back_porch  = 16;

            cfg.vsync_polarity    = 0;
            cfg.vsync_front_porch = 16;
            cfg.vsync_pulse_width = 4;
            cfg.vsync_back_porch  = 16;

            cfg.pclk_idle_high = 0;
            _rgbBus.config(cfg);
            _rgbPanel.setBus(&_rgbBus);
        }
        {
            auto cfg = _bl.config();
            cfg.pin_bl = GPIO_NUM_2;
            cfg.freq   = 44100;
            cfg.pwm_channel = 7;
            cfg.invert = false;
            _bl.config(cfg);
            _rgbPanel.setLight(&_bl);
        }
        {
            auto cfg = _gt911.config();
            cfg.x_min = 0; cfg.x_max = W - 1;
            cfg.y_min = 0; cfg.y_max = H - 1;
            cfg.pin_int = -1;
            cfg.pin_rst = -1;
            cfg.bus_shared = false;
            cfg.offset_rotation = 0;
            cfg.i2c_port = 1;
            cfg.pin_sda = GPIO_NUM_8;
            cfg.pin_scl = GPIO_NUM_9;
            cfg.freq = 400000;
            cfg.i2c_addr = 0x14;  // GT911 default (sometimes 0x5D)
            _gt911.config(cfg);
            _rgbPanel.setTouch(&_gt911);
        }
        setPanel(&_rgbPanel);
    }
};
LGFX tft;

// ═══════════════════════════════════════════════
//  PALETTE — Star Wars amber/cyan command center
// ═══════════════════════════════════════════════
#define C_BG         lv_color_hex(0x040608)
#define C_PNL        lv_color_hex(0x0C1218)
#define C_PNL2       lv_color_hex(0x141C28)
#define C_FRM        lv_color_hex(0x283848)
#define C_AMB        lv_color_hex(0xE8A820)
#define C_AMB_DIM    lv_color_hex(0x6A4810)
#define C_AMB_BRT    lv_color_hex(0xFFD050)
#define C_CYN        lv_color_hex(0x38B4C8)
#define C_CYN_BRT    lv_color_hex(0x88E0F0)
#define C_GRN        lv_color_hex(0x30C868)
#define C_RED        lv_color_hex(0xE83830)
#define C_TXT        lv_color_hex(0xC8D0E0)
#define C_DIM        lv_color_hex(0x5A6878)
#define C_MUT        lv_color_hex(0x303848)
#define C_WHITE      lv_color_hex(0xFFFFFF)
#define C_PURPLE     lv_color_hex(0xA070E0)

// ═══════════════════════════════════════════════
//  TRACKED DEVICES (heard via mesh)
// ═══════════════════════════════════════════════
struct TrackedDevice {
    char id[16];
    uint8_t role;
    unsigned long lastHeard;
    int score;
    int activeMsn;
    int scans;
    int slicesWon;
    bool active;
};

#define MAX_DEVICES 32
TrackedDevice trackedDevs[MAX_DEVICES];
int deviceCount = 0;

// Activity log — recent events for dashboard feed
#define MAX_ACTIVITY 12
struct ActivityEntry {
    char text[80];
    uint8_t icon;       // 0=event 1=score 2=scan 3=slice 4=alert 5=info
    unsigned long when;
};
ActivityEntry activityLog[MAX_ACTIVITY];
int activityHead = 0;     // next write slot
int activityCount = 0;

void logActivity(const char *text, uint8_t icon) {
    ActivityEntry &e = activityLog[activityHead];
    strlcpy(e.text, text, sizeof(e.text));
    e.icon = icon;
    e.when = millis();
    activityHead = (activityHead + 1) % MAX_ACTIVITY;
    if (activityCount < MAX_ACTIVITY) activityCount++;
}

TrackedDevice* findOrAddDevice(const char *id, uint8_t role) {
    for (int i = 0; i < deviceCount; i++)
        if (strcmp(trackedDevs[i].id, id) == 0) return &trackedDevs[i];
    if (deviceCount >= MAX_DEVICES) return nullptr;
    TrackedDevice &d = trackedDevs[deviceCount];
    memset(&d, 0, sizeof(d));
    strlcpy(d.id, id, sizeof(d.id));
    d.role = role;
    d.active = true;
    deviceCount++;
    S.printf("[GM] New device: %s role=%d\n", id, role);
    return &d;
}

// ═══════════════════════════════════════════════
//  GM CONFIG (from /SWTS/gm_config.json)
// ═══════════════════════════════════════════════
struct GmConfig {
    char scenario[32] = "Rebel Extraction";
    char planet[24]   = "Tatooine";
    char gm_id[16]    = "GM-1";
    int  max_score    = 1000;
};
GmConfig gmConfig;

#define MAX_TEMPLATES 16
#define MAX_CLUES 5
struct EventTemplate { char id[24]; char name[40]; uint8_t severity; char description[80]; };
struct CommTemplate  { char id[24]; char from[24]; char subject[40]; char body[120]; };
struct BountyTemplate{
    char id[24];
    char target_name[24];
    char description[60];
    int  reward;
    char clues[MAX_CLUES][160];
    uint8_t clueCount;
    // runtime tracking state on GM only
    bool    posted;
    uint8_t cluesSent;
    bool    closed;
};

EventTemplate  eventTemplates[MAX_TEMPLATES];   int eventCount = 0;
CommTemplate   commTemplates[MAX_TEMPLATES];    int commCount = 0;
BountyTemplate bountyTemplates[MAX_TEMPLATES];  int bountyCount = 0;

bool loadGmConfig() {
    File f = SD_MMC.open("/SWTS/gm_config.json", FILE_READ);
    if (!f) {
        S.println("[GM] gm_config.json not found on SD");
        return false;
    }
    S.printf("[GM] Loading gm_config.json (%u bytes)\n", (unsigned)f.size());
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) { S.printf("[GM] JSON parse error: %s\n", err.c_str()); return false; }

    strlcpy(gmConfig.scenario, doc["scenario"]["name"] | "Unknown", sizeof(gmConfig.scenario));
    strlcpy(gmConfig.planet,   doc["scenario"]["planet"] | "Unknown", sizeof(gmConfig.planet));
    strlcpy(gmConfig.gm_id,    doc["device"]["id"] | "GM-1", sizeof(gmConfig.gm_id));
    gmConfig.max_score = doc["game"]["max_score"] | 1000;

    eventCount = 0;
    for (JsonObject e : doc["events"].as<JsonArray>()) {
        if (eventCount >= MAX_TEMPLATES) break;
        EventTemplate &t = eventTemplates[eventCount++];
        strlcpy(t.id, e["id"] | "", sizeof(t.id));
        strlcpy(t.name, e["name"] | "", sizeof(t.name));
        t.severity = e["severity"] | 1;
        strlcpy(t.description, e["description"] | "", sizeof(t.description));
    }
    commCount = 0;
    for (JsonObject c : doc["comms"].as<JsonArray>()) {
        if (commCount >= MAX_TEMPLATES) break;
        CommTemplate &t = commTemplates[commCount++];
        strlcpy(t.id, c["id"] | "", sizeof(t.id));
        strlcpy(t.from, c["from"] | "GM", sizeof(t.from));
        strlcpy(t.subject, c["subject"] | "", sizeof(t.subject));
        strlcpy(t.body, c["body"] | "", sizeof(t.body));
    }
    bountyCount = 0;
    for (JsonObject b : doc["bounties"].as<JsonArray>()) {
        if (bountyCount >= MAX_TEMPLATES) break;
        BountyTemplate &t = bountyTemplates[bountyCount++];
        memset(&t, 0, sizeof(t));
        strlcpy(t.id, b["id"] | "", sizeof(t.id));
        strlcpy(t.target_name, b["target_name"] | "", sizeof(t.target_name));
        strlcpy(t.description, b["description"] | "", sizeof(t.description));
        t.reward = b["reward"] | 100;
        t.clueCount = 0;
        for (const char *cl : b["clues"].as<JsonArray>()) {
            if (t.clueCount >= MAX_CLUES) break;
            strlcpy(t.clues[t.clueCount++], cl ? cl : "", 160);
        }
    }
    S.printf("[GM] Config: %d events, %d comms, %d bounties\n", eventCount, commCount, bountyCount);
    return true;
}

// ═══════════════════════════════════════════════
//  GM STATE PERSISTENCE — /SWTS/gm_state.json
//  Bounty posting/clue/closed status + last-known leaderboard.
//  Survives reboot; supports swapping SD into a spare GM mid-event.
// ═══════════════════════════════════════════════
volatile bool gmDirty = false;
unsigned long gmLastSave = 0;
#define GM_SAVE_DEBOUNCE_MS 3000

extern bool sdOk;    // defined later, near setup()
bool saveGmState() {
    if (!sdOk) return false;
    File f = SD_MMC.open("/SWTS/gm_state.json", FILE_WRITE);
    if (!f) { S.println("[GM] save_state: open failed"); return false; }

    JsonDocument doc;
    doc["scenario"] = gmConfig.scenario;

    JsonArray b = doc["bounties"].to<JsonArray>();
    for (int i = 0; i < bountyCount; i++) {
        BountyTemplate &t = bountyTemplates[i];
        if (!t.posted && !t.closed) continue;   // only persist non-default
        JsonObject o = b.add<JsonObject>();
        o["id"]        = t.id;
        o["posted"]    = t.posted;
        o["cluesSent"] = t.cluesSent;
        o["closed"]    = t.closed;
    }

    JsonArray d = doc["devices"].to<JsonArray>();
    for (int i = 0; i < deviceCount; i++) {
        TrackedDevice &td = trackedDevs[i];
        JsonObject o = d.add<JsonObject>();
        o["id"]        = td.id;
        o["role"]      = td.role;
        o["score"]     = td.score;
        o["scans"]     = td.scans;
        o["slicesWon"] = td.slicesWon;
        o["activeMsn"] = td.activeMsn;
    }

    serializeJson(doc, f);
    f.close();
    gmLastSave = millis();
    return true;
}

bool loadGmState() {
    File f = SD_MMC.open("/SWTS/gm_state.json", FILE_READ);
    if (!f) { S.println("[GM] No gm_state.json yet (first boot or fresh card)"); return false; }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) { S.printf("[GM] state parse error: %s\n", err.c_str()); return false; }

    // Restore bounty state
    for (JsonObject o : doc["bounties"].as<JsonArray>()) {
        const char *id = o["id"] | "";
        for (int i = 0; i < bountyCount; i++) {
            if (strcmp(bountyTemplates[i].id, id) == 0) {
                bountyTemplates[i].posted    = o["posted"]    | false;
                bountyTemplates[i].cluesSent = o["cluesSent"] | 0;
                bountyTemplates[i].closed    = o["closed"]    | false;
                break;
            }
        }
    }

    // Restore last-known device snapshot. lastHeard reset to 0 so they read as "stale"
    // until they re-ping or respond to the boot-time sync request.
    deviceCount = 0;
    for (JsonObject o : doc["devices"].as<JsonArray>()) {
        if (deviceCount >= MAX_DEVICES) break;
        TrackedDevice &td = trackedDevs[deviceCount];
        memset(&td, 0, sizeof(td));
        strlcpy(td.id, o["id"] | "", sizeof(td.id));
        td.role      = o["role"]      | 0;
        td.score     = o["score"]     | 0;
        td.scans     = o["scans"]     | 0;
        td.slicesWon = o["slicesWon"] | 0;
        td.activeMsn = o["activeMsn"] | 0;
        td.lastHeard = 0;
        td.active    = false;
        deviceCount++;
    }

    int postedN = 0, closedN = 0;
    for (int i = 0; i < bountyCount; i++) {
        if (bountyTemplates[i].posted && !bountyTemplates[i].closed) postedN++;
        if (bountyTemplates[i].closed) closedN++;
    }
    S.printf("[GM] State restored: %d devices, %d posted bounties, %d closed\n",
             deviceCount, postedN, closedN);
    return true;
}

// ═══════════════════════════════════════════════
//  MESH HANDLER
// ═══════════════════════════════════════════════
void onMeshMsg(const swts::MeshHeader *hdr, const uint8_t *payload, int len) {
    TrackedDevice *d = findOrAddDevice(hdr->from_id, hdr->role);
    if (!d) return;
    bool wasInactive = !d->active || (millis() - d->lastHeard > 30000);
    d->lastHeard = millis();
    d->active = true;

    char log[80];

    if (wasInactive) {
        snprintf(log, sizeof(log), "%s came online", hdr->from_id);
        logActivity(log, 5);
    }

    if (hdr->type == swts::MSG_STATUS && len >= (int)sizeof(swts::MeshStatus)) {
        const swts::MeshStatus *s = (const swts::MeshStatus *)payload;
        d->score = s->score;
        d->activeMsn = s->mission_active;
        d->scans = s->scans;
        d->slicesWon = s->slices_won;
        gmDirty = true;
    }
    else if (hdr->type == swts::MSG_SCORE && len >= (int)sizeof(swts::MeshScore)) {
        const swts::MeshScore *s = (const swts::MeshScore *)payload;
        d->score = s->new_score;
        snprintf(log, sizeof(log), "%s scored %+d (%s)", hdr->from_id, s->delta, s->reason);
        logActivity(log, 1);
        gmDirty = true;
    }
    else if (hdr->type == swts::MSG_NFC_SCAN && len >= (int)sizeof(swts::MeshNfcScan)) {
        const swts::MeshNfcScan *n = (const swts::MeshNfcScan *)payload;
        snprintf(log, sizeof(log), "%s scanned %s", hdr->from_id, n->tag_name);
        logActivity(log, 2);
    }
    else if (hdr->type == swts::MSG_SLICE_RESULT && len >= (int)sizeof(swts::MeshSliceResult)) {
        const swts::MeshSliceResult *r = (const swts::MeshSliceResult *)payload;
        snprintf(log, sizeof(log), "%s %s slice on %s", hdr->from_id, r->won ? "WON" : "FAILED", r->panel_id);
        logActivity(log, 3);
        if (r->won) { d->slicesWon++; gmDirty = true; }
    }
}

// ═══════════════════════════════════════════════
//  LVGL bridge
// ═══════════════════════════════════════════════
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1 = nullptr;

void lvgl_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    tft.pushImageDMA(area->x1, area->y1, w, h, (uint16_t *)px);
    lv_disp_flush_ready(drv);
}

void lvgl_touch(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    uint16_t x, y;
    if (tft.getTouch(&x, &y)) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ═══════════════════════════════════════════════
//  UI
// ═══════════════════════════════════════════════
lv_obj_t *tabview = nullptr;
lv_obj_t *playerList = nullptr;       // leaderboard list (assigned players)
lv_obj_t *unassignedList = nullptr;   // unassigned datapads
lv_obj_t *unassignedBtnLbl = nullptr; // for "UNASSIGNED (N)" badge in toggle

// Mirror of datapad's predicate. Anything matching "DATAPAD-*" or "OPERATIVE" is unclaimed.
inline bool gmIsUnassignedCallsign(const char *cs) {
    if (!cs || !cs[0]) return true;
    if (strcmp(cs, "OPERATIVE") == 0) return true;
    if (strncmp(cs, "DATAPAD-", 8) == 0) return true;
    return false;
}
lv_obj_t *panelList = nullptr;
lv_obj_t *statusLabel = nullptr;

static void ev_trigger_event(lv_event_t *e) {
    EventTemplate *t = (EventTemplate *)lv_event_get_user_data(e);
    swts::gmTriggerEvent(t->id, t->name, t->severity, t->description);
    char log[80]; snprintf(log, sizeof(log), "EVENT: %s broadcast", t->name);
    logActivity(log, 0);
    S.printf("[GM] Event: %s\n", t->name);
}
static void ev_push_comm(lv_event_t *e) {
    CommTemplate *t = (CommTemplate *)lv_event_get_user_data(e);
    swts::gmPushComm(t->id, "", t->from, t->subject, t->body);
    char log[80]; snprintf(log, sizeof(log), "COMM sent: %s", t->subject);
    logActivity(log, 4);
    S.printf("[GM] Comm: %s\n", t->subject);
}
// Forward decls for bounty detail modal
void openBountyDetail(BountyTemplate *t);
static void refreshBountyCards();

static void ev_open_bounty_detail(lv_event_t *e) {
    BountyTemplate *t = (BountyTemplate *)lv_event_get_user_data(e);
    openBountyDetail(t);
}

// Dashboard widgets (updated each refresh)
lv_obj_t *dashTop3 = nullptr;
lv_obj_t *dashPanelsRow = nullptr;
lv_obj_t *dashActivity = nullptr;
lv_obj_t *dashStats = nullptr;

void buildDashboardTab(lv_obj_t *tab) {
    lv_obj_set_style_bg_color(tab, C_BG, 0);
    lv_obj_set_style_pad_all(tab, 12, 0);

    // ── TOP STATS ROW ──
    lv_obj_t *statsBar = lv_obj_create(tab);
    lv_obj_set_size(statsBar, lv_pct(100), 70);
    lv_obj_set_pos(statsBar, 0, 0);
    lv_obj_set_style_bg_color(statsBar, C_PNL, 0);
    lv_obj_set_style_border_color(statsBar, C_AMB, 0);
    lv_obj_set_style_border_side(statsBar, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(statsBar, 4, 0);
    lv_obj_set_style_radius(statsBar, 4, 0);
    lv_obj_clear_flag(statsBar, LV_OBJ_FLAG_SCROLLABLE);

    dashStats = lv_label_create(statsBar);
    lv_label_set_text(dashStats, "INITIALIZING...");
    lv_obj_set_style_text_color(dashStats, C_WHITE, 0);
    lv_obj_set_style_text_font(dashStats, &lv_font_montserrat_24, 0);
    lv_obj_align(dashStats, LV_ALIGN_LEFT_MID, 16, 0);

    // ── TOP 3 LEADERBOARD (left half) ──
    lv_obj_t *leadSection = lv_obj_create(tab);
    lv_obj_set_size(leadSection, 380, 230);
    lv_obj_set_pos(leadSection, 0, 78);
    lv_obj_set_style_bg_color(leadSection, C_PNL, 0);
    lv_obj_set_style_border_color(leadSection, C_AMB_DIM, 0);
    lv_obj_set_style_border_width(leadSection, 1, 0);
    lv_obj_set_style_radius(leadSection, 4, 0);
    lv_obj_set_style_pad_all(leadSection, 12, 0);
    lv_obj_clear_flag(leadSection, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lh = lv_label_create(leadSection);
    lv_label_set_text(lh, "TOP OPERATIVES");
    lv_obj_set_style_text_color(lh, C_AMB_BRT, 0);
    lv_obj_set_style_text_font(lh, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(lh, 0, 0);

    dashTop3 = lv_obj_create(leadSection);
    lv_obj_set_size(dashTop3, lv_pct(100), 180);
    lv_obj_set_pos(dashTop3, 0, 30);
    lv_obj_set_style_bg_opa(dashTop3, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dashTop3, 0, 0);
    lv_obj_set_style_pad_all(dashTop3, 0, 0);
    lv_obj_set_flex_flow(dashTop3, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(dashTop3, 4, 0);

    // ── PANELS GRID (right half top) ──
    lv_obj_t *panelSection = lv_obj_create(tab);
    lv_obj_set_size(panelSection, 380, 230);
    lv_obj_set_pos(panelSection, 390, 78);
    lv_obj_set_style_bg_color(panelSection, C_PNL, 0);
    lv_obj_set_style_border_color(panelSection, C_CYN, 0);
    lv_obj_set_style_border_side(panelSection, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(panelSection, 4, 0);
    lv_obj_set_style_radius(panelSection, 4, 0);
    lv_obj_set_style_pad_all(panelSection, 12, 0);
    lv_obj_clear_flag(panelSection, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ph = lv_label_create(panelSection);
    lv_label_set_text(ph, "PROP NETWORK");
    lv_obj_set_style_text_color(ph, C_CYN_BRT, 0);
    lv_obj_set_style_text_font(ph, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(ph, 0, 0);

    dashPanelsRow = lv_obj_create(panelSection);
    lv_obj_set_size(dashPanelsRow, lv_pct(100), 180);
    lv_obj_set_pos(dashPanelsRow, 0, 30);
    lv_obj_set_style_bg_opa(dashPanelsRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dashPanelsRow, 0, 0);
    lv_obj_set_style_pad_all(dashPanelsRow, 0, 0);
    lv_obj_set_flex_flow(dashPanelsRow, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(dashPanelsRow, 6, 0);

    // ── ACTIVITY FEED (bottom full width) ──
    lv_obj_t *actSection = lv_obj_create(tab);
    lv_obj_set_size(actSection, lv_pct(100), 100);
    lv_obj_set_pos(actSection, 0, 314);
    lv_obj_set_style_bg_color(actSection, C_PNL, 0);
    lv_obj_set_style_border_color(actSection, C_PURPLE, 0);
    lv_obj_set_style_border_side(actSection, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(actSection, 4, 0);
    lv_obj_set_style_radius(actSection, 4, 0);
    lv_obj_set_style_pad_all(actSection, 12, 0);
    lv_obj_clear_flag(actSection, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ah = lv_label_create(actSection);
    lv_label_set_text(ah, "LIVE ACTIVITY");
    lv_obj_set_style_text_color(ah, C_AMB_BRT, 0);
    lv_obj_set_style_text_font(ah, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(ah, 0, 0);

    dashActivity = lv_obj_create(actSection);
    lv_obj_set_size(dashActivity, lv_pct(100), 60);
    lv_obj_set_pos(dashActivity, 0, 24);
    lv_obj_set_style_bg_opa(dashActivity, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dashActivity, 0, 0);
    lv_obj_set_style_pad_all(dashActivity, 0, 0);
    lv_obj_set_flex_flow(dashActivity, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(dashActivity, 2, 0);
}

void refreshDashboard() {
    if (!dashTop3 || !dashPanelsRow || !dashActivity || !dashStats) return;

    // Stats bar
    int onP = 0, onD = 0;
    for (int i = 0; i < deviceCount; i++) {
        bool live = (millis() - trackedDevs[i].lastHeard < 30000);
        if (live && trackedDevs[i].role == swts::ROLE_PANEL) onP++;
        if (live && trackedDevs[i].role == swts::ROLE_DATAPAD) onD++;
    }
    unsigned long up = millis() / 1000;
    char sb[120];
    snprintf(sb, sizeof(sb), "%s   //   %s   //   %d PLAYERS   //   %d PANELS   //   %02lu:%02lu:%02lu",
             gmConfig.scenario, gmConfig.planet, onD, onP, up/3600, (up/60)%60, up%60);
    lv_label_set_text(dashStats, sb);

    // Top 3 leaderboard
    lv_obj_clean(dashTop3);
    int idx[MAX_DEVICES]; int n = 0;
    for (int i = 0; i < deviceCount; i++)
        if (trackedDevs[i].role == swts::ROLE_DATAPAD) idx[n++] = i;
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (trackedDevs[idx[i]].score < trackedDevs[idx[j]].score) {
                int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
            }
    int show = n < 3 ? n : 3;
    if (show == 0) {
        lv_obj_t *empty = lv_label_create(dashTop3);
        lv_label_set_text(empty, "(no players online)");
        lv_obj_set_style_text_color(empty, C_DIM, 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
    }
    lv_color_t medalColors[] = {C_AMB_BRT, lv_color_hex(0xC0C0C0), lv_color_hex(0xCD7F32)};
    for (int rank = 0; rank < show; rank++) {
        TrackedDevice &d = trackedDevs[idx[rank]];
        lv_obj_t *row = lv_obj_create(dashTop3);
        lv_obj_set_size(row, lv_pct(100), 50);
        lv_obj_set_style_bg_color(row, C_PNL2, 0);
        lv_obj_set_style_border_color(row, medalColors[rank], 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(row, 3, 0);
        lv_obj_set_style_radius(row, 2, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        char r[8]; snprintf(r, sizeof(r), "%d", rank + 1);
        lv_obj_t *rk = lv_label_create(row);
        lv_label_set_text(rk, r);
        lv_obj_set_style_text_color(rk, medalColors[rank], 0);
        lv_obj_set_style_text_font(rk, &lv_font_montserrat_28, 0);
        lv_obj_set_pos(rk, 10, 6);

        lv_obj_t *n = lv_label_create(row);
        lv_label_set_text(n, d.id);
        lv_obj_set_style_text_color(n, C_WHITE, 0);
        lv_obj_set_style_text_font(n, &lv_font_montserrat_20, 0);
        lv_obj_set_pos(n, 48, 12);

        char sb2[16]; snprintf(sb2, sizeof(sb2), "%d", d.score);
        lv_obj_t *sc = lv_label_create(row);
        lv_label_set_text(sc, sb2);
        lv_obj_set_style_text_color(sc, C_AMB_BRT, 0);
        lv_obj_set_style_text_font(sc, &lv_font_montserrat_24, 0);
        lv_obj_align(sc, LV_ALIGN_RIGHT_MID, -16, 0);
    }

    // Panel grid (small status pills)
    lv_obj_clean(dashPanelsRow);
    int pn = 0;
    for (int i = 0; i < deviceCount; i++) {
        if (trackedDevs[i].role != swts::ROLE_PANEL) continue;
        TrackedDevice &d = trackedDevs[i];
        bool stale = (millis() - d.lastHeard > 30000);
        lv_obj_t *pill = lv_obj_create(dashPanelsRow);
        lv_obj_set_size(pill, 110, 50);
        lv_obj_set_style_bg_color(pill, C_PNL2, 0);
        lv_obj_set_style_border_color(pill, stale ? C_RED : C_GRN, 0);
        lv_obj_set_style_border_width(pill, 2, 0);
        lv_obj_set_style_radius(pill, 4, 0);
        lv_obj_set_style_pad_all(pill, 4, 0);
        lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *nm = lv_label_create(pill);
        lv_label_set_text(nm, d.id);
        lv_obj_set_style_text_color(nm, C_WHITE, 0);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(nm, 4, 2);

        lv_obj_t *st = lv_label_create(pill);
        lv_label_set_text(st, stale ? "OFFLINE" : "ONLINE");
        lv_obj_set_style_text_color(st, stale ? C_RED : C_GRN, 0);
        lv_obj_set_style_text_font(st, &lv_font_montserrat_12, 0);
        lv_obj_set_pos(st, 4, 24);
        pn++;
    }
    if (pn == 0) {
        lv_obj_t *empty = lv_label_create(dashPanelsRow);
        lv_label_set_text(empty, "(no panels online)");
        lv_obj_set_style_text_color(empty, C_DIM, 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
    }

    // Activity feed (last 4 entries newest first)
    lv_obj_clean(dashActivity);
    lv_color_t iconCols[] = {C_AMB, C_GRN, C_CYN, C_PURPLE, C_RED, C_DIM};
    const char *iconChars[] = {"!", "+", "*", "/", "#", ">"};
    int shown = activityCount < 4 ? activityCount : 4;
    for (int i = 0; i < shown; i++) {
        int slot = (activityHead - 1 - i + MAX_ACTIVITY) % MAX_ACTIVITY;
        ActivityEntry &e = activityLog[slot];
        lv_obj_t *row = lv_obj_create(dashActivity);
        lv_obj_set_size(row, lv_pct(100), 14);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *ic = lv_label_create(row);
        lv_label_set_text(ic, iconChars[e.icon < 6 ? e.icon : 5]);
        lv_obj_set_style_text_color(ic, iconCols[e.icon < 6 ? e.icon : 5], 0);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(ic, 0, 0);

        lv_obj_t *tx = lv_label_create(row);
        lv_label_set_text(tx, e.text);
        lv_obj_set_style_text_color(tx, C_TXT, 0);
        lv_obj_set_style_text_font(tx, &lv_font_montserrat_12, 0);
        lv_obj_set_pos(tx, 20, 1);

        unsigned long age = (millis() - e.when) / 1000;
        char a[8];
        if (age < 60) snprintf(a, sizeof(a), "%lus", age);
        else snprintf(a, sizeof(a), "%lum", age / 60);
        lv_obj_t *ag = lv_label_create(row);
        lv_label_set_text(ag, a);
        lv_obj_set_style_text_color(ag, C_DIM, 0);
        lv_obj_set_style_text_font(ag, &lv_font_montserrat_12, 0);
        lv_obj_align(ag, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    if (shown == 0) {
        lv_obj_t *empty = lv_label_create(dashActivity);
        lv_label_set_text(empty, "(waiting for activity...)");
        lv_obj_set_style_text_color(empty, C_DIM, 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
    }
}

// ═══════════════════════════════════════════════
//  BOUNTY DETAIL MODAL
// ═══════════════════════════════════════════════
static lv_obj_t *bountyModal = nullptr;
static BountyTemplate *modalBounty = nullptr;
static lv_obj_t *modalStatusPill = nullptr;
static lv_obj_t *modalClueList = nullptr;
static lv_obj_t *modalPlayerList = nullptr;
static lv_obj_t *modalPostBtn = nullptr;

static void closeBountyModal(lv_event_t *e) {
    if (bountyModal) {
        lv_obj_del(bountyModal);
        bountyModal = nullptr;
        modalBounty = nullptr;
        modalStatusPill = nullptr;
        modalClueList = nullptr;
        modalPlayerList = nullptr;
        modalPostBtn = nullptr;
        refreshBountyCards();
    }
}

static void refreshModalStatus();
static void refreshModalClues();
static void refreshModalPlayers();

static void ev_post_bounty(lv_event_t *e) {
    if (!modalBounty || modalBounty->posted) return;
    BountyTemplate *t = modalBounty;
    swts::gmPushBounty(t->id, "", t->target_name, t->description, t->reward);
    t->posted = true;
    t->cluesSent = 0;
    t->closed = false;
    gmDirty = true;
    char log[80]; snprintf(log, sizeof(log), "BOUNTY posted: %s (%d)", t->target_name, t->reward);
    logActivity(log, 4);
    S.printf("[GM] Bounty posted: %s\n", t->target_name);
    refreshModalStatus();
    refreshModalClues();
    refreshModalPlayers();
}

static void ev_send_clue(lv_event_t *e) {
    if (!modalBounty || !modalBounty->posted || modalBounty->closed) return;
    int clueIdx = (int)(intptr_t)lv_event_get_user_data(e);
    if (clueIdx < 0 || clueIdx >= modalBounty->clueCount) return;
    BountyTemplate *t = modalBounty;
    swts::gmPushBountyClue(t->id, clueIdx + 1, t->clues[clueIdx]);
    if (clueIdx + 1 > t->cluesSent) { t->cluesSent = clueIdx + 1; gmDirty = true; }
    char log[80]; snprintf(log, sizeof(log), "CLUE %d pushed: %s", clueIdx + 1, t->target_name);
    logActivity(log, 4);
    S.printf("[GM] Clue %d sent: %s\n", clueIdx + 1, t->clues[clueIdx]);
    refreshModalClues();
}

static void ev_award_to(lv_event_t *e) {
    if (!modalBounty || !modalBounty->posted || modalBounty->closed) return;
    TrackedDevice *d = (TrackedDevice *)lv_event_get_user_data(e);
    if (!d) return;
    BountyTemplate *t = modalBounty;
    swts::gmClaimBounty(t->id, d->id, d->id, t->reward);
    d->score += t->reward;   // GM keeps live mirror
    t->closed = true;
    gmDirty = true;
    char log[80]; snprintf(log, sizeof(log), "BOUNTY %s awarded to %s (+%d)", t->target_name, d->id, t->reward);
    logActivity(log, 1);
    S.printf("[GM] Bounty awarded: %s -> %s\n", t->target_name, d->id);
    refreshModalStatus();
    refreshModalPlayers();
}

static void ev_cancel_bounty(lv_event_t *e) {
    if (!modalBounty || modalBounty->closed) return;
    BountyTemplate *t = modalBounty;
    if (t->posted) swts::gmCancelBounty(t->id, "expired");
    t->closed = true;
    gmDirty = true;
    char log[80]; snprintf(log, sizeof(log), "BOUNTY %s closed (no winner)", t->target_name);
    logActivity(log, 5);
    refreshModalStatus();
    refreshModalPlayers();
}

static void refreshModalStatus() {
    if (!modalStatusPill || !modalBounty) return;
    BountyTemplate *t = modalBounty;
    const char *txt;
    lv_color_t col;
    if (t->closed)      { txt = "CLOSED";  col = C_DIM; }
    else if (t->posted) { txt = "POSTED";  col = C_AMB_BRT; }
    else                { txt = "DRAFT";   col = C_RED; }
    lv_label_set_text(modalStatusPill, txt);
    lv_obj_set_style_text_color(modalStatusPill, col, 0);
    if (modalPostBtn) {
        if (t->posted || t->closed) lv_obj_add_state(modalPostBtn, LV_STATE_DISABLED);
        else lv_obj_clear_state(modalPostBtn, LV_STATE_DISABLED);
    }
}

static void refreshModalClues() {
    if (!modalClueList || !modalBounty) return;
    lv_obj_clean(modalClueList);
    BountyTemplate *t = modalBounty;
    if (t->clueCount == 0) {
        lv_obj_t *empty = lv_label_create(modalClueList);
        lv_label_set_text(empty, "(no clues defined in gm_config.json)");
        lv_obj_set_style_text_color(empty, C_DIM, 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
        return;
    }
    for (int i = 0; i < t->clueCount; i++) {
        bool sent = (i < t->cluesSent);
        bool canSend = t->posted && !t->closed && !sent;

        lv_obj_t *row = lv_obj_create(modalClueList);
        lv_obj_set_size(row, lv_pct(100), 56);
        lv_obj_set_style_bg_color(row, C_PNL2, 0);
        lv_obj_set_style_border_color(row, sent ? C_AMB : C_AMB_DIM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_radius(row, 2, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        char hd[16]; snprintf(hd, sizeof(hd), "CLUE %d", i + 1);
        lv_obj_t *hl = lv_label_create(row);
        lv_label_set_text(hl, hd);
        lv_obj_set_style_text_color(hl, sent ? C_AMB_BRT : C_DIM, 0);
        lv_obj_set_style_text_font(hl, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(hl, 0, 0);

        lv_obj_t *tx = lv_label_create(row);
        lv_label_set_text(tx, t->clues[i]);
        lv_obj_set_style_text_color(tx, C_TXT, 0);
        lv_obj_set_style_text_font(tx, &lv_font_montserrat_14, 0);
        lv_obj_set_width(tx, 460);
        lv_label_set_long_mode(tx, LV_LABEL_LONG_WRAP);
        lv_obj_set_pos(tx, 80, 0);

        lv_obj_t *btn = lv_btn_create(row);
        lv_obj_set_size(btn, 110, 40);
        lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(btn, sent ? C_PNL : C_AMB_DIM, 0);
        lv_obj_set_style_bg_color(btn, C_AMB, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, sent ? C_AMB : C_AMB_BRT, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        if (canSend) lv_obj_add_event_cb(btn, ev_send_clue, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        else lv_obj_add_state(btn, LV_STATE_DISABLED);

        lv_obj_t *bl = lv_label_create(btn);
        lv_label_set_text(bl, sent ? "PUSHED" : "PUSH");
        lv_obj_set_style_text_color(bl, sent ? C_AMB_BRT : C_BG, 0);
        lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
        lv_obj_center(bl);
    }
}

static void refreshModalPlayers() {
    if (!modalPlayerList || !modalBounty) return;
    lv_obj_clean(modalPlayerList);
    BountyTemplate *t = modalBounty;
    int shown = 0;
    for (int i = 0; i < deviceCount; i++) {
        if (trackedDevs[i].role != swts::ROLE_DATAPAD) continue;
        TrackedDevice *d = &trackedDevs[i];
        bool stale = (millis() - d->lastHeard > 60000);
        bool canAward = t->posted && !t->closed;

        lv_obj_t *row = lv_btn_create(modalPlayerList);
        lv_obj_set_size(row, lv_pct(48), 56);
        lv_obj_set_style_bg_color(row, C_PNL2, 0);
        lv_obj_set_style_bg_color(row, C_PNL, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(row, stale ? C_DIM : C_GRN, 0);
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_pad_all(row, 8, 0);
        if (canAward) lv_obj_add_event_cb(row, ev_award_to, LV_EVENT_CLICKED, d);
        else lv_obj_add_state(row, LV_STATE_DISABLED);

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, d->id);
        lv_obj_set_style_text_color(nm, C_WHITE, 0);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_18, 0);
        lv_obj_set_pos(nm, 0, 0);

        char ss[40]; snprintf(ss, sizeof(ss), "%d %s   //   AWARD %+d", d->score, gmConfig.scenario[0] ? "" : "", t->reward);
        lv_obj_t *sc = lv_label_create(row);
        lv_label_set_text(sc, ss);
        lv_obj_set_style_text_color(sc, stale ? C_DIM : C_AMB_BRT, 0);
        lv_obj_set_style_text_font(sc, &lv_font_montserrat_12, 0);
        lv_obj_set_pos(sc, 0, 24);

        shown++;
    }
    if (shown == 0) {
        lv_obj_t *empty = lv_label_create(modalPlayerList);
        lv_label_set_text(empty, "(no players online — cannot award)");
        lv_obj_set_style_text_color(empty, C_DIM, 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
    }
}

void openBountyDetail(BountyTemplate *t) {
    if (bountyModal) return;
    modalBounty = t;

    bountyModal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(bountyModal, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(bountyModal, 0, 0);
    lv_obj_set_style_bg_color(bountyModal, C_BG, 0);
    lv_obj_set_style_bg_opa(bountyModal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bountyModal, 0, 0);
    lv_obj_set_style_pad_all(bountyModal, 18, 0);
    lv_obj_clear_flag(bountyModal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(bountyModal);

    // Header strip
    lv_obj_t *wanted = lv_label_create(bountyModal);
    lv_label_set_text(wanted, "WANTED");
    lv_obj_set_style_text_color(wanted, C_RED, 0);
    lv_obj_set_style_text_font(wanted, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(wanted, 0, 0);

    modalStatusPill = lv_label_create(bountyModal);
    lv_label_set_text(modalStatusPill, "DRAFT");
    lv_obj_set_style_text_color(modalStatusPill, C_RED, 0);
    lv_obj_set_style_text_font(modalStatusPill, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(modalStatusPill, 130, 4);

    lv_obj_t *closeBtn = lv_btn_create(bountyModal);
    lv_obj_set_size(closeBtn, 80, 40);
    lv_obj_align(closeBtn, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(closeBtn, C_PNL2, 0);
    lv_obj_set_style_border_color(closeBtn, C_DIM, 0);
    lv_obj_set_style_border_width(closeBtn, 1, 0);
    lv_obj_add_event_cb(closeBtn, closeBountyModal, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cl = lv_label_create(closeBtn);
    lv_label_set_text(cl, "CLOSE");
    lv_obj_set_style_text_color(cl, C_TXT, 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_14, 0);
    lv_obj_center(cl);

    // Target block
    lv_obj_t *nm = lv_label_create(bountyModal);
    lv_label_set_text(nm, t->target_name);
    lv_obj_set_style_text_color(nm, C_WHITE, 0);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_36, 0);
    lv_obj_set_pos(nm, 0, 36);

    lv_obj_t *ds = lv_label_create(bountyModal);
    lv_label_set_text(ds, t->description);
    lv_obj_set_style_text_color(ds, C_TXT, 0);
    lv_obj_set_style_text_font(ds, &lv_font_montserrat_16, 0);
    lv_obj_set_width(ds, 600);
    lv_label_set_long_mode(ds, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(ds, 0, 80);

    char rb[32]; snprintf(rb, sizeof(rb), "REWARD: %d", t->reward);
    lv_obj_t *rw = lv_label_create(bountyModal);
    lv_label_set_text(rw, rb);
    lv_obj_set_style_text_color(rw, C_AMB_BRT, 0);
    lv_obj_set_style_text_font(rw, &lv_font_montserrat_24, 0);
    lv_obj_align(rw, LV_ALIGN_TOP_RIGHT, -100, 36);

    // POST button
    modalPostBtn = lv_btn_create(bountyModal);
    lv_obj_set_size(modalPostBtn, 160, 50);
    lv_obj_set_style_bg_color(modalPostBtn, C_RED, 0);
    lv_obj_set_style_bg_color(modalPostBtn, lv_color_hex(0xFF6050), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(modalPostBtn, 0, 0);
    lv_obj_set_pos(modalPostBtn, 0, 130);
    lv_obj_add_event_cb(modalPostBtn, ev_post_bounty, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *pl = lv_label_create(modalPostBtn);
    lv_label_set_text(pl, "POST BOUNTY");
    lv_obj_set_style_text_color(pl, C_WHITE, 0);
    lv_obj_set_style_text_font(pl, &lv_font_montserrat_18, 0);
    lv_obj_center(pl);

    // CANCEL button
    lv_obj_t *cancelBtn = lv_btn_create(bountyModal);
    lv_obj_set_size(cancelBtn, 160, 50);
    lv_obj_set_style_bg_color(cancelBtn, C_PNL2, 0);
    lv_obj_set_style_border_color(cancelBtn, C_DIM, 0);
    lv_obj_set_style_border_width(cancelBtn, 1, 0);
    lv_obj_set_pos(cancelBtn, 180, 130);
    lv_obj_add_event_cb(cancelBtn, ev_cancel_bounty, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *xl = lv_label_create(cancelBtn);
    lv_label_set_text(xl, "CLOSE BOUNTY");
    lv_obj_set_style_text_color(xl, C_DIM, 0);
    lv_obj_set_style_text_font(xl, &lv_font_montserrat_14, 0);
    lv_obj_center(xl);

    // Clues section
    lv_obj_t *clueH = lv_label_create(bountyModal);
    lv_label_set_text(clueH, "CLUES");
    lv_obj_set_style_text_color(clueH, C_AMB_BRT, 0);
    lv_obj_set_style_text_font(clueH, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(clueH, 0, 196);

    modalClueList = lv_obj_create(bountyModal);
    lv_obj_set_size(modalClueList, 600, 90);
    lv_obj_set_pos(modalClueList, 0, 222);
    lv_obj_set_style_bg_opa(modalClueList, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(modalClueList, 0, 0);
    lv_obj_set_style_pad_all(modalClueList, 0, 0);
    lv_obj_set_flex_flow(modalClueList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(modalClueList, 6, 0);

    // Players (award section)
    lv_obj_t *plH = lv_label_create(bountyModal);
    lv_label_set_text(plH, "AWARD TO PLAYER");
    lv_obj_set_style_text_color(plH, C_GRN, 0);
    lv_obj_set_style_text_font(plH, &lv_font_montserrat_18, 0);
    lv_obj_set_pos(plH, 0, 332);

    modalPlayerList = lv_obj_create(bountyModal);
    lv_obj_set_size(modalPlayerList, lv_pct(100), 80);
    lv_obj_set_pos(modalPlayerList, 0, 358);
    lv_obj_set_style_bg_opa(modalPlayerList, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(modalPlayerList, 0, 0);
    lv_obj_set_style_pad_all(modalPlayerList, 0, 0);
    lv_obj_set_flex_flow(modalPlayerList, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(modalPlayerList, 8, 0);

    refreshModalStatus();
    refreshModalClues();
    refreshModalPlayers();
}

void buildEventsTab(lv_obj_t *tab) {
    lv_obj_set_style_bg_color(tab, C_BG, 0);
    lv_obj_set_style_pad_all(tab, 16, 0);

    lv_obj_t *title = lv_label_create(tab);
    lv_label_set_text(title, "EVENT TRIGGERS");
    lv_obj_set_style_text_color(title, C_AMB_BRT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    lv_obj_t *cont = lv_obj_create(tab);
    lv_obj_set_size(cont, lv_pct(100), 320);
    lv_obj_set_pos(cont, 0, 44);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_gap(cont, 12, 0);

    for (int i = 0; i < eventCount; i++) {
        EventTemplate *t = &eventTemplates[i];
        lv_obj_t *btn = lv_btn_create(cont);
        lv_obj_set_size(btn, 240, 100);
        lv_obj_set_style_bg_color(btn, C_PNL, 0);
        lv_obj_set_style_bg_color(btn, C_PNL2, LV_STATE_PRESSED);
        lv_color_t accent = (t->severity >= 2) ? C_RED : (t->severity == 1) ? C_AMB : C_CYN;
        lv_obj_set_style_border_color(btn, accent, 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_add_event_cb(btn, ev_trigger_event, LV_EVENT_CLICKED, t);

        lv_obj_t *l1 = lv_label_create(btn);
        lv_label_set_text(l1, t->name);
        lv_obj_set_style_text_color(l1, C_WHITE, 0);
        lv_obj_set_style_text_font(l1, &lv_font_montserrat_18, 0);
        lv_obj_set_pos(l1, 10, 6);

        lv_obj_t *l2 = lv_label_create(btn);
        lv_label_set_text(l2, t->description);
        lv_obj_set_style_text_color(l2, C_DIM, 0);
        lv_obj_set_style_text_font(l2, &lv_font_montserrat_12, 0);
        lv_obj_set_width(l2, 220);
        lv_obj_set_pos(l2, 10, 32);

        const char *sl = (t->severity >= 2) ? "CRITICAL" : (t->severity == 1) ? "WARNING" : "INFO";
        lv_obj_t *sev = lv_label_create(btn);
        lv_label_set_text(sev, sl);
        lv_obj_set_style_text_color(sev, accent, 0);
        lv_obj_set_style_text_font(sev, &lv_font_montserrat_12, 0);
        lv_obj_set_pos(sev, 10, 78);
    }
}

// ═══════════════════════════════════════════════
//  COMMS COMPOSE MODAL — custom message + popup keyboard
// ═══════════════════════════════════════════════
static lv_obj_t *composeModal = nullptr;
static lv_obj_t *composeKeyboard = nullptr;
static lv_obj_t *composeFromTA = nullptr;
static lv_obj_t *composeSubjTA = nullptr;
static lv_obj_t *composeBodyTA = nullptr;
static lv_obj_t *composeTargetLbl = nullptr;
static char composeTargetId[16] = "";   // empty = broadcast

static void closeComposeModal(lv_event_t *e) {
    if (composeModal) {
        lv_obj_del(composeModal);
        composeModal = nullptr;
        composeKeyboard = nullptr;
        composeFromTA = nullptr;
        composeSubjTA = nullptr;
        composeBodyTA = nullptr;
        composeTargetLbl = nullptr;
    }
}

static void ev_ta_focus(lv_event_t *e) {
    lv_obj_t *ta = lv_event_get_target(e);
    if (composeKeyboard) {
        lv_keyboard_set_textarea(composeKeyboard, ta);
        lv_obj_clear_flag(composeKeyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ev_kb_done(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        if (composeKeyboard) {
            lv_keyboard_set_textarea(composeKeyboard, nullptr);
            lv_obj_add_flag(composeKeyboard, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void refreshComposeTarget() {
    if (!composeTargetLbl) return;
    if (composeTargetId[0] == '\0') {
        lv_label_set_text(composeTargetLbl, "TO: ALL OPERATIVES");
        lv_obj_set_style_text_color(composeTargetLbl, C_CYN_BRT, 0);
    } else {
        char b[40]; snprintf(b, sizeof(b), "TO: %s", composeTargetId);
        lv_label_set_text(composeTargetLbl, b);
        lv_obj_set_style_text_color(composeTargetLbl, C_AMB_BRT, 0);
    }
}

static void ev_target_cycle(lv_event_t *e) {
    // Cycle through: broadcast → each online datapad → back to broadcast
    int cur = -1;   // -1 = broadcast
    if (composeTargetId[0] != '\0') {
        for (int i = 0; i < deviceCount; i++) {
            if (trackedDevs[i].role == swts::ROLE_DATAPAD &&
                strcmp(trackedDevs[i].id, composeTargetId) == 0) { cur = i; break; }
        }
    }
    // Find next datapad after cur
    int next = -1;
    for (int i = cur + 1; i < deviceCount; i++) {
        if (trackedDevs[i].role == swts::ROLE_DATAPAD) { next = i; break; }
    }
    if (next < 0) composeTargetId[0] = '\0';
    else strlcpy(composeTargetId, trackedDevs[next].id, sizeof(composeTargetId));
    refreshComposeTarget();
}

static void ev_send_custom_comm(lv_event_t *e) {
    if (!composeFromTA || !composeSubjTA || !composeBodyTA) return;
    const char *fromTxt = lv_textarea_get_text(composeFromTA);
    const char *subjTxt = lv_textarea_get_text(composeSubjTA);
    const char *bodyTxt = lv_textarea_get_text(composeBodyTA);
    if (!fromTxt[0] || !subjTxt[0]) {
        S.println("[GM] Custom comm rejected: empty FROM or SUBJECT");
        return;
    }
    char commId[24];
    snprintf(commId, sizeof(commId), "gm_custom_%lu", millis());
    swts::gmPushComm(commId, composeTargetId, fromTxt, subjTxt, bodyTxt);
    char log[80];
    snprintf(log, sizeof(log), "CUSTOM COMM: %s%s%s", subjTxt,
             composeTargetId[0] ? " -> " : "", composeTargetId);
    logActivity(log, 4);
    S.printf("[GM] Custom comm sent: %s -> %s\n", subjTxt,
             composeTargetId[0] ? composeTargetId : "ALL");
    closeComposeModal(nullptr);
}

static void openComposeModal(const CommTemplate *prefill) {
    if (composeModal) return;
    composeTargetId[0] = '\0';

    composeModal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(composeModal, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(composeModal, 0, 0);
    lv_obj_set_style_bg_color(composeModal, C_BG, 0);
    lv_obj_set_style_bg_opa(composeModal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(composeModal, 0, 0);
    lv_obj_set_style_pad_all(composeModal, 16, 0);
    lv_obj_clear_flag(composeModal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(composeModal);

    // Title
    lv_obj_t *title = lv_label_create(composeModal);
    lv_label_set_text(title, "COMPOSE TRANSMISSION");
    lv_obj_set_style_text_color(title, C_CYN_BRT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(title, 0, 0);

    // Close button
    lv_obj_t *closeBtn = lv_btn_create(composeModal);
    lv_obj_set_size(closeBtn, 80, 40);
    lv_obj_align(closeBtn, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(closeBtn, C_PNL2, 0);
    lv_obj_set_style_border_color(closeBtn, C_DIM, 0);
    lv_obj_set_style_border_width(closeBtn, 1, 0);
    lv_obj_add_event_cb(closeBtn, closeComposeModal, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cl = lv_label_create(closeBtn);
    lv_label_set_text(cl, "CLOSE");
    lv_obj_set_style_text_color(cl, C_TXT, 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_14, 0);
    lv_obj_center(cl);

    // Target row
    lv_obj_t *targBtn = lv_btn_create(composeModal);
    lv_obj_set_size(targBtn, 320, 38);
    lv_obj_set_pos(targBtn, 0, 36);
    lv_obj_set_style_bg_color(targBtn, C_PNL, 0);
    lv_obj_set_style_bg_color(targBtn, C_PNL2, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(targBtn, C_CYN, 0);
    lv_obj_set_style_border_side(targBtn, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(targBtn, 4, 0);
    lv_obj_add_event_cb(targBtn, ev_target_cycle, LV_EVENT_CLICKED, nullptr);
    composeTargetLbl = lv_label_create(targBtn);
    lv_obj_set_style_text_font(composeTargetLbl, &lv_font_montserrat_16, 0);
    lv_obj_center(composeTargetLbl);
    refreshComposeTarget();

    lv_obj_t *hint = lv_label_create(composeModal);
    lv_label_set_text(hint, "tap to cycle target");
    lv_obj_set_style_text_color(hint, C_DIM, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(hint, 330, 44);

    // FROM
    lv_obj_t *fl = lv_label_create(composeModal);
    lv_label_set_text(fl, "FROM");
    lv_obj_set_style_text_color(fl, C_CYN, 0);
    lv_obj_set_style_text_font(fl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(fl, 0, 84);

    composeFromTA = lv_textarea_create(composeModal);
    lv_obj_set_size(composeFromTA, 760, 40);
    lv_obj_set_pos(composeFromTA, 0, 102);
    lv_textarea_set_one_line(composeFromTA, true);
    lv_textarea_set_max_length(composeFromTA, 19);
    lv_textarea_set_placeholder_text(composeFromTA, "ALLIANCE COMMAND");
    lv_textarea_set_text(composeFromTA, prefill ? prefill->from : "ALLIANCE COMMAND");
    lv_obj_set_style_bg_color(composeFromTA, C_PNL2, 0);
    lv_obj_set_style_border_color(composeFromTA, C_CYN, 0);
    lv_obj_set_style_text_color(composeFromTA, C_WHITE, 0);
    lv_obj_set_style_text_font(composeFromTA, &lv_font_montserrat_16, 0);
    lv_obj_add_event_cb(composeFromTA, ev_ta_focus, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(composeFromTA, ev_ta_focus, LV_EVENT_CLICKED, nullptr);

    // SUBJECT
    lv_obj_t *sl = lv_label_create(composeModal);
    lv_label_set_text(sl, "SUBJECT");
    lv_obj_set_style_text_color(sl, C_CYN, 0);
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(sl, 0, 148);

    composeSubjTA = lv_textarea_create(composeModal);
    lv_obj_set_size(composeSubjTA, 760, 40);
    lv_obj_set_pos(composeSubjTA, 0, 166);
    lv_textarea_set_one_line(composeSubjTA, true);
    lv_textarea_set_max_length(composeSubjTA, 35);
    lv_textarea_set_placeholder_text(composeSubjTA, "OPS UPDATE");
    if (prefill) lv_textarea_set_text(composeSubjTA, prefill->subject);
    lv_obj_set_style_bg_color(composeSubjTA, C_PNL2, 0);
    lv_obj_set_style_border_color(composeSubjTA, C_CYN, 0);
    lv_obj_set_style_text_color(composeSubjTA, C_WHITE, 0);
    lv_obj_set_style_text_font(composeSubjTA, &lv_font_montserrat_16, 0);
    lv_obj_add_event_cb(composeSubjTA, ev_ta_focus, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(composeSubjTA, ev_ta_focus, LV_EVENT_CLICKED, nullptr);

    // BODY
    lv_obj_t *bl = lv_label_create(composeModal);
    lv_label_set_text(bl, "MESSAGE");
    lv_obj_set_style_text_color(bl, C_CYN, 0);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(bl, 0, 212);

    composeBodyTA = lv_textarea_create(composeModal);
    lv_obj_set_size(composeBodyTA, 660, 90);
    lv_obj_set_pos(composeBodyTA, 0, 230);
    lv_textarea_set_max_length(composeBodyTA, 119);
    lv_textarea_set_placeholder_text(composeBodyTA, "Operatives, ...");
    if (prefill) lv_textarea_set_text(composeBodyTA, prefill->body);
    lv_obj_set_style_bg_color(composeBodyTA, C_PNL2, 0);
    lv_obj_set_style_border_color(composeBodyTA, C_CYN, 0);
    lv_obj_set_style_text_color(composeBodyTA, C_WHITE, 0);
    lv_obj_set_style_text_font(composeBodyTA, &lv_font_montserrat_16, 0);
    lv_obj_add_event_cb(composeBodyTA, ev_ta_focus, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(composeBodyTA, ev_ta_focus, LV_EVENT_CLICKED, nullptr);

    // SEND button
    lv_obj_t *sendBtn = lv_btn_create(composeModal);
    lv_obj_set_size(sendBtn, 100, 90);
    lv_obj_set_pos(sendBtn, 670, 230);
    lv_obj_set_style_bg_color(sendBtn, C_CYN, 0);
    lv_obj_set_style_bg_color(sendBtn, C_CYN_BRT, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(sendBtn, 0, 0);
    lv_obj_add_event_cb(sendBtn, ev_send_custom_comm, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *sl2 = lv_label_create(sendBtn);
    lv_label_set_text(sl2, "SEND");
    lv_obj_set_style_text_color(sl2, C_BG, 0);
    lv_obj_set_style_text_font(sl2, &lv_font_montserrat_24, 0);
    lv_obj_center(sl2);

    // KEYBOARD (hidden until focus)
    composeKeyboard = lv_keyboard_create(composeModal);
    lv_obj_set_size(composeKeyboard, lv_pct(100), 220);
    lv_obj_align(composeKeyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(composeKeyboard, C_PNL, 0);
    lv_obj_set_style_text_color(composeKeyboard, C_WHITE, 0);
    lv_obj_set_style_text_color(composeKeyboard, C_AMB_BRT, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(composeKeyboard, C_PNL2, LV_PART_ITEMS);
    lv_obj_add_flag(composeKeyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(composeKeyboard, ev_kb_done, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(composeKeyboard, ev_kb_done, LV_EVENT_CANCEL, nullptr);
}

static void ev_open_compose(lv_event_t *e) {
    openComposeModal(nullptr);
}

// Long-press a template card → open compose pre-filled
static void ev_template_long_press(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_LONG_PRESSED) return;
    CommTemplate *t = (CommTemplate *)lv_event_get_user_data(e);
    openComposeModal(t);
}

// ═══════════════════════════════════════════════
//  ASSIGN CALLSIGN MODAL — name an unassigned datapad
// ═══════════════════════════════════════════════
static lv_obj_t *assignModal = nullptr;
static lv_obj_t *assignKeyboard = nullptr;
static lv_obj_t *assignNameTA = nullptr;
static char assignTargetId[16] = "";   // current callsign of the device we're naming

static void closeAssignModal(lv_event_t *e) {
    if (assignModal) {
        lv_obj_del(assignModal);
        assignModal = nullptr;
        assignKeyboard = nullptr;
        assignNameTA = nullptr;
        assignTargetId[0] = 0;
    }
}

static void ev_assign_focus(lv_event_t *e) {
    if (assignKeyboard) {
        lv_keyboard_set_textarea(assignKeyboard, assignNameTA);
        lv_obj_clear_flag(assignKeyboard, LV_OBJ_FLAG_HIDDEN);
    }
}
static void ev_assign_kb_done(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        if (assignKeyboard) {
            lv_keyboard_set_textarea(assignKeyboard, nullptr);
            lv_obj_add_flag(assignKeyboard, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void ev_assign_send(lv_event_t *e) {
    if (!assignNameTA || !assignTargetId[0]) return;
    const char *name = lv_textarea_get_text(assignNameTA);
    if (!name || !name[0]) { S.println("[GM] assign: empty callsign rejected"); return; }
    if (gmIsUnassignedCallsign(name)) {
        S.printf("[GM] assign: %s is a reserved placeholder, rejected\n", name);
        return;
    }
    S.printf("[GM] Assigning callsign: %s -> %s\n", assignTargetId, name);
    swts::gmAssignCallsign(assignTargetId, name);
    // Also locally rename the tracked device so the UI reflects immediately
    for (int i = 0; i < deviceCount; i++) {
        if (strcmp(trackedDevs[i].id, assignTargetId) == 0) {
            strlcpy(trackedDevs[i].id, name, sizeof(trackedDevs[i].id));
            break;
        }
    }
    char log[80]; snprintf(log, sizeof(log), "ASSIGNED: %s -> %s", assignTargetId, name);
    logActivity(log, 5);
    gmDirty = true;
    closeAssignModal(nullptr);
}

void openAssignModal(TrackedDevice *target) {
    if (assignModal || !target) return;
    strlcpy(assignTargetId, target->id, sizeof(assignTargetId));

    assignModal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(assignModal, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(assignModal, 0, 0);
    lv_obj_set_style_bg_color(assignModal, C_BG, 0);
    lv_obj_set_style_bg_opa(assignModal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(assignModal, 0, 0);
    lv_obj_set_style_pad_all(assignModal, 18, 0);
    lv_obj_clear_flag(assignModal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(assignModal);

    lv_obj_t *title = lv_label_create(assignModal);
    lv_label_set_text(title, "ASSIGN CALLSIGN");
    lv_obj_set_style_text_color(title, C_AMB_BRT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(title, 0, 0);

    lv_obj_t *tgt = lv_label_create(assignModal);
    char tbuf[40]; snprintf(tbuf, sizeof(tbuf), "Device: %s", assignTargetId);
    lv_label_set_text(tgt, tbuf);
    lv_obj_set_style_text_color(tgt, C_DIM, 0);
    lv_obj_set_style_text_font(tgt, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(tgt, 0, 36);

    lv_obj_t *closeBtn = lv_btn_create(assignModal);
    lv_obj_set_size(closeBtn, 80, 40);
    lv_obj_align(closeBtn, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(closeBtn, C_PNL2, 0);
    lv_obj_set_style_border_color(closeBtn, C_DIM, 0);
    lv_obj_set_style_border_width(closeBtn, 1, 0);
    lv_obj_add_event_cb(closeBtn, closeAssignModal, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cl = lv_label_create(closeBtn);
    lv_label_set_text(cl, "CLOSE");
    lv_obj_set_style_text_color(cl, C_TXT, 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_14, 0);
    lv_obj_center(cl);

    lv_obj_t *lbl = lv_label_create(assignModal);
    lv_label_set_text(lbl, "Operative name (player's chosen callsign):");
    lv_obj_set_style_text_color(lbl, C_AMB, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(lbl, 0, 90);

    assignNameTA = lv_textarea_create(assignModal);
    lv_obj_set_size(assignNameTA, 560, 50);
    lv_obj_set_pos(assignNameTA, 0, 116);
    lv_textarea_set_one_line(assignNameTA, true);
    lv_textarea_set_max_length(assignNameTA, 15);
    lv_textarea_set_placeholder_text(assignNameTA, "e.g. KESTIS-7");
    lv_obj_set_style_bg_color(assignNameTA, C_PNL2, 0);
    lv_obj_set_style_border_color(assignNameTA, C_AMB, 0);
    lv_obj_set_style_text_color(assignNameTA, C_WHITE, 0);
    lv_obj_set_style_text_font(assignNameTA, &lv_font_montserrat_20, 0);
    lv_obj_add_event_cb(assignNameTA, ev_assign_focus, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(assignNameTA, ev_assign_focus, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *sendBtn = lv_btn_create(assignModal);
    lv_obj_set_size(sendBtn, 200, 50);
    lv_obj_set_pos(sendBtn, 570, 116);
    lv_obj_set_style_bg_color(sendBtn, C_AMB, 0);
    lv_obj_set_style_bg_color(sendBtn, C_AMB_BRT, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(sendBtn, 0, 0);
    lv_obj_add_event_cb(sendBtn, ev_assign_send, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *sl = lv_label_create(sendBtn);
    lv_label_set_text(sl, "DEPLOY");
    lv_obj_set_style_text_color(sl, C_BG, 0);
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_20, 0);
    lv_obj_center(sl);

    lv_obj_t *hint = lv_label_create(assignModal);
    lv_label_set_text(hint, "Tap the field to type. Press Enter or close to dismiss the keyboard.");
    lv_obj_set_style_text_color(hint, C_DIM, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(hint, 0, 176);

    assignKeyboard = lv_keyboard_create(assignModal);
    lv_obj_set_size(assignKeyboard, lv_pct(100), 220);
    lv_obj_align(assignKeyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(assignKeyboard, C_PNL, 0);
    lv_obj_set_style_text_color(assignKeyboard, C_WHITE, 0);
    lv_obj_set_style_text_color(assignKeyboard, C_AMB_BRT, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(assignKeyboard, C_PNL2, LV_PART_ITEMS);
    lv_obj_add_flag(assignKeyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(assignKeyboard, ev_assign_kb_done, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(assignKeyboard, ev_assign_kb_done, LV_EVENT_CANCEL, nullptr);
}

static void ev_open_assign(lv_event_t *e) {
    TrackedDevice *d = (TrackedDevice *)lv_event_get_user_data(e);
    if (d) openAssignModal(d);
}

void buildCommsTab(lv_obj_t *tab) {
    lv_obj_set_style_bg_color(tab, C_BG, 0);
    lv_obj_set_style_pad_all(tab, 16, 0);

    lv_obj_t *title = lv_label_create(tab);
    lv_label_set_text(title, "BROADCAST COMMS");
    lv_obj_set_style_text_color(title, C_CYN_BRT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    // COMPOSE NEW button (top-right)
    lv_obj_t *cmpBtn = lv_btn_create(tab);
    lv_obj_set_size(cmpBtn, 200, 44);
    lv_obj_align(cmpBtn, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(cmpBtn, C_CYN, 0);
    lv_obj_set_style_bg_color(cmpBtn, C_CYN_BRT, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(cmpBtn, 0, 0);
    lv_obj_add_event_cb(cmpBtn, ev_open_compose, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cmpL = lv_label_create(cmpBtn);
    lv_label_set_text(cmpL, "+ COMPOSE NEW");
    lv_obj_set_style_text_color(cmpL, C_BG, 0);
    lv_obj_set_style_text_font(cmpL, &lv_font_montserrat_18, 0);
    lv_obj_center(cmpL);

    lv_obj_t *hint = lv_label_create(tab);
    lv_label_set_text(hint, "Tap to send template, long-press to edit before sending.");
    lv_obj_set_style_text_color(hint, C_DIM, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(hint, 0, 50);

    lv_obj_t *cont = lv_obj_create(tab);
    lv_obj_set_size(cont, lv_pct(100), 300);
    lv_obj_set_pos(cont, 0, 72);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 0, 0);
    lv_obj_set_style_pad_row(cont, 8, 0);

    for (int i = 0; i < commCount; i++) {
        CommTemplate *t = &commTemplates[i];
        lv_obj_t *btn = lv_btn_create(cont);
        lv_obj_set_size(btn, lv_pct(100), 70);
        lv_obj_set_style_bg_color(btn, C_PNL, 0);
        lv_obj_set_style_bg_color(btn, C_PNL2, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, C_CYN, 0);
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(btn, 4, 0);
        lv_obj_set_style_radius(btn, 2, 0);
        // SHORT_CLICKED (not CLICKED) so a long-press for "edit before sending"
        // doesn't also fire the send on release.
        lv_obj_add_event_cb(btn, ev_push_comm, LV_EVENT_SHORT_CLICKED, t);
        lv_obj_add_event_cb(btn, ev_template_long_press, LV_EVENT_LONG_PRESSED, t);

        lv_obj_t *from = lv_label_create(btn);
        lv_label_set_text(from, t->from);
        lv_obj_set_style_text_color(from, C_CYN_BRT, 0);
        lv_obj_set_style_text_font(from, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(from, 10, 4);

        lv_obj_t *subj = lv_label_create(btn);
        lv_label_set_text(subj, t->subject);
        lv_obj_set_style_text_color(subj, C_WHITE, 0);
        lv_obj_set_style_text_font(subj, &lv_font_montserrat_18, 0);
        lv_obj_set_pos(subj, 10, 22);

        lv_obj_t *body = lv_label_create(btn);
        lv_label_set_text(body, t->body);
        lv_obj_set_style_text_color(body, C_DIM, 0);
        lv_obj_set_style_text_font(body, &lv_font_montserrat_12, 0);
        lv_obj_set_width(body, lv_pct(90));
        lv_obj_set_pos(body, 10, 46);
    }
}

lv_obj_t *bountyGrid = nullptr;

static void refreshBountyCards() {
    if (!bountyGrid) return;
    lv_obj_clean(bountyGrid);
    for (int i = 0; i < bountyCount; i++) {
        BountyTemplate *t = &bountyTemplates[i];
        lv_obj_t *btn = lv_btn_create(bountyGrid);
        lv_obj_set_size(btn, 240, 150);
        lv_obj_set_style_bg_color(btn, C_PNL, 0);
        lv_obj_set_style_bg_color(btn, C_PNL2, LV_STATE_PRESSED);
        lv_color_t border = t->closed ? C_DIM : (t->posted ? C_AMB : C_RED);
        lv_obj_set_style_border_color(btn, border, 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_radius(btn, 4, 0);
        lv_obj_add_event_cb(btn, ev_open_bounty_detail, LV_EVENT_CLICKED, t);

        lv_obj_t *w1 = lv_label_create(btn);
        lv_label_set_text(w1, t->closed ? "CLOSED" : (t->posted ? "POSTED" : "WANTED"));
        lv_obj_set_style_text_color(w1, border, 0);
        lv_obj_set_style_text_font(w1, &lv_font_montserrat_12, 0);
        lv_obj_set_pos(w1, 10, 6);

        lv_obj_t *nm = lv_label_create(btn);
        lv_label_set_text(nm, t->target_name);
        lv_obj_set_style_text_color(nm, C_WHITE, 0);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_20, 0);
        lv_obj_set_pos(nm, 10, 24);

        lv_obj_t *ds = lv_label_create(btn);
        lv_label_set_text(ds, t->description);
        lv_obj_set_style_text_color(ds, C_DIM, 0);
        lv_obj_set_style_text_font(ds, &lv_font_montserrat_12, 0);
        lv_obj_set_width(ds, 220);
        lv_obj_set_pos(ds, 10, 54);

        char rb[40];
        if (t->posted && !t->closed)
            snprintf(rb, sizeof(rb), "REWARD %d   //   %d/%d CLUES", t->reward, t->cluesSent, t->clueCount);
        else
            snprintf(rb, sizeof(rb), "REWARD %d", t->reward);
        lv_obj_t *rw = lv_label_create(btn);
        lv_label_set_text(rw, rb);
        lv_obj_set_style_text_color(rw, C_AMB_BRT, 0);
        lv_obj_set_style_text_font(rw, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(rw, 10, 122);
    }
}

void buildBountiesTab(lv_obj_t *tab) {
    lv_obj_set_style_bg_color(tab, C_BG, 0);
    lv_obj_set_style_pad_all(tab, 16, 0);

    lv_obj_t *title = lv_label_create(tab);
    lv_label_set_text(title, "WANTED BOARD");
    lv_obj_set_style_text_color(title, C_RED, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    lv_obj_t *hint = lv_label_create(tab);
    lv_label_set_text(hint, "Tap a target to post, push clues, or award the bounty.");
    lv_obj_set_style_text_color(hint, C_DIM, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(hint, 0, 36);

    bountyGrid = lv_obj_create(tab);
    lv_obj_set_size(bountyGrid, lv_pct(100), 320);
    lv_obj_set_pos(bountyGrid, 0, 58);
    lv_obj_set_flex_flow(bountyGrid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_bg_opa(bountyGrid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bountyGrid, 0, 0);
    lv_obj_set_style_pad_all(bountyGrid, 0, 0);
    lv_obj_set_style_pad_gap(bountyGrid, 12, 0);

    refreshBountyCards();
}

// Forward decl for the callsign modal (defined below)
void openAssignModal(TrackedDevice *target);
static void ev_open_assign(lv_event_t *e);

static void ev_show_leaderboard(lv_event_t *e) {
    if (playerList)     lv_obj_clear_flag(playerList, LV_OBJ_FLAG_HIDDEN);
    if (unassignedList) lv_obj_add_flag(unassignedList, LV_OBJ_FLAG_HIDDEN);
}
static void ev_show_unassigned(lv_event_t *e) {
    if (playerList)     lv_obj_add_flag(playerList, LV_OBJ_FLAG_HIDDEN);
    if (unassignedList) lv_obj_clear_flag(unassignedList, LV_OBJ_FLAG_HIDDEN);
}

void buildPlayersTab(lv_obj_t *tab) {
    lv_obj_set_style_bg_color(tab, C_BG, 0);
    lv_obj_set_style_pad_all(tab, 16, 0);

    // ── Toggle row ──
    lv_obj_t *lbBtn = lv_btn_create(tab);
    lv_obj_set_size(lbBtn, 220, 44);
    lv_obj_set_pos(lbBtn, 0, 0);
    lv_obj_set_style_bg_color(lbBtn, C_AMB_DIM, 0);
    lv_obj_set_style_bg_color(lbBtn, C_AMB, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(lbBtn, 0, 0);
    lv_obj_add_event_cb(lbBtn, ev_show_leaderboard, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbL = lv_label_create(lbBtn);
    lv_label_set_text(lbL, "LEADERBOARD");
    lv_obj_set_style_text_color(lbL, C_BG, 0);
    lv_obj_set_style_text_font(lbL, &lv_font_montserrat_18, 0);
    lv_obj_center(lbL);

    lv_obj_t *unBtn = lv_btn_create(tab);
    lv_obj_set_size(unBtn, 240, 44);
    lv_obj_set_pos(unBtn, 232, 0);
    lv_obj_set_style_bg_color(unBtn, C_PNL2, 0);
    lv_obj_set_style_bg_color(unBtn, C_PNL, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(unBtn, C_RED, 0);
    lv_obj_set_style_border_width(unBtn, 1, 0);
    lv_obj_add_event_cb(unBtn, ev_show_unassigned, LV_EVENT_CLICKED, nullptr);
    unassignedBtnLbl = lv_label_create(unBtn);
    lv_label_set_text(unassignedBtnLbl, "UNASSIGNED");
    lv_obj_set_style_text_color(unassignedBtnLbl, C_RED, 0);
    lv_obj_set_style_text_font(unassignedBtnLbl, &lv_font_montserrat_18, 0);
    lv_obj_center(unassignedBtnLbl);

    // ── Leaderboard list (visible by default) ──
    playerList = lv_obj_create(tab);
    lv_obj_set_size(playerList, lv_pct(100), 320);
    lv_obj_set_pos(playerList, 0, 54);
    lv_obj_set_flex_flow(playerList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(playerList, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(playerList, 0, 0);
    lv_obj_set_style_pad_row(playerList, 6, 0);

    // ── Unassigned list (hidden by default) ──
    unassignedList = lv_obj_create(tab);
    lv_obj_set_size(unassignedList, lv_pct(100), 320);
    lv_obj_set_pos(unassignedList, 0, 54);
    lv_obj_set_flex_flow(unassignedList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_opa(unassignedList, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(unassignedList, 0, 0);
    lv_obj_set_style_pad_row(unassignedList, 6, 0);
    lv_obj_add_flag(unassignedList, LV_OBJ_FLAG_HIDDEN);
}

void buildPanelsTab(lv_obj_t *tab) {
    lv_obj_set_style_bg_color(tab, C_BG, 0);
    lv_obj_set_style_pad_all(tab, 16, 0);

    lv_obj_t *title = lv_label_create(tab);
    lv_label_set_text(title, "PROP NETWORK");
    lv_obj_set_style_text_color(title, C_CYN_BRT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);

    panelList = lv_obj_create(tab);
    lv_obj_set_size(panelList, lv_pct(100), 320);
    lv_obj_set_pos(panelList, 0, 44);
    lv_obj_set_flex_flow(panelList, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_bg_opa(panelList, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(panelList, 0, 0);
    lv_obj_set_style_pad_gap(panelList, 12, 0);
}

void refreshLiveLists() {
    if (!playerList || !panelList || !unassignedList) return;
    lv_obj_clean(playerList);
    lv_obj_clean(panelList);
    lv_obj_clean(unassignedList);

    int idx[MAX_DEVICES], uidx[MAX_DEVICES], pidx[MAX_DEVICES];
    int playerN = 0, unN = 0, panelN = 0;
    for (int i = 0; i < deviceCount; i++) {
        if (trackedDevs[i].role == swts::ROLE_DATAPAD) {
            if (gmIsUnassignedCallsign(trackedDevs[i].id)) uidx[unN++] = i;
            else                                            idx[playerN++] = i;
        } else if (trackedDevs[i].role == swts::ROLE_PANEL) {
            pidx[panelN++] = i;
        }
    }

    if (unassignedBtnLbl) {
        char b[24];
        if (unN > 0) snprintf(b, sizeof(b), "UNASSIGNED (%d)", unN);
        else         strlcpy(b, "UNASSIGNED", sizeof(b));
        lv_label_set_text(unassignedBtnLbl, b);
        lv_obj_set_style_text_color(unassignedBtnLbl, unN > 0 ? C_RED : C_DIM, 0);
    }

    // Leaderboard sort
    for (int i = 0; i < playerN - 1; i++)
        for (int j = i + 1; j < playerN; j++)
            if (trackedDevs[idx[i]].score < trackedDevs[idx[j]].score) {
                int tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp;
            }

    for (int rank = 0; rank < playerN; rank++) {
        TrackedDevice &d = trackedDevs[idx[rank]];
        lv_obj_t *card = lv_obj_create(playerList);
        lv_obj_set_size(card, lv_pct(100), 70);
        lv_obj_set_style_bg_color(card, C_PNL, 0);
        bool stale = (millis() - d.lastHeard > 30000);
        lv_obj_set_style_border_color(card, stale ? C_MUT : C_AMB, 0);
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_radius(card, 2, 0);

        char rb[8]; snprintf(rb, sizeof(rb), "#%d", rank + 1);
        lv_obj_t *r = lv_label_create(card);
        lv_label_set_text(r, rb);
        lv_obj_set_style_text_color(r, C_AMB_BRT, 0);
        lv_obj_set_style_text_font(r, &lv_font_montserrat_24, 0);
        lv_obj_set_pos(r, 10, 10);

        lv_obj_t *n = lv_label_create(card);
        lv_label_set_text(n, d.id);
        lv_obj_set_style_text_color(n, stale ? C_DIM : C_WHITE, 0);
        lv_obj_set_style_text_font(n, &lv_font_montserrat_20, 0);
        lv_obj_set_pos(n, 70, 4);

        char info[64];
        snprintf(info, sizeof(info), "Missions: %d  /  Scans: %d  /  Slices: %d", d.activeMsn, d.scans, d.slicesWon);
        lv_obj_t *st = lv_label_create(card);
        lv_label_set_text(st, info);
        lv_obj_set_style_text_color(st, C_DIM, 0);
        lv_obj_set_style_text_font(st, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(st, 70, 36);

        char sb[16]; snprintf(sb, sizeof(sb), "%d", d.score);
        lv_obj_t *sc = lv_label_create(card);
        lv_label_set_text(sc, sb);
        lv_obj_set_style_text_color(sc, C_AMB_BRT, 0);
        lv_obj_set_style_text_font(sc, &lv_font_montserrat_28, 0);
        lv_obj_align(sc, LV_ALIGN_RIGHT_MID, -20, 0);
    }

    // Unassigned cards — clickable to open the assign-callsign modal
    for (int i = 0; i < unN; i++) {
        TrackedDevice &d = trackedDevs[uidx[i]];
        lv_obj_t *card = lv_btn_create(unassignedList);
        lv_obj_set_size(card, lv_pct(100), 70);
        lv_obj_set_style_bg_color(card, C_PNL, 0);
        lv_obj_set_style_bg_color(card, C_PNL2, LV_STATE_PRESSED);
        bool stale = (millis() - d.lastHeard > 30000);
        lv_obj_set_style_border_color(card, stale ? C_MUT : C_RED, 0);
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_radius(card, 2, 0);
        lv_obj_add_event_cb(card, ev_open_assign, LV_EVENT_CLICKED, &trackedDevs[uidx[i]]);

        lv_obj_t *n = lv_label_create(card);
        lv_label_set_text(n, d.id);
        lv_obj_set_style_text_color(n, stale ? C_DIM : C_WHITE, 0);
        lv_obj_set_style_text_font(n, &lv_font_montserrat_20, 0);
        lv_obj_set_pos(n, 10, 6);

        lv_obj_t *st = lv_label_create(card);
        lv_label_set_text(st, "tap to assign callsign");
        lv_obj_set_style_text_color(st, C_AMB_BRT, 0);
        lv_obj_set_style_text_font(st, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(st, 10, 38);

        lv_obj_t *ic = lv_label_create(card);
        lv_label_set_text(ic, ">");
        lv_obj_set_style_text_color(ic, C_RED, 0);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_28, 0);
        lv_obj_align(ic, LV_ALIGN_RIGHT_MID, -20, 0);
    }

    if (unN == 0) {
        lv_obj_t *e = lv_label_create(unassignedList);
        lv_label_set_text(e, "All datapads have been claimed.");
        lv_obj_set_style_text_color(e, C_DIM, 0);
        lv_obj_set_style_text_font(e, &lv_font_montserrat_16, 0);
        lv_obj_set_width(e, lv_pct(100));
        lv_obj_set_style_text_align(e, LV_TEXT_ALIGN_CENTER, 0);
    }

    for (int i = 0; i < panelN; i++) {
        TrackedDevice &d = trackedDevs[pidx[i]];
        lv_obj_t *card = lv_obj_create(panelList);
        lv_obj_set_size(card, 220, 100);
        lv_obj_set_style_bg_color(card, C_PNL, 0);
        bool stale = (millis() - d.lastHeard > 30000);
        lv_obj_set_style_border_color(card, stale ? C_RED : C_GRN, 0);
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_radius(card, 4, 0);

        lv_obj_t *n = lv_label_create(card);
        lv_label_set_text(n, d.id);
        lv_obj_set_style_text_color(n, C_WHITE, 0);
        lv_obj_set_style_text_font(n, &lv_font_montserrat_18, 0);
        lv_obj_set_pos(n, 10, 8);

        lv_obj_t *st = lv_label_create(card);
        lv_label_set_text(st, stale ? "OFFLINE" : "ONLINE");
        lv_obj_set_style_text_color(st, stale ? C_RED : C_GRN, 0);
        lv_obj_set_style_text_font(st, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(st, 10, 34);

        char age[24];
        unsigned long secs = (millis() - d.lastHeard) / 1000;
        snprintf(age, sizeof(age), "Last seen: %lus", secs);
        lv_obj_t *a = lv_label_create(card);
        lv_label_set_text(a, age);
        lv_obj_set_style_text_color(a, C_DIM, 0);
        lv_obj_set_style_text_font(a, &lv_font_montserrat_12, 0);
        lv_obj_set_pos(a, 10, 64);
    }

    if (statusLabel) {
        int onP = 0, onD = 0;
        for (int i = 0; i < deviceCount; i++) {
            bool live = (millis() - trackedDevs[i].lastHeard < 30000);
            if (live && trackedDevs[i].role == swts::ROLE_PANEL) onP++;
            if (live && trackedDevs[i].role == swts::ROLE_DATAPAD) onD++;
        }
        char sb[100];
        snprintf(sb, sizeof(sb), "GM ONLINE  //  %s  //  %s  //  %d PLAYERS  //  %d PANELS",
                 gmConfig.scenario, gmConfig.planet, onD, onP);
        lv_label_set_text(statusLabel, sb);
    }
}

void buildUI() {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, C_BG, 0);

    // Header
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_set_size(hdr, W, 56);
    lv_obj_set_pos(hdr, 0, 0);
    lv_obj_set_style_bg_color(hdr, C_PNL, 0);
    lv_obj_set_style_border_color(hdr, C_AMB, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(hdr, 2, 0);
    lv_obj_set_style_radius(hdr, 0, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *brand = lv_label_create(hdr);
    lv_label_set_text(brand, "SWTS GM COMMAND");
    lv_obj_set_style_text_color(brand, C_AMB_BRT, 0);
    lv_obj_set_style_text_font(brand, &lv_font_montserrat_28, 0);
    lv_obj_align(brand, LV_ALIGN_LEFT_MID, 16, 0);

    statusLabel = lv_label_create(hdr);
    lv_label_set_text(statusLabel, "INITIALIZING...");
    lv_obj_set_style_text_color(statusLabel, C_CYN, 0);
    lv_obj_set_style_text_font(statusLabel, &lv_font_montserrat_14, 0);
    lv_obj_align(statusLabel, LV_ALIGN_RIGHT_MID, -20, 0);

    // Tabs
    tabview = lv_tabview_create(scr, LV_DIR_TOP, 60);
    lv_obj_set_size(tabview, W, H - 56);
    lv_obj_set_pos(tabview, 0, 56);
    lv_obj_set_style_bg_color(tabview, C_BG, 0);

    lv_obj_t *btns = lv_tabview_get_tab_btns(tabview);
    lv_obj_set_style_bg_color(btns, C_PNL, 0);
    lv_obj_set_style_text_color(btns, C_TXT, 0);
    lv_obj_set_style_text_font(btns, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(btns, C_AMB_BRT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(btns, C_AMB, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_side(btns, LV_BORDER_SIDE_BOTTOM, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(btns, 3, LV_PART_ITEMS | LV_STATE_CHECKED);

    buildDashboardTab(lv_tabview_add_tab(tabview, "DASHBOARD"));
    buildEventsTab(lv_tabview_add_tab(tabview, "EVENTS"));
    buildCommsTab(lv_tabview_add_tab(tabview, "COMMS"));
    buildBountiesTab(lv_tabview_add_tab(tabview, "BOUNTIES"));
    buildPlayersTab(lv_tabview_add_tab(tabview, "PLAYERS"));
    buildPanelsTab(lv_tabview_add_tab(tabview, "PANELS"));
}

// ═══════════════════════════════════════════════
//  SD card on SPI (CS=10, MOSI=11, MISO=13, CLK=12)
// ═══════════════════════════════════════════════
bool sdOk = false;

// TCA9554PWR IO expander — controls LCD_BL, LCD_RST, TP_RST on Waveshare 7"
#define TCA9554_ADDR 0x20
void initIoExpander() {
    Wire.begin(8, 9, 400000);  // GT911 I2C bus
    delay(100);
    // Probe expander
    Wire.beginTransmission(TCA9554_ADDR);
    if (Wire.endTransmission() != 0) {
        Serial.println("[IOEX] TCA9554 not found at 0x20");
        return;
    }
    // Set all pins as outputs (config reg 0x03, write 0x00 = output)
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(0x03);
    Wire.write(0x00);
    Wire.endTransmission();
    // Set all outputs HIGH (output reg 0x01, write 0xFF)
    // This turns on backlight, releases LCD_RST and TP_RST
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(0x01);
    Wire.write(0xFF);
    Wire.endTransmission();
    Serial.println("[IOEX] TCA9554 configured, all outputs HIGH");
    delay(100);  // let LCD/touch come out of reset
}

void setup() {
    S.begin(115200);
    delay(2000);
    S.println("\n=== SWTS GM (Waveshare 7\" 800x480) ===");

    // Init IO expander FIRST — turns on backlight, releases LCD/touch reset
    initIoExpander();

    // Display + touch
    tft.init();
    tft.setRotation(0);
    tft.setBrightness(255);
    tft.fillScreen(0x0000);

    // SD card on Waveshare ESP32-S3-Touch-LCD-7: SDMMC 1-bit mode
    //   CLK=GPIO12, CMD=GPIO11, D0=GPIO13
    SD_MMC.setPins(12, 11, 13);
    if (SD_MMC.begin("/sdcard", true, false, BOARD_MAX_SDMMC_FREQ, 5)) {
        sdOk = true;
        S.printf("[SD] Mounted %lluMB\n", SD_MMC.cardSize() / (1024*1024));
        loadGmConfig();
        loadGmState();   // restore bounty/device state from previous session
    } else {
        S.println("[SD] Mount failed — check card / format (FAT32)");
    }

    // LVGL
    lv_init();
    buf1 = (lv_color_t *)heap_caps_malloc(W * 40 * sizeof(lv_color_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf1) buf1 = (lv_color_t *)malloc(W * 40 * sizeof(lv_color_t));
    lv_disp_draw_buf_init(&draw_buf, buf1, nullptr, W * 40);

    static lv_disp_drv_t dd;
    lv_disp_drv_init(&dd);
    dd.hor_res = W;
    dd.ver_res = H;
    dd.flush_cb = lvgl_flush;
    dd.draw_buf = &draw_buf;
    lv_disp_drv_register(&dd);

    static lv_indev_drv_t id;
    lv_indev_drv_init(&id);
    id.type = LV_INDEV_TYPE_POINTER;
    id.read_cb = lvgl_touch;
    lv_indev_drv_register(&id);

    buildUI();

    // Mesh
    WiFi.mode(WIFI_STA);
    swts::meshInit(gmConfig.gm_id, swts::ROLE_GM, onMeshMsg);

    // Resync after boot — every device replies with its current MSG_STATUS,
    // so tracked-device scores/scans refresh immediately rather than waiting
    // up to 5 s for the natural heartbeat.
    delay(300);                  // give the mesh a moment to settle
    swts::gmSyncRequest();
    S.println("[GM] Sync request broadcast");

    S.println("[GM] Ready");
}

void loop() {
    lv_timer_handler();

    static unsigned long lastRefresh = 0;
    if (millis() - lastRefresh > 2000) {
        lastRefresh = millis();
        refreshLiveLists();
        refreshDashboard();
        if (bountyModal) refreshModalPlayers();
    }

    // Debounced GM state save
    if (gmDirty && sdOk && millis() - gmLastSave > GM_SAVE_DEBOUNCE_MS) {
        gmDirty = false;
        if (saveGmState()) S.println("[GM] state saved");
    }

    delay(5);
}
