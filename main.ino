/****************************************************************
 *  ESP32 Air-Quality Monitor with Holt’s Forecasting           *
 *  – Dual-read SCD30, pre-emptive fan / cooling control        *
 *  – Halts SCD-30 immediately after second reading             *
 ****************************************************************/

#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include "Adafruit_SCD30.h"
#include <InfluxDbClient.h>
#include <InfluxDbCloud.h>
#include <esp_wifi.h>   // esp_wifi_stop()
#include <esp_bt.h>     // btStop()
#include <time.h>

/* ─────────────────────────────── User settings ─────────────────────────── */
const char* ssid     = "ali";
const char* password = "12345678";

const char* mqtt_server = "192.168.1.8";
const int   mqtt_port   = 1883;
const char* mqtt_topic  = "test/esp32";

#define INFLUXDB_URL    "https://us-east-1-1.aws.cloud2.influxdata.com"
#define INFLUXDB_TOKEN  "QrqLWiaXnfbRHrtyWxADDYnKQH-IJ4hR-x1W05QgcK_sbabjpPEqiz6ZrjKWmlonSUujoKRdcArNzo23zMVbGA=="
#define INFLUXDB_ORG    "1a05d41f13d1a80a"
#define INFLUXDB_BUCKET "air_quality"
#define TZ_INFO         "CET-1CEST,M3.5.0,M10.5.0/3"   // Europe / Rome

/* ─────────────────────────────── Tuning constants ───────────────────────── */
static constexpr float ALPHA_CO2        = 0.10f;
static constexpr float BETA_CO2         = 0.03f;
static constexpr float ALPHA_TEMP       = 0.10f;
static constexpr float BETA_TEMP        = 0.03f;
static constexpr float LOOKAHEAD_MIN    = 2.0f;

static constexpr float DRIFT_PPM        = 50.0f;
static constexpr float EPSILON          = 0.01f;
static constexpr float MIN_SLEEP_MIN    = 0.5f;   // 30 s
static constexpr float MAX_SLEEP_MIN    = 15.0f;  // 15 min
static constexpr float EMPTY_SLEEP_MIN  = 20.0f;  // 20 min

/* ───────────────────────────── RTC-persistent state ────────────────────── */
RTC_DATA_ATTR float  lastCo2   = NAN, lastTemp = NAN;
RTC_DATA_ATTR time_t lastEpoch = 0;

RTC_DATA_ATTR float  L_co2 = NAN,  T_co2 = 0.0f;
RTC_DATA_ATTR float  L_temp = NAN, T_temp = 0.0f;

RTC_DATA_ATTR bool   fanWasOn     = false;
RTC_DATA_ATTR bool   coolingWasOn = false;

RTC_DATA_ATTR bool   ntpDone  = false;
RTC_DATA_ATTR int    wakeCount = 0;

/* ───────────────────────────── Global objects ─────────────────────────── */
WiFiClient      net;
PubSubClient    mqtt(net);
Adafruit_SCD30  scd30;
InfluxDBClient  influx(INFLUXDB_URL, INFLUXDB_ORG,
                       INFLUXDB_BUCKET, INFLUXDB_TOKEN,
                       InfluxDbCloud2CACert);
Point influxPt("air_quality");

/* ───────────────────────────── Runtime variables ───────────────────────── */
float co2_i = 0, temp_i = 0, hum_i = 0;
float dCo2_i = 0, dTemp_i = 0;
float co2_pred = 0, temp_pred = 0;

/* ───────────────────────────── Helper prototypes ───────────────────────── */
void initSerial();
void initPowerSavings();
void connectWiFiAndMQTT(bool doNtpAtColdBoot);
bool readSCD30(float &co2, float &temp, float &hum);
bool checkEmptyRoom(float co2);
void computeSlopes(float co2, float temp, float &dCo2, float &dTemp);
void updateHoltSmoothing(float co2, float temp, float dt);
void forecastValues(float lookaheadMin);
void decideActuators();
void publishPayload(bool preVent, bool preCool,
                    const String &fanAct, const String &coolAct);
