// Minimal panel test — just WiFi AP
#include <WiFi.h>

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n=== PANEL TEST ===");
    WiFi.softAP("SWTS_PANEL_01", NULL, 6);
    Serial.printf("AP: SWTS_PANEL_01\nIP: %s\n", WiFi.softAPIP().toString().c_str());
    Serial.println("Ready.");
}

void loop() {
    delay(5000);
    Serial.printf("Clients: %d\n", WiFi.softAPgetStationNum());
}
