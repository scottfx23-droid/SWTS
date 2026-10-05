/*
 * SFGE Provisioning — SD card to LittleFS copy
 * On boot: if SD has /SFGE/config.json, wipe flash and copy all files.
 * After copy, device runs entirely from flash. SD can be removed.
 */
#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <LittleFS.h>

// SD card pins — override before including if different per board
#ifndef SFGE_SD_CS
#define SFGE_SD_CS   -1   // -1 = no SD card slot on this device
#endif
#ifndef SFGE_SD_MOSI
#define SFGE_SD_MOSI -1
#endif
#ifndef SFGE_SD_CLK
#define SFGE_SD_CLK  -1
#endif
#ifndef SFGE_SD_MISO
#define SFGE_SD_MISO -1
#endif

namespace sfge {

inline void deleteRecursive(fs::FS &fs, const char *path) {
    File dir = fs.open(path);
    if (!dir || !dir.isDirectory()) return;
    File f = dir.openNextFile();
    while (f) {
        String fp = String(path) + "/" + f.name();
        if (f.isDirectory()) {
            f.close();
            deleteRecursive(fs, fp.c_str());
            fs.rmdir(fp.c_str());
        } else {
            f.close();
            fs.remove(fp.c_str());
        }
        f = dir.openNextFile();
    }
    dir.close();
}

inline void copyFile(fs::FS &src, const char *sp, fs::FS &dst, const char *dp) {
    File in = src.open(sp, FILE_READ);
    if (!in) return;
    File out = dst.open(dp, FILE_WRITE);
    if (!out) { in.close(); return; }
    uint8_t buf[512];
    while (in.available()) { size_t n = in.read(buf, sizeof(buf)); out.write(buf, n); }
    out.close(); in.close();
}

inline void copyDir(fs::FS &src, const char *sd, fs::FS &dst, const char *dd) {
    dst.mkdir(dd);
    File dir = src.open(sd);
    if (!dir || !dir.isDirectory()) return;
    File f = dir.openNextFile();
    while (f) {
        String sp = String(sd) + "/" + f.name();
        String dp = String(dd) + "/" + f.name();
        if (f.isDirectory()) {
            f.close();
            copyDir(src, sp.c_str(), dst, dp.c_str());
        } else {
            size_t sz = f.size();
            f.close();
            copyFile(src, sp.c_str(), dst, dp.c_str());
            Serial.printf("  %s (%d bytes)\n", dp.c_str(), sz);
        }
        f = dir.openNextFile();
    }
    dir.close();
}

// Returns true if files were copied from SD
inline bool provisionFromSD() {
    if (SFGE_SD_CS < 0) {
        Serial.println("[PROV] No SD card slot configured");
        return false;
    }

    Serial.println("[PROV] Checking SD card...");
    SPIClass sdSPI(HSPI);
    sdSPI.begin(SFGE_SD_CLK, SFGE_SD_MISO, SFGE_SD_MOSI, SFGE_SD_CS);
    pinMode(SFGE_SD_CS, OUTPUT);
    digitalWrite(SFGE_SD_CS, HIGH);

    if (!SD.begin(SFGE_SD_CS, sdSPI, 4000000)) {
        Serial.println("[PROV] No SD card found");
        sdSPI.end();
        return false;
    }

    if (!SD.exists("/SFGE/config.json")) {
        Serial.println("[PROV] No /SFGE/config.json on SD");
        SD.end(); sdSPI.end();
        return false;
    }

    Serial.println("[PROV] SD card found — copying to flash...");
    if (LittleFS.exists("/SFGE")) {
        Serial.println("[PROV] Erasing old data...");
        deleteRecursive(LittleFS, "/SFGE");
        LittleFS.rmdir("/SFGE");
    }

    Serial.println("[PROV] Copying files:");
    copyDir(SD, "/SFGE", LittleFS, "/SFGE");

    SD.end(); sdSPI.end();
    Serial.println("[PROV] Done — SD card can be removed");
    return true;
}

// Init flash filesystem
inline bool initFS() {
    if (!LittleFS.begin(true)) {
        Serial.println("[FS] LittleFS mount failed!");
        return false;
    }
    Serial.println("[FS] Flash filesystem ready");
    return true;
}

} // namespace sfge
