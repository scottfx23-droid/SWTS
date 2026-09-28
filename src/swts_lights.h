/*
 * SWTS Lights — addressable RGB strips on props (5V / GND / DATA)
 *
 * Configured entirely from /SWTS/lights.txt so scenarios can restyle a
 * prop without reflashing. Format (lines starting with # are comments):
 *
 *     chip=ws2812          ws2812 | ws2811 | sk6812
 *     order=GRB            RGB | RBG | GRB | GBR | BRG | BGR
 *     pin=21               data GPIO (optional, default 21)
 *     RRGGBB,on_ms,off_ms  one line per LED, in strip order
 *
 * Per-LED timing:
 *     on_ms 0   -> always on (solid)
 *     on_ms -1  -> random flicker (faulty-cell / fire effect)
 *     otherwise -> blink: on_ms lit, off_ms dark
 *
 * Interaction feedback: flash() temporarily overrides the whole strip
 * with a blinking color, then the idle pattern resumes. HTTP handlers
 * run on the async task, so props set a pending-effect flag and call
 * these only from loop() (via applyPendingFx-style code in the prop).
 *
 * Call update() every loop pass.
 */
#pragma once
#include <Arduino.h>
#include <FS.h>
#include <Adafruit_NeoPixel.h>

namespace swts_lights {

#define LIGHTS_MAX_LEDS 64

enum LedMode : uint8_t { LED_SOLID, LED_BLINK, LED_FLICKER };

struct LedDef {
    uint32_t color;      // 0xRRGGBB
    int32_t  on_ms;
    int32_t  off_ms;
    LedMode  mode;
    // runtime
    bool          lit;
    unsigned long nextChange;
};

inline Adafruit_NeoPixel *strip = nullptr;
inline LedDef leds[LIGHTS_MAX_LEDS];
inline int    ledCount = 0;
inline int    dataPin  = 21;

// Effect override (set from loop context only)
inline uint32_t      fxColor   = 0;
inline uint8_t       fxLeft    = 0;      // remaining half-periods (on+off = 2)
inline uint16_t      fxPeriod  = 120;
inline unsigned long fxNext    = 0;
inline bool          fxLit     = false;

inline neoPixelType orderFlag(const char *o) {
    if (strcasecmp(o, "RGB") == 0) return NEO_RGB;
    if (strcasecmp(o, "RBG") == 0) return NEO_RBG;
    if (strcasecmp(o, "GBR") == 0) return NEO_GBR;
    if (strcasecmp(o, "BRG") == 0) return NEO_BRG;
    if (strcasecmp(o, "BGR") == 0) return NEO_BGR;
    return NEO_GRB;   // WS2812 default
}

inline uint32_t scale(uint32_t rgb, uint8_t pct) {
    uint8_t r = ((rgb >> 16) & 0xFF) * pct / 100;
    uint8_t g = ((rgb >> 8) & 0xFF) * pct / 100;
    uint8_t b = (rgb & 0xFF) * pct / 100;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

inline void setPixel(int i, uint32_t rgb) {
    strip->setPixelColor(i, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

// Parse a full lights config (file contents) and start the strip.
inline bool parseAndBegin(String content) {
    char chip[12] = "ws2812";
    char order[6] = "GRB";
    ledCount = 0;

    int pos = 0;
    while (pos < (int)content.length() && ledCount < LIGHTS_MAX_LEDS) {
        int nl = content.indexOf('\n', pos);
        if (nl < 0) nl = content.length();
        String line = content.substring(pos, nl);
        pos = nl + 1;
        line.trim();
        if (line.length() == 0 || line[0] == '#') continue;

        if (line.startsWith("chip="))       { strlcpy(chip, line.c_str() + 5, sizeof(chip)); }
        else if (line.startsWith("order=")) { strlcpy(order, line.c_str() + 6, sizeof(order)); }
        else if (line.startsWith("pin="))   { dataPin = line.substring(4).toInt(); }
        else {
            // LED line: RRGGBB,on_ms,off_ms
            int c1 = line.indexOf(',');
            if (c1 < 0) continue;
            int c2 = line.indexOf(',', c1 + 1);
            LedDef &d = leds[ledCount];
            memset(&d, 0, sizeof(d));
            d.color  = (uint32_t)strtoul(line.substring(0, c1).c_str(), nullptr, 16);
            d.on_ms  = line.substring(c1 + 1, c2 < 0 ? line.length() : c2).toInt();
            d.off_ms = (c2 < 0) ? 0 : line.substring(c2 + 1).toInt();
            if (d.on_ms < 0 || d.off_ms < 0) d.mode = LED_FLICKER;
            else if (d.on_ms == 0)           d.mode = LED_SOLID;
            else                             d.mode = LED_BLINK;
            if (d.on_ms  < 20 && d.mode == LED_BLINK) d.on_ms = 20;
            if (d.off_ms < 20 && d.mode == LED_BLINK) d.off_ms = 20;
            d.lit = true;
            ledCount++;
        }
    }
    if (ledCount == 0) { Serial.println("[LIGHTS] lights config has no LED lines"); return false; }

    neoPixelType t = orderFlag(order) |
                     ((strcasecmp(chip, "ws2811") == 0) ? NEO_KHZ400 : NEO_KHZ800);
    if (strip) { delete strip; strip = nullptr; }
    strip = new Adafruit_NeoPixel(ledCount, dataPin, t);
    strip->begin();
    strip->clear();
    strip->show();
    Serial.printf("[LIGHTS] %d LEDs, chip=%s order=%s pin=%d\n", ledCount, chip, order, dataPin);
    return true;
}

// Load /SWTS/lights.txt from the given filesystem and start the strip.
// Returns false (strip stays off) when the file is missing or empty.
inline bool load(fs::FS &fs, const char *path = "/SWTS/lights.txt") {
    File f = fs.open(path, "r");
    if (!f) { Serial.println("[LIGHTS] no lights.txt — strip disabled"); return false; }
    String content = f.readString();
    f.close();
    return parseAndBegin(content);
}

// Start the strip from an in-firmware pattern (props with no storage present)
inline bool loadBuiltin(const char *txt) {
    return parseAndBegin(String(txt));
}

// Blink the whole strip `times` times in `rgb`, then resume the idle pattern.
inline void flash(uint32_t rgb, uint8_t times = 3, uint16_t periodMs = 120) {
    if (!strip) return;
    fxColor  = rgb;
    fxLeft   = times * 2;
    fxPeriod = periodMs;
    fxNext   = 0;        // fire immediately on next update()
    fxLit    = false;
}

inline void effectActivity() { flash(0xFFAA00, 2, 100); }   // amber double-blink
inline void effectSuccess()  { flash(0x00FF40, 4, 120); }   // green celebration
inline void effectFail()     { flash(0xFF2000, 3, 170); }   // red rejection
inline void effectSignal()   { flash(0x00AAFF, 6, 90);  }   // cyan transmit

// Advance idle pattern + any active effect. Call every loop pass.
inline void update() {
    if (!strip || ledCount == 0) return;
    unsigned long now = millis();
    static unsigned long lastShow = 0;
    bool dirty = false;

    if (fxLeft > 0) {
        // Effect override — whole strip blinks fxColor
        if (now >= fxNext) {
            fxLit = !fxLit;
            for (int i = 0; i < ledCount; i++) setPixel(i, fxLit ? fxColor : 0);
            fxNext = now + fxPeriod;
            fxLeft--;
            dirty = true;
            if (fxLeft == 0) {
                // Restore idle state on the next pass
                for (int i = 0; i < ledCount; i++) leds[i].nextChange = 0;
            }
        }
    } else {
        for (int i = 0; i < ledCount; i++) {
            LedDef &d = leds[i];
            switch (d.mode) {
            case LED_SOLID:
                if (d.nextChange == 0) { setPixel(i, d.color); d.nextChange = 1; dirty = true; }
                break;
            case LED_BLINK:
                if (now >= d.nextChange) {
                    d.lit = (d.nextChange == 0) ? true : !d.lit;
                    setPixel(i, d.lit ? d.color : 0);
                    d.nextChange = now + (d.lit ? d.on_ms : d.off_ms);
                    dirty = true;
                }
                break;
            case LED_FLICKER:
                if (now >= d.nextChange) {
                    uint8_t roll = esp_random() % 100;
                    if (roll < 20) setPixel(i, 0);                              // wink out
                    else           setPixel(i, scale(d.color, 25 + roll % 76)); // 25-100%
                    d.nextChange = now + 50 + (esp_random() % 140);
                    dirty = true;
                }
                break;
            }
        }
    }

    // Rate-limit show() a little; WS28xx writes disable interrupts briefly
    if (dirty && now - lastShow >= 15) {
        strip->show();
        lastShow = now;
    }
}

} // namespace swts_lights
