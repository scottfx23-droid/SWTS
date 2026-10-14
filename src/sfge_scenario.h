/*
 * SFGE Scenario selection — one card, many games
 *
 * A device's card may hold extra scenario content folders:
 *
 *     /SFGE/...                      <- default content (always present)
 *     /SFGE/scenarios/<id>/...       <- same layout, alternative scenario
 *
 * The GM picks a scenario at boot and broadcasts its id (MSG_SCENARIO).
 * Devices write the id to /SFGE/active_scenario.txt and reboot; at boot,
 * content loads from the scenario folder when the card has it.
 *
 * Fallback rule: a device WITHOUT that folder keeps running its default
 * /SFGE content as a generic side-game prop — its minigames still pay
 * credits, but its events won't match the scenario's missions, so it never
 * breaks the story. (player.json and other state files stay at /SFGE/.)
 *
 * Usage, after the filesystem mounts (and after test provisioning):
 *     sfge_scn::begin(SD);
 *     ... fs.open(sfge_scn::path("config.json")) ...
 * And in the MSG_SCENARIO mesh handler (defer the reboot to loop()):
 *     if (sfge_scn::applyScenario(SD, s->id)) scheduleReboot();
 */
#pragma once
#include <Arduino.h>
#include <FS.h>

namespace sfge_scn {

inline char activeId[24] = "";     // requested scenario ("" = defaults)
inline char root[56] = "/SFGE";    // where content actually loads from
inline bool haveContent = true;    // false = requested scenario not on card
                                   //         (side-game default mode)

// Marker file holding the requested id. The panel overrides this to a path
// outside /SFGE/ because it wipes and re-copies that tree at every boot.
inline const char *markerPath = "/SFGE/active_scenario.txt";

inline void begin(fs::FS &fsys) {
    activeId[0] = 0;
    strlcpy(root, "/SFGE", sizeof(root));
    haveContent = true;

    File f = fsys.open(markerPath, "r");
    if (f) {
        int n = f.readBytes(activeId, sizeof(activeId) - 1);
        f.close();
        while (n > 0 && (activeId[n-1] == '\n' || activeId[n-1] == '\r' || activeId[n-1] == ' '))
            n--;
        activeId[n < 0 ? 0 : n] = 0;
    }
    if (!activeId[0]) return;   // defaults

    char dir[56];
    snprintf(dir, sizeof(dir), "/SFGE/scenarios/%s", activeId);
    File d = fsys.open(dir);
    if (d && d.isDirectory()) {
        strlcpy(root, dir, sizeof(root));
        Serial.printf("[SCN] Active scenario '%s' -> %s\n", activeId, root);
    } else {
        haveContent = false;
        Serial.printf("[SCN] Scenario '%s' not on this card -- running default "
                      "content as a side-game prop\n", activeId);
    }
    if (d) d.close();
}

// Scenario-rooted content path ("config.json" -> "/SFGE/scenarios/<id>/config.json"
// or "/SFGE/config.json"). One static buffer: use the result before calling again.
inline const char* path(const char *rel) {
    static char buf[120];
    snprintf(buf, sizeof(buf), "%s/%s", root, rel);
    return buf;
}

// GM announced scenario `id`. Returns true when the marker changed and the
// device should reboot into it (callers defer the restart to loop context).
inline bool applyScenario(fs::FS &fsys, const char *id) {
    if (strcmp(id, activeId) == 0) return false;   // already on it
    if (!id[0]) fsys.remove(markerPath);
    else {
        File f = fsys.open(markerPath, FILE_WRITE);
        if (!f) { Serial.println("[SCN] marker write failed"); return false; }
        f.print(id);
        f.close();
    }
    Serial.printf("[SCN] Scenario switch '%s' -> '%s', rebooting\n", activeId, id);
    return true;
}

} // namespace sfge_scn