void writeToInfluxDB(bool preVent);
float computeNextSleep(float dCo2);
void storeState(float co2, float temp, time_t epoch);
void shutdownPeripherals();
void enterDeepSleep(float minutes);

/* ───────── Low-level: stop SCD-30 (command 0x0104, no CRC needed) ─────── */
void stopSCD30() {
  Wire.beginTransmission(SCD30_I2CADDR_DEFAULT); // 0x61
  Wire.write(0x01);   // MSB of 0x0104
  Wire.write(0x04);   // LSB
  Wire.endTransmission();
  delay(100);         // let the sensor finish shutting down
}

/* ────────────────────────────────── setup ───────────────────────────────── */
void setup() {
  initSerial();
  initPowerSavings();

  wakeCount++;
  Serial.printf("\n=== Wake #%d ===\n", wakeCount);

  connectWiFiAndMQTT(!ntpDone);

  /* 1) sensor read */
  if (!readSCD30(co2_i, temp_i, hum_i)) {
    Serial.println(F("[SCD30] Read failed → sleep 2 min"));
    enterDeepSleep(2.0f);
  }

  /* 2) empty-room shortcut */
  if (checkEmptyRoom(co2_i)) {
    Serial.printf("[Sleep] Room empty → %.1f min\n", EMPTY_SLEEP_MIN);
    enterDeepSleep(EMPTY_SLEEP_MIN);
  }

  /* 3) Δt since last wake */
  time_t now = time(nullptr);
  float dt = (ntpDone && lastEpoch) ? (now - lastEpoch) / 60.0f : 0.0f;

  /* 4-7) processing */
  computeSlopes(co2_i, temp_i, dCo2_i, dTemp_i);
  Serial.printf("[Slope] CO₂ %.1f ppm/min  Temp %.2f °C/min\n",
                dCo2_i, dTemp_i);

  updateHoltSmoothing(co2_i, temp_i, dt);
  forecastValues(LOOKAHEAD_MIN);
  decideActuators();

  /* 8) MQTT + Influx */
  bool preVent = (co2_pred  >= 1000.0f);
  bool preCool = (temp_pred >=   26.0f);
  String fanAct  = fanWasOn     ? "TURNED_ON"  : "TURNED_OFF";
  String coolAct = coolingWasOn ? "TURNED_ON"  : "TURNED_OFF";
  publishPayload(preVent, preCool, fanAct, coolAct);
  writeToInfluxDB(preVent);

  /* 9) next sleep */
  float nextSleep = computeNextSleep(dCo2_i);
  Serial.printf("[Sleep] Next %.2f min\n", nextSleep);

  /* 10) persist & sleep */
  if (ntpDone && now > 946684800UL) storeState(co2_i, temp_i, now);
  Serial.printf("[Deep-sleep] %.2f min …\n\n", nextSleep);
  enterDeepSleep(nextSleep);
}

void loop() { /* never reached */ }

/* ────────────────────────── Power-down helpers ─────────────────────────── */
void shutdownPeripherals() {
  stopSCD30();                       // safe even if already stopped
  if (mqtt.connected()) mqtt.disconnect();

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();

  Wire.end();                        // release SDA/SCL
}

void enterDeepSleep(float minutes) {
  shutdownPeripherals();
  Serial.flush();
  delay(200);
  esp_deep_sleep((uint64_t)(minutes * 60e6));
}

/* ───────────────────────────── System helpers ─────────────────────────── */
void initSerial() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && (millis() - t0 < 2000)) { /* wait */ }
  Serial.println(F("[Serial] Ready"));
}

void initPowerSavings() {
  setCpuFrequencyMhz(80);   // save ~5 mA
  btStop();                 // save ~5 mA
}

