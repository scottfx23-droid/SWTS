/*
 * SFGE OTA — firmware updates from the SD/TF card
 *
 * Fleet workflow: updates travel the same way as content. Copy the built
 * binary onto the device's card as its DEVICE-SPECIFIC filename and boot:
 *
 *     .pio/build/datapad/firmware.bin     ->  /SFGE/firmware_datapad.bin
 *     .pio/build/gm_datapad/firmware.bin  ->  /SFGE/firmware_gm.bin
 *     .pio/build/panel/firmware.bin       ->  /SFGE/firmware_panel.bin
 *     .pio/build/terminal/firmware.bin    ->  /SFGE/firmware_terminal.bin
 *     .pio/build/droid/firmware.bin       ->  /SFGE/firmware_droid.bin
 *     .pio/build/guildmaster/firmware.bin ->  /SFGE/firmware_guild.bin
 *     .pio/build/actor/firmware.bin       ->  /SFGE/firmware_actor.bin
 *
 * The per-device name is deliberate: a card in the wrong prop can never
 * flash the wrong firmware.
 *
 * Safety: the file is renamed to firmware_flashing.bin BEFORE writing, so
 * a power cut mid-flash can't cause a retry loop (the ESP32 keeps booting
 * the old app until Update completes; the update is simply lost and the
 * operator re-copies). On success it becomes firmware_installed.bin and
 * the device reboots into the new build; on failure, firmware_failed.bin.
 *
 * Call sfge_ota::checkAndFlash(fs) right after the card mounts, before
 * anything else runs.
 */
#pragma once
#include <Arduino.h>
#include <FS.h>
#include <Update.h>

namespace sfge_ota {

#if defined(SFGE_DEVICE_DATAPAD)
  #define SFGE_FW_FILE "/SFGE/firmware_datapad.bin"
#elif defined(SFGE_DEVICE_GM)
  #define SFGE_FW_FILE "/SFGE/firmware_gm.bin"
#elif defined(SFGE_DEVICE_PANEL)
  #define SFGE_FW_FILE "/SFGE/firmware_panel.bin"
#elif defined(SFGE_DEVICE_TERMINAL)
  #define SFGE_FW_FILE "/SFGE/firmware_terminal.bin"
#elif defined(SFGE_DEVICE_DROID)
  #define SFGE_FW_FILE "/SFGE/firmware_droid.bin"
#elif defined(SFGE_DEVICE_GUILD)
  #define SFGE_FW_FILE "/SFGE/firmware_guild.bin"
#elif defined(SFGE_DEVICE_ACTOR)
  #define SFGE_FW_FILE "/SFGE/firmware_actor.bin"
#else
  #define SFGE_FW_FILE "/SFGE/firmware_unknown.bin"
#endif

inline void checkAndFlash(fs::FS &fsys) {
    if (!fsys.exists(SFGE_FW_FILE)) return;

    File f = fsys.open(SFGE_FW_FILE, "r");
    if (!f) return;
    size_t sz = f.size();
    f.close();
    Serial.printf("[OTA] %s found (%u bytes)\n", SFGE_FW_FILE, (unsigned)sz);
    if (sz < 100000) {   // sanity: a real app image is never this small
        Serial.println("[OTA] rejected: file too small to be firmware");
        fsys.remove("/SFGE/firmware_failed.bin");
        fsys.rename(SFGE_FW_FILE, "/SFGE/firmware_failed.bin");
        return;
    }

    // Take the file out of the trigger name first — no flash loops ever
    fsys.remove("/SFGE/firmware_flashing.bin");
    if (!fsys.rename(SFGE_FW_FILE, "/SFGE/firmware_flashing.bin")) {
        Serial.println("[OTA] rename failed — aborting");
        return;
    }
    f = fsys.open("/SFGE/firmware_flashing.bin", "r");
    if (!f) return;

    if (!Update.begin(sz)) {
        Serial.printf("[OTA] cannot start: %s\n", Update.errorString());
        f.close();
        fsys.remove("/SFGE/firmware_failed.bin");
        fsys.rename("/SFGE/firmware_flashing.bin", "/SFGE/firmware_failed.bin");
        return;
    }

    Serial.println("[OTA] flashing — do not power off...");
    uint8_t buf[4096];
    size_t written = 0;
    while (written < sz) {
        int n = f.read(buf, sizeof(buf));
        if (n <= 0) break;
        if (Update.write(buf, n) != (size_t)n) break;
        written += n;
    }
    f.close();

    bool ok = (written == sz) && Update.end(true);
    fsys.remove(ok ? "/SFGE/firmware_installed.bin" : "/SFGE/firmware_failed.bin");
    fsys.rename("/SFGE/firmware_flashing.bin",
                ok ? "/SFGE/firmware_installed.bin" : "/SFGE/firmware_failed.bin");

    if (ok) {
        Serial.println("[OTA] success — rebooting into new firmware");
        delay(400);
        ESP.restart();
    } else {
        Serial.printf("[OTA] FAILED after %u bytes: %s\n",
                      (unsigned)written, Update.errorString());
    }
}

} // namespace sfge_ota
