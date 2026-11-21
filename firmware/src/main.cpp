#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include "secrets.h"

// --- Pins ---
#define PIN_MODEM_RX 17
#define PIN_MODEM_TX 16
#define PIN_SOS_BTN  0
#define PIN_LED      2

// --- Tuning ---
const float GRAVITY = 9.81;
const float FALL_G_LOW = 0.5;
const float FALL_G_HIGH = 3.0;
const int FALL_WIN = 400; 
const int DRIFT_THRESHOLD = 15;      
const long GRACE_PERIOD = 10000;

// --- Config ---
const char* broker = MQTT_BROKER;
const char* clientID = "Nexus_ESP32_V2";
const char* mqtt_user = MQTT_USER;
const char* mqtt_pass = MQTT_PASS;
const char* sosPhone = SOS_PHONE; 

// --- Shared State ---
volatile int systemState = 0; 
volatile bool sosRequest = false;
volatile bool fallTriggered = false; 
volatile bool ackReceived = false;   
volatile bool mqttConnected = false;
bool modemReady = false; 
bool syncRequired = false;

struct StoredData {
  float currentLat; float currentLon;
  float homeLat; float homeLon;
  int radius; bool homeValid; bool fallPending;
};

// Globals
Preferences pref;
Adafruit_MPU6050 mpu;
HardwareSerial& modemSerial = Serial2;
StoredData romData;

TaskHandle_t TaskSensor;
TaskHandle_t TaskComm;

// --- Helpers ---
String getField(String data, char separator, int index) {
  int found = 0;
  int strIndex[] = {0, -1};
  int maxIndex = data.length() - 1;
  for (int i = 0; i <= maxIndex && found <= index; i++) {
    if (data.charAt(i) == separator || i == maxIndex) {
      found++;
      strIndex[0] = strIndex[1] + 1;
      strIndex[1] = (i == maxIndex) ? i + 1 : i;
    }
  }
  return found > index ? data.substring(strIndex[0], strIndex[1]) : "";
}