/* ───────────────────────────── Connection helpers ──────────────────────── */
void connectWiFiAndMQTT(bool doNtpAtColdBoot) {
  WiFi.mode(WIFI_STA);

  if (WiFi.status() != WL_CONNECTED) {
    Serial.print(F("[Wi-Fi] Connecting"));
    WiFi.begin(ssid, password);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 10000) {
      delay(500); Serial.print('.');
    }
    Serial.println(WiFi.status() == WL_CONNECTED ? F(" OK") : F(" FAIL"));
  } else {
    Serial.println(F("[Wi-Fi] Already connected"));
  }

  /* one-time NTP sync */
  if (doNtpAtColdBoot && WiFi.status() == WL_CONNECTED) {
    Serial.println(F("[Time] First-boot NTP sync…"));
    timeSync(TZ_INFO, "pool.ntp.org", "time.nis.gov");
    ntpDone = true;
    time_t synced = time(nullptr);
    Serial.printf(F("[Time] Now: %s"), ctime(&synced));
  }

  /* MQTT */
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(true);
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);

    if (!mqtt.connected()) {
      mqtt.setServer(mqtt_server, mqtt_port);
      String clientId = "ESP32-" + String(random(0xFFFF), HEX);
      Serial.print(F("[MQTT] Connecting … "));
      if (mqtt.connect(clientId.c_str())) Serial.println(F("OK"));
      else                                Serial.printf(F("FAIL rc=%d\n"),
                                                        mqtt.state());
    }
    mqtt.loop();
  } else {
    Serial.println(F("[MQTT] Skipped (no Wi-Fi)"));
  }
}

/* ───────────────────────────── Sensor helpers ─────────────────────────── */
bool readSCD30(float &co2, float &temp, float &hum) {
  if (!scd30.begin()) return false;

  scd30.startContinuousMeasurement();
  Serial.println(F("[SCD30] Waiting for stable CO₂…"));

  float c = 0, t = 0, h = 0;
  int   count = 0;
  unsigned long t0 = millis();

  while (count < 2 && (millis() - t0) < 10000) {
    if (scd30.dataReady()) {
      if (scd30.read() && scd30.CO2 > 0.0f) {
        c = scd30.CO2;
        t = scd30.temperature;
        h = scd30.relative_humidity;
        count++;
        if (count == 1)
          Serial.printf("[SCD30] First reading discarded: CO₂ %.1f  T %.1f\n",
                        c, t);
        else
          Serial.printf("[SCD30] Second reading accepted: CO₂ %.1f  T %.1f  RH %.1f%%\n",
                        c, t, h);
      }
    }
    delay(200);
  }

  if (count < 2) return false;

  /* ─── NEW ✱✱✱  Halt the SCD-30 immediately ─── */
  stopSCD30();                           // power-down right here
  Wire.end();                            // optional: release bus sooner
  /* ───────────────────────────────────────────── */

  co2 = c; temp = t; hum = h;
  return true;
}

bool checkEmptyRoom(float co2) {
  if (co2 < 500.0f) {
    Serial.println(F("[Sleep] CO₂ < 500 ppm → room empty"));
    return true;
  }
  return false;
}

/* ───────────────────── Forecasting / smoothing helpers ─────────────────── */
void computeSlopes(float co2, float temp, float &dCo2, float &dTemp) {
  float dt = (lastEpoch == 0) ? 0.0f :
             ((time(nullptr) - lastEpoch) / 60.0f);
  dCo2 = dTemp = 0.0f;
  if (dt >= 0.166f && !isnan(lastCo2)) {
    dCo2  = (co2  - lastCo2)  / dt;
    dTemp = (temp - lastTemp) / dt;
  }
}

void updateHoltSmoothing(float co2, float temp, float dt) {
  if (isnan(L_co2)) { L_co2 = co2; T_co2 = 0.0f; }
  else {
    float prevL = L_co2, prevT = T_co2;
    L_co2 = ALPHA_CO2 * co2 +
            (1.0f - ALPHA_CO2) * (prevL + prevT * dt);
    T_co2 = BETA_CO2 *
            ((L_co2 - prevL) / dt) +
            (1.0f - BETA_CO2) * prevT;
  }

  if (isnan(L_temp)) { L_temp = temp; T_temp = 0.0f; }
  else {
    float prevL = L_temp, prevT = T_temp;
    L_temp = ALPHA_TEMP * temp +
             (1.0f - ALPHA_TEMP) * (prevL + prevT * dt);
    T_temp = BETA_TEMP *
             ((L_temp - prevL) / dt) +
             (1.0f - BETA_TEMP) * prevT;
  }
}

