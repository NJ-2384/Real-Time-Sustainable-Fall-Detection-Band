<h1 align="center">🛟 Nexus — Real-Time Sustainable Fall Detection Band</h1>

<p align="center">
  <em>A low-cost wearable that detects falls, calls for help over 4G/GSM and shares live GPS location — no smartphone or Wi-Fi needed.</em>
</p>

<p align="center">
  <img alt="ESP32" src="https://img.shields.io/badge/MCU-ESP32-blue">
  <img alt="PlatformIO" src="https://img.shields.io/badge/build-PlatformIO-orange">
  <img alt="Modem" src="https://img.shields.io/badge/modem-SIMCOM%20A7672S-green">
  <img alt="MQTT" src="https://img.shields.io/badge/cloud-Adafruit%20IO%20MQTT-red">
  <img alt="SDG" src="https://img.shields.io/badge/UN%20SDG-3%20%7C%209%20%7C%2011-lightgrey">
</p>

Every year, falls among the elderly, children and people with disabilities go unnoticed — especially in rural areas where affordable monitoring doesn't exist. CCTV is fixed, costly and intrusive; fall-detecting smartwatches cost ₹89,000+ and need a phone. **Nexus** is a small band that works on a plain SIM card.

Built by **Neeraj Joshi** and **Priyanshi Mittal**

---

## ✨ Features

| | |
|---|---|
| 🧠 **3-stage fall detection** | Free-fall → impact spike → confirmation, using accelerometer magnitude (SVM) |
| ⏱️ **Pre-alarm grace period** | 10 s of beeping lets the wearer cancel a false alarm with one press |
| 🆘 **SOS button** | Instant alert on press |
| 📲 **SMS + call** | Sends a Google Maps link by SMS, then calls the caregiver |
| 📍 **Live GNSS tracking** | Position published to the cloud every 15 s |
| 🏠 **Geo-fence** | Set "home" from the dashboard; get INSIDE / OUTSIDE status by radius |
| ☁️ **Dashboard** | Adafruit IO feeds for status, location, fall state, acknowledge, fetch-location |
| 💾 **Survives reboots** | Pending alarm, home and radius persisted in flash |
| 🔁 **Dual-core** | Sensing and comms run on separate FreeRTOS tasks so a slow modem never blocks fall detection |

## 🔄 How it works

```
 IMU (SVM) ──► SensorTask (core 0) ──► state machine ──┐
                                                       │ fallTriggered
 A7672S modem ◄─ AT commands ◄── CommTask (core 1) ◄───┘
      │                                │
      ├─ SMS + voice call ──► caregiver│
      ├─ GNSS fix ────────────────────►│
      └─ MQTT ──► Adafruit IO ──► web / mobile dashboard
```

**State machine**

```
NORMAL ──(free-fall <0.5 g then impact >3 g within 400 ms)──► PRE-ALARM
PRE-ALARM ──(button press)──► NORMAL            (false alarm cancelled)
PRE-ALARM ──(10 s timeout)──► ALARM             (SMS + call + MQTT fall-stat=0)
NORMAL    ──(SOS button)────► ALARM
ALARM ──(dashboard ACK or button)──► NORMAL     (call hung up, fall-stat=1)
```

### MQTT feeds (`<user>/feeds/…`)

| Feed | Direction | Meaning |
|---|---|---|
| `status` | device → cloud | 1 online / 0 offline (last-will) |
| `fall-stat` | device → cloud | 0 = fall active, 1 = OK |
| `gnns/csv` | device → cloud | `0,lat,lon,0` for the map widget |
| `whl` / `gfs` | device → cloud | Home location text / geo-fence INSIDE·OUTSIDE |
| `ack` | cloud → device | Caregiver acknowledges the alarm |
| `fetch` | cloud → device | Request an SMS with current location |
| `shl` / `rad` | cloud → device | Set home here / set fence radius (m) |

## 🧰 Versions

| | V1 | V2 |
|---|---|---|
| Purpose | Proof of concept | Wearable prototype |
| Hardware | ESP32 dev board + MPU-6050 on a breadboard | ESP32 + MPU-6050 IMU, SIMCOM A7672S 4G modem (GSM + GNSS), LiPo, SOS button + status LED, 3D-printed case ([STLs](STL)) |
| Firmware | _not preserved_ | [`firmware`](firmware) |

**V1 — breadboard prototype**

<p align="center">
  <img width="320" alt="V1 breadboard prototype" src="https://github.com/user-attachments/assets/016d04eb-8822-4ce4-88e1-6564f85ae22c" />
  <img width="520" alt="V1 prototype" src="https://github.com/user-attachments/assets/ab996b3e-d7fc-439c-b65c-d6cacc8bef55" />
</p>

**Wiring diagram**

<p align="center">
  <img width="640" alt="Wiring diagram" src="https://github.com/user-attachments/assets/7a703dc8-0780-450e-8cb3-bb49a3fc8311" />
</p>

## 🔐 Configuration: `secrets.h`

Credentials never live in the source. The firmware reads them from `firmware/include/secrets.h`, which is **git-ignored**.

```bash
cd firmware
cp include/secrets.h.example include/secrets.h   # then edit it
```

| Macro | What to put |
|---|---|
| `MQTT_BROKER` | Broker URL, e.g. `tcp://io.adafruit.com:1883` |
| `MQTT_USER` | Your Adafruit IO username |
| `MQTT_PASS` | Your Adafruit IO key |
| `SOS_PHONE` | Caregiver number with country code, e.g. `+91XXXXXXXXXX` |

> Never commit `secrets.h`. If a key was ever pushed, rotate it in Adafruit IO.

## 🧹 Resetting saved state: `wipe_rom`

The band stores its **home location, geo-fence radius, last GPS fix and pending-alarm flag** in the ESP32's flash (NVS, namespace `nexus`). This survives reboots, so a stuck alarm will resume after power-up. To start clean, flash the wipe tool, then re-flash the main firmware:

```bash
cd firmware
pio run -e wipe -t upload       # clears the "nexus" namespace (watch the serial monitor)
pio run -e esp32dev -t upload   # back to the main firmware
```

Source: [`firmware/tools/wipe_rom.cpp`](firmware/tools/wipe_rom.cpp). On the next boot the firmware re-creates its defaults (radius 30 m, no home, no pending alarm).

## 🔮 Roadmap

- Heart-rate and SpO₂ monitoring
- ML-based activity recognition to cut false positives
- Cloud medical records for doctors
- Production-grade redesign (nRF5340 + LTE-M modem + on-sensor ML core, IP68 housing)
- Deployment for families, old-age homes and hazardous-work sites

## 🌍 Impact

Aligned with the Anveshana theme *Future Fabric: AI, Semiconductors & Innovation for Rural India* and UN SDGs **3** (Good Health), **9** (Industry & Innovation) and **11** (Sustainable Communities).