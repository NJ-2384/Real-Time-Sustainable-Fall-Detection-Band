// Utility: wipes the "nexus" NVS namespace (home location, radius, pending-fall flag).
// Flash with:  pio run -e wipe -t upload   then re-flash the main firmware.
#include <Arduino.h>
#include <Preferences.h>

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== ESP32 PREFERENCES WIPER ===");

  Preferences pref;
  if (pref.begin("nexus", false)) {
    Serial.println(pref.clear() ? "[OK] 'nexus' namespace cleared." : "[ERROR] Failed to clear memory.");
    pref.end();
  } else {
    Serial.println("[ERROR] Could not open 'nexus' namespace.");
  }
  Serial.println("[DONE] Re-upload the main firmware.");
}

void loop() {}