void forecastValues(float lookaheadMin) {
  co2_pred  = L_co2  + T_co2  * lookaheadMin;
  temp_pred = L_temp + T_temp * lookaheadMin;
  Serial.printf("Forecast %.1f min → CO₂ %.1f ppm  Temp %.1f °C\n",
                lookaheadMin, co2_pred, temp_pred);
}

void decideActuators() {
  bool preVent  = (co2_pred  >= 1000.0f);
  bool preCool  = (temp_pred >=   26.0f);

  bool fanNow     = preVent  || (co2_i  > 1000.0f);
  bool coolingNow = preCool  || (temp_i >   26.0f);

  String fanAct  = "NO_CHANGE";
  String coolAct = "NO_CHANGE";

  if (fanNow && !fanWasOn)       fanAct = "TURNED_ON";
  else if (!fanNow && fanWasOn)  fanAct = "TURNED_OFF";
  fanWasOn = fanNow;

  if (coolingNow && !coolingWasOn)       coolAct = "TURNED_ON";
  else if (!coolingNow && coolingWasOn)  coolAct = "TURNED_OFF";
  coolingWasOn = coolingNow;

  Serial.printf("[Action] Fan: %s  Cooling: %s\n",
                fanAct.c_str(), coolAct.c_str());
}

/* ─────────────────────────── MQTT / Influx helpers ─────────────────────── */
void publishPayload(bool preVent, bool preCool,
                    const String &fanAct, const String &coolAct) {
  if (WiFi.status() != WL_CONNECTED || !mqtt.connected()) {
    Serial.println(F("[MQTT] Skipped publish (no conn)"));
    return;
  }

  String payload = "{";
  payload += "\"co2\":"          + String(co2_i, 1)  + ",";
  payload += "\"temp\":"         + String(temp_i, 1)  + ",";
  payload += "\"fan\":\""        + fanAct             + "\",";
  payload += "\"cooling\":\""    + coolAct            + "\",";
  payload += "\"co2Slope\":"     + String(dCo2_i, 1)  + ",";
  payload += "\"tempSlope\":"    + String(dTemp_i, 2) + ",";
  payload += "\"preemptVent\":";
  payload += preVent ? "true,"  : "false,";
  payload += "\"preemptCool\":";
  payload += preCool ? "true"   : "false";
  payload += "}";

  mqtt.publish(mqtt_topic, payload.c_str());
  Serial.println(F("[MQTT] Payload published"));
}

void writeToInfluxDB(bool preVent) {
  if (WiFi.status() != WL_CONNECTED || !influx.validateConnection()) {
    Serial.println(F("[InfluxDB] Skipped write (no conn)"));
    return;
  }

  influxPt.clearFields();
  influxPt.addTag("device", "esp32-vent");
  influxPt.addField("co2",         co2_i);
  influxPt.addField("temperature", temp_i);
  influxPt.addField("humidity",    hum_i);
  influxPt.addField("co2Slope",    dCo2_i);
  influxPt.addField("tempSlope",   dTemp_i);
  influxPt.addField("preempt",     preVent);

  influx.writePoint(influxPt);
  Serial.println(F("[InfluxDB] Write OK"));
}

/* ───────────────────────────── Sleep helpers ───────────────────────────── */
float computeNextSleep(float dCo2) {
  if (lastEpoch == 0) return 1.0f;   // first wake → 1 min
  float absSlope = fabs(dCo2);
  float next = DRIFT_PPM / (absSlope + EPSILON);
  if (next < MIN_SLEEP_MIN) next = MIN_SLEEP_MIN;
  if (next > MAX_SLEEP_MIN) next = MAX_SLEEP_MIN;
  return next;
}

void storeState(float co2, float temp, time_t epoch) {
  lastCo2  = co2;
  lastTemp = temp;
  if (epoch > 946684800UL) lastEpoch = epoch;  // > 1 Jan 2000
}