String AT(String cmd, int timeout = 2000) {
  if (modemSerial.available() > 200) while (modemSerial.available()) modemSerial.read();
  String response = "";
  Serial.println("[AT SEND] " + cmd);
  modemSerial.println(cmd);
  unsigned long start = millis();
  while ((millis() - start) < (unsigned long)timeout) {
    while (modemSerial.available()) response += (char)modemSerial.read();
    if (response.indexOf("OK") != -1 || response.indexOf("ERROR") != -1 || response.indexOf(">") != -1) break;
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
  if (response.length() > 0) { Serial.print("[AT RECV] "); Serial.println(response); }
  return response;
}

bool sendData(String data, int timeout = 3000) {
  Serial.println("[DATA SEND] " + data);
  modemSerial.print(data); 
  String response = "";
  unsigned long start = millis();
  while ((millis() - start) < (unsigned long)timeout) {
    while (modemSerial.available()) response += (char)modemSerial.read();
    if (response.indexOf("OK") != -1 || response.indexOf("ERROR") != -1) break;
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
  Serial.println("[DATA RECV] " + response);
  return (response.indexOf("OK") != -1);
}

float calculateDistance(float lat1, float lon1, float lat2, float lon2) {
  if (lat1 == 0 || lat2 == 0) return 99999;
  float R = 6371000; 
  float dLat = radians(lat2 - lat1); float dLon = radians(lon2 - lon1);
  float a = sin(dLat/2)*sin(dLat/2) + cos(radians(lat1))*cos(radians(lat2))*sin(dLon/2)*sin(dLon/2);
  return R * (2 * atan2(sqrt(a), sqrt(1-a)));
}

String formatGeoString(float lat, float lon) {
  String latDir = (lat >= 0) ? "N" : "S";
  String lonDir = (lon >= 0) ? "E" : "W";
  return "Latitude: " + String(abs(lat), 4) + " " + latDir + "  Longitude: " + String(abs(lon), 4) + " " + lonDir;
}

float parseCoord(String raw, String dir) {
  if (raw.length() < 2) return 0.0;
  float dec = raw.toFloat();
  if (dir == "S" || dir == "W") dec *= -1;
  return dec;
}

void mqttPub(String feed, String payload, bool retained = true) {
  bool critical = (feed == "fall-stat" || feed == "status");
  if (!mqttConnected && !critical) return;

  String topic = String(mqtt_user) + "/feeds/" + feed;
  if (feed != "gnns/csv") Serial.printf("[MQTT PUB] %s -> %s\n", feed.c_str(), payload.c_str());
  
  String res = AT("AT+CMQTTTOPIC=0," + String(topic.length()));
  
  if (res == "") { // Modem timeout - likely off
     Serial.println("[MQTT] Modem Unresponsive. Setting Offline.");
     modemReady = false; mqttConnected = false; syncRequired = false;
     return;
  }

  if (res.indexOf(">") != -1) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    sendData(topic);
    if (AT("AT+CMQTTPAYLOAD=0," + String(payload.length())).indexOf(">") != -1) {
      vTaskDelay(100 / portTICK_PERIOD_MS);
      sendData(payload);
      int qos = (feed == "gnns/csv") ? 0 : 1;
      String pubRes = AT("AT+CMQTTPUB=0," + String(qos) + ",60" + String(retained ? ",1" : ""));
      if (pubRes.indexOf("ERROR") != -1) mqttConnected = false;
    } 
  } else if (res.indexOf("ERROR") != -1) mqttConnected = false;
}

void syncROMToBroker() {
  Serial.println("[COMM] Syncing ROM -> Broker...");
  mqttPub("status", "1", true); vTaskDelay(500 / portTICK_PERIOD_MS);
  mqttPub("fall-stat", romData.fallPending ? "0" : "1", true); vTaskDelay(500 / portTICK_PERIOD_MS);
  mqttPub("whl", romData.homeValid ? formatGeoString(romData.homeLat, romData.homeLon) : "Setup Home");
  if (romData.homeValid && romData.currentLat != 0) {
     float hDist = calculateDistance(romData.currentLat, romData.currentLon, romData.homeLat, romData.homeLon);
     mqttPub("gfs", (hDist > (float)romData.radius) ? "OUTSIDE" : "INSIDE");
  }
  
  if (romData.fallPending) {
     Serial.println("[COMM] Active alarm detected in ROM. Resuming alerts...");
     fallTriggered = true;
  }
}

// --- TASK 1: SENSORS (Core 0) ---
void SensorTask(void * pvParameters) {
  unsigned long freefallTime = 0;
  bool freefall = false;
  unsigned long preAlarmStart = 0;
  unsigned long lastBeep = 0;
  int beepCount = 0;
  unsigned long lastHeartbeat = 0;

  for(;;) {
    unsigned long now = millis();
    if (now - lastHeartbeat > 30000) {
      Serial.println("[HEARTBEAT] Sensor Task State: " + String(systemState));
      lastHeartbeat = now;
    }

    if (digitalRead(PIN_SOS_BTN) == LOW) {
       vTaskDelay(50 / portTICK_PERIOD_MS); 
       if (digitalRead(PIN_SOS_BTN) == LOW) {
          sosRequest = true; 
          while(digitalRead(PIN_SOS_BTN) == LOW) vTaskDelay(10 / portTICK_PERIOD_MS); 
       }
    }

    sensors_event_t a, g, temp;
    if (mpu.getEvent(&a, &g, &temp)) {
       float svm = sqrt(pow(a.acceleration.x,2) + pow(a.acceleration.y,2) + pow(a.acceleration.z,2)) / GRAVITY;
       static unsigned long lastSvmLog = 0;
       if (now - lastSvmLog > 2000) {
          Serial.printf("[SENSOR] SVM: %.2f (State: %d)\n", svm, systemState);
          lastSvmLog = now;
       }

       if (systemState == 0) { 
          if (svm < FALL_G_LOW) { freefall = true; freefallTime = now; }
          if (freefall && (now - freefallTime < FALL_WIN)) {
             if (svm > FALL_G_HIGH) {
                systemState = 1; preAlarmStart = now; freefall = false;
             }
          } else if (now - freefallTime > FALL_WIN && freefall) freefall = false;
          
          if (sosRequest) {
             systemState = 2; sosRequest = false; fallTriggered = true; 
             romData.fallPending = true; pref.putBool("fallPending", true);
          }
       }
    }

    if (systemState == 1) { 
       if (now - lastBeep > 2000) {
          if (beepCount < 10) {
             digitalWrite(PIN_LED, (beepCount % 2 == 0) ? HIGH : LOW);
             beepCount++; vTaskDelay(50 / portTICK_PERIOD_MS);
          } else { lastBeep = now; beepCount = 0; }
       }
       if (sosRequest) { 
          systemState = 0; digitalWrite(PIN_LED, LOW); sosRequest = false; 
       } else if (now - preAlarmStart > GRACE_PERIOD) { 
          systemState = 2; fallTriggered = true; 
          romData.fallPending = true; pref.putBool("fallPending", true);
       }
    }

    if (systemState == 2) { 
       if (now - lastBeep > 200) { lastBeep = now; digitalWrite(PIN_LED, !digitalRead(PIN_LED)); }
       if (sosRequest) { systemState = 0; digitalWrite(PIN_LED, LOW); sosRequest = false; }
       if (ackReceived) { systemState = 0; digitalWrite(PIN_LED, LOW); ackReceived = false; }
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

// --- TASK 2: COMM (Core 1) ---
void CommTask(void * pvParameters) {
  unsigned long lastGpsTask = 0;
  bool callInProgress = false;
  unsigned long lastHeartbeat = 0;
  unsigned long lastConnAttempt = 0;

  pref.begin("nexus", false);
  
  // Check if first-run after wipe
  if (!pref.isKey("lat")) {
     Serial.println("[ROM] First run detected. Initializing defaults...");
     pref.putFloat("lat", 0); pref.putFloat("lon", 0);
     pref.putFloat("hLat", 0); pref.putFloat("hLon", 0);
     pref.putInt("rad", 30); pref.putBool("hVal", false);
     pref.putBool("fallPending", false);
  }

  romData.currentLat = pref.getFloat("lat", 0);
  romData.currentLon = pref.getFloat("lon", 0);
  romData.homeLat = pref.getFloat("hLat", 0);
  romData.homeLon = pref.getFloat("hLon", 0);
  romData.radius = pref.getInt("rad", 30);
  romData.homeValid = pref.getBool("hVal", false);
  romData.fallPending = pref.getBool("fallPending", false);
  if (romData.fallPending) systemState = 2;

  modemSerial.begin(115200, SERIAL_8N1, PIN_MODEM_RX, PIN_MODEM_TX);

  for(;;) {
    unsigned long now = millis();
    if (now - lastHeartbeat > 30000) { Serial.println("[HEARTBEAT] Comm Task Active"); lastHeartbeat = now; }

    // 1. Connection Management (Passive Check)
    if (!mqttConnected && (now - lastConnAttempt > 20000)) {
       lastConnAttempt = now;
       if (AT("AT", 500).indexOf("OK") != -1) {
          // If we just found the modem again, re-init core settings
          if (!modemReady) { 
             Serial.println("[COMM] Modem back online. Re-initializing settings...");
             AT("AT+CMGF=1"); 
             AT("AT+CGNSSPWR=1"); 
             modemReady = true; 
          }
          
          if (AT("AT+IPADDR").indexOf(".") == -1) { AT("AT+NETOPEN"); vTaskDelay(5000 / portTICK_PERIOD_MS); }
          
          // Check if identity is valid
          String accq = AT("AT+CMQTTACCQ?");
          if (accq.indexOf("ERROR") != -1 || accq.indexOf("0,\"\",0") != -1) {
             Serial.println("[COMM] Identity invalid or empty. Running Full Init...");
             AT("AT+CMQTTREL=0");
             AT("AT+CMQTTSTOP"); 
             vTaskDelay(1000 / portTICK_PERIOD_MS);
             AT("AT+CMQTTCFG=\"pdpidx\",0");
             AT("AT+CMQTTSTART"); vTaskDelay(1000 / portTICK_PERIOD_MS);
             
             String uniqueID = "Nexus_" + String(millis() % 10000);
             AT("AT+CMQTTACCQ=0,\"" + uniqueID + "\"");
          }

          // LWT Setup
          String sTop = String(mqtt_user) + "/feeds/status";
          if (AT("AT+CMQTTWILLTOPIC=0," + String(sTop.length())).indexOf(">") != -1) {
             if (sendData(sTop)) { if (AT("AT+CMQTTWILLMSG=0,1,1").indexOf(">") != -1) sendData("0"); }
          }

          String connCmd = "AT+CMQTTCONNECT=0,\"" + String(broker) + "\",60,1,\"" + String(mqtt_user) + "\",\"" + String(mqtt_pass) + "\"";
          String connRes = AT(connCmd, 10000);
          
          if (connRes.indexOf(": 0") != -1 || connRes.indexOf(": 3") != -1 || connRes.indexOf(": 9") != -1) {
             mqttConnected = true;
          } else if (connRes.indexOf("OK") != -1) {
             unsigned long wait = millis();
             while (millis() - wait < 15000) {
                if (modemSerial.available()) {
                   String urc = modemSerial.readString();
                   Serial.println("[URC] " + urc);
                   if (urc.indexOf(": 0") != -1 || urc.indexOf(": 3") != -1 || urc.indexOf(": 9") != -1) { 
                      mqttConnected = true; break; 
                   }
                }
                vTaskDelay(100 / portTICK_PERIOD_MS);
             }
          }
       }
    }

    // 2. ROM Sync (Once flag is true)
    if (mqttConnected && !syncRequired) {
       syncROMToBroker();
       String feeds[] = {"ack", "fetch", "shl", "rad"};
       for(String f : feeds) {
          String top = String(mqtt_user) + "/feeds/" + f;
          if (AT("AT+CMQTTSUBTOPIC=0," + String(top.length()) + ",1").indexOf(">") != -1) {
             sendData(top); AT("AT+CMQTTSUB=0"); vTaskDelay(1000 / portTICK_PERIOD_MS);
          }
       }
       syncRequired = true; 
    }

    // 3. Alert Triggers
    if (fallTriggered && mqttConnected) {
       fallTriggered = false; mqttPub("fall-stat", "0");
       String link = "https://maps.google.com/?q=" + String(romData.currentLat, 6) + "," + String(romData.currentLon, 6);
       if (AT("AT+CMGS=\"" + String(sosPhone) + "\"", 1000).indexOf(">") != -1) {
          modemSerial.print("EMERGENCY! Location: " + link); vTaskDelay(100 / portTICK_PERIOD_MS); 
          modemSerial.write(26); vTaskDelay(2000 / portTICK_PERIOD_MS);
       }
       AT("ATD" + String(sosPhone) + ";", 1000); callInProgress = true; 
    }

    if (systemState == 0 && callInProgress) {
       AT("ATH", 1000); callInProgress = false;
       romData.fallPending = false; pref.putBool("fallPending", false);
       mqttPub("fall-stat", "1");
    }

    // 4. GPS (Every 15s)
    if (modemReady && (now - lastGpsTask > 15000)) {
       lastGpsTask = now;
       String res = AT("AT+CGNSSINFO");
       if (res.indexOf("+CGNSSINFO:") != -1) {
          String rLat = getField(res, ',', 5); String dLat = getField(res, ',', 6);
          String rLon = getField(res, ',', 7); String dLon = getField(res, ',', 8);
          if (rLat.length() > 5 && rLon.length() > 5) {
             float decLat = parseCoord(rLat, dLat); float decLon = parseCoord(rLon, dLon);
             if (decLat != 0.0) {
                float dMoved = calculateDistance(decLat, decLon, romData.currentLat, romData.currentLon);
                romData.currentLat = decLat; romData.currentLon = decLon;
                mqttPub("gnns/csv", "0," + String(decLat, 6) + "," + String(decLon, 6) + ",0");
                if (dMoved > DRIFT_THRESHOLD) {
                   pref.putFloat("lat", decLat); pref.putFloat("lon", decLon);
                   if (romData.homeValid) {
                      float hDist = calculateDistance(decLat, decLon, romData.homeLat, romData.homeLon);
                      mqttPub("gfs", (hDist > (float)romData.radius) ? "OUTSIDE" : "INSIDE");
                   }
                }
             }
          }
       }
    }

    // 5. URC Listener
    if (modemSerial.available()) {
       String d = modemSerial.readString(); Serial.println("[MODEM] " + d);
       // Codes: 0=Success, 3=Session Active, 9=Already Connected
       if (d.indexOf("+CMQTTRX") != -1 || d.indexOf(": 0") != -1 || d.indexOf(": 3") != -1 || d.indexOf(": 9") != -1) mqttConnected = true;
       if (d.indexOf("+CMQTTCONNLOST") != -1) { mqttConnected = false; syncRequired = false; }
       if (d.indexOf("ack") != -1 && d.indexOf("1") != -1) { 
          ackReceived = true; 
          mqttPub("fall-stat", "1"); // Ensure dashboard updates on ack
       }
       if (d.indexOf("fetch") != -1 && d.indexOf("1") != -1) {
          String link = "https://maps.google.com/?q=" + String(romData.currentLat, 6) + "," + String(romData.currentLon, 6);
          if (AT("AT+CMGS=\"" + String(sosPhone) + "\"", 1000).indexOf(">") != -1) {
             modemSerial.print("Device Location: " + link); vTaskDelay(100 / portTICK_PERIOD_MS); modemSerial.write(26);
          }
       }
       if (d.indexOf("shl") != -1 && d.indexOf("1") != -1 && romData.currentLat != 0) {
          romData.homeLat = romData.currentLat; romData.homeLon = romData.currentLon; romData.homeValid = true;
          pref.putFloat("hLat", romData.homeLat); pref.putFloat("hLon", romData.homeLon); pref.putBool("hVal", true);
          mqttPub("whl", formatGeoString(romData.homeLat, romData.homeLon));
          mqttPub("gfs", "INSIDE");
       }
       if (d.indexOf("rad") != -1) {
          int c = d.lastIndexOf(",");
          if (c != -1) { 
             romData.radius = d.substring(c + 1).toInt(); pref.putInt("rad", romData.radius);
             if (romData.homeValid && romData.currentLat != 0) {
                float hDist = calculateDistance(romData.currentLat, romData.currentLon, romData.homeLat, romData.homeLon);
                mqttPub("gfs", (hDist > (float)romData.radius) ? "OUTSIDE" : "INSIDE");
             }
          }
       }
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_SOS_BTN, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  Wire.begin(21, 22);
  mpu.begin();
  xTaskCreatePinnedToCore(SensorTask, "Sensor", 10000, NULL, 1, &TaskSensor, 0); 
  xTaskCreatePinnedToCore(CommTask, "Comm", 15000, NULL, 1, &TaskComm, 1);     
}

void loop() { vTaskDelete(NULL); }
