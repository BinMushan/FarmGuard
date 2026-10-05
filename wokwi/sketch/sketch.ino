/*
  IoT-Based Intelligent Farm Decision & Waste Detection System
  ESP32 field node firmware (Wokwi simulation)

  Reads soil + air sensors, lets the farmer pick a planned action with a
  button, classifies it as NECESSARY / RISKY / UNNECESSARY, estimates the
  avoidable waste, shows it locally (OLED, LEDs, buzzer) and publishes
  everything over Wi-Fi (HTTP POST) to the cloud decision engine / LLM / WhatsApp.

  Serial monitor commands (115200 baud):
    rain <mm>     set the forecast rain for the next 24 h (simulated weather API)
    crop <1-3>    change crop profile
    act <0-3>     0 none, 1 irrigate, 2 fertilize, 3 spray (same as buttons)
    help
*/

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>      // built into the ESP32 core, no extra library needed
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>

// ===================== CONFIG =====================
#define USE_NETWORK   1                 // set 0 to run fully offline
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";
// Replace with your own backend. httpbin.org just echoes the request back (test only).
// If your backend replies with {"cloudVerdict":"NECESSARY|RISKY|UNNECESSARY"} the node
// shows that verdict instead of the local one for 60 s.
const char* API_URL = "http://httpbin.org/post";
const char* CURRENCY = "$";

const uint32_t SENSE_MS   = 2000;       // sensing + decision period
const uint32_t PUBLISH_MS = 10000;      // telemetry period
const float    NPK_MAX    = 200.0f;     // mg/kg at full-scale pot

// ===================== PINS (match diagram.json) =====================
const uint8_t PIN_MOIST = 34;
const uint8_t PIN_NTC   = 35;
const uint8_t PIN_PH    = 32;
const uint8_t PIN_N     = 33;
const uint8_t PIN_P     = 36;   // VP
const uint8_t PIN_K     = 39;   // VN
const uint8_t PIN_DHT   = 14;
const uint8_t PIN_BTN_IRR  = 16;
const uint8_t PIN_BTN_FERT = 4;
const uint8_t PIN_BTN_PEST = 17;
const uint8_t PIN_LED_G = 25;
const uint8_t PIN_LED_Y = 26;
const uint8_t PIN_LED_R = 27;
const uint8_t PIN_BUZZER = 18;
const uint8_t PIN_SDA = 21;
const uint8_t PIN_SCL = 22;

// ===================== TYPES =====================
enum Action  { ACT_NONE = 0, ACT_IRRIGATE, ACT_FERTILIZE, ACT_SPRAY };
enum Verdict { V_NONE = 0, V_NECESSARY, V_RISKY, V_UNNECESSARY };

const char* ANAME[] = {"NONE", "IRRIGATE", "FERTILIZE", "SPRAY"};
const char* VNAME[] = {"-", "NECESSARY", "RISKY", "UNNECESSARY"};

struct CropProfile {
  const char* name;                 // crop + growth stage
  float moistLow, moistHigh;        // target soil moisture band (%)
  float phMin, phMax;               // acceptable pH band
  float nTarget, pTarget, kTarget;  // target nutrient levels (mg/kg)
  float planWaterL, waterPrice;     // planned irrigation volume, price per L
  float planFertKg, fertPrice;      // planned fertilizer amount, price per kg
  float planChemL, chemPrice;       // planned pesticide volume, price per L
};

CropProfile crops[] = {
  // name               mL  mH  phMin phMax  N    P   K     water   $/L    fert  $/kg  chem  $/L
  {"Tomato-Flower",     55, 75, 6.0f, 6.8f, 120, 50, 150,   5000, 0.005f,   50, 0.8f,  10, 6.0f},
  {"Rice-Tillering",    70, 90, 5.5f, 7.0f, 100, 40, 120,  12000, 0.003f,   60, 0.6f,   8, 5.0f},
  {"Maize-Vegetative",  40, 65, 5.8f, 7.0f, 140, 45, 130,   6000, 0.004f,   70, 0.7f,  12, 5.5f},
};
const int NUM_CROPS = sizeof(crops) / sizeof(crops[0]);

struct SensorData {
  float moist = 0, soilT = 0, ph = 0, n = 0, p = 0, k = 0;
  float airT = 25.0f, hum = 60.0f;
  bool  dhtOk = false;
};

struct Decision {
  Verdict v;
  const char* reason;
  float avoidCost;
  float savedQty;
  const char* unit;
};

struct Btn { uint8_t pin; Action act; bool last; uint32_t t; };

// ===================== GLOBALS =====================
Adafruit_SSD1306 display(128, 64, &Wire, -1);
DHT dht(PIN_DHT, DHT22);
WiFiClient wifiClient;

SensorData sd;
Decision dec = {V_NONE, "Pick an action", 0, 0, "-"};
Action action = ACT_NONE;
int cropIdx = 0;
float rainMm = 0.0f;                 // simulated 24 h rain forecast

Btn btns[3] = {
  {PIN_BTN_IRR,  ACT_IRRIGATE,  HIGH, 0},
  {PIN_BTN_FERT, ACT_FERTILIZE, HIGH, 0},
  {PIN_BTN_PEST, ACT_SPRAY,     HIGH, 0},
};

bool forceUpdate = true, forcePublish = false, actionPressed = false;
uint32_t lastSense = 0, lastPublish = 0, lastDht = 0, nextPostAllowed = 0;
bool lastPostOk = false;
bool firstDht = true;
Verdict lastShown = V_NONE;
Verdict cloudVerdict = V_NONE;
uint32_t cloudAt = 0;
bool oledOk = false;

// ===================== SENSOR HELPERS =====================
float readAvg(uint8_t pin) {
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) { sum += analogRead(pin); delay(1); }
  return sum / 8.0f;
}

// Beta-equation conversion used by the Wokwi NTC part (Beta = 3950)
float ntcCelsius(float raw) {
  if (raw < 1) raw = 1;
  if (raw > 4094) raw = 4094;
  return 1.0f / (log(1.0f / (4095.0f / raw - 1.0f)) / 3950.0f + 1.0f / 298.15f) - 273.15f;
}

void readSensors(uint32_t now) {
  sd.moist = readAvg(PIN_MOIST) / 4095.0f * 100.0f;
  sd.soilT = ntcCelsius(readAvg(PIN_NTC));
  sd.ph    = readAvg(PIN_PH) / 4095.0f * 14.0f;
  sd.n     = readAvg(PIN_N) / 4095.0f * NPK_MAX;
  sd.p     = readAvg(PIN_P) / 4095.0f * NPK_MAX;
  sd.k     = readAvg(PIN_K) / 4095.0f * NPK_MAX;

  if (firstDht || now - lastDht >= 2000) {      // DHT22 needs >= 2 s between reads
    firstDht = false;
    lastDht = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) { sd.airT = t; sd.hum = h; sd.dhtOk = true; }
    else sd.dhtOk = false;                      // keep last valid values
  }
}

// ===================== DECISION ENGINE (local fallback) =====================
Decision evaluate(Action a, const SensorData& s, const CropProfile& c, float rain) {
  Decision d = {V_NONE, "Pick an action", 0, 0, "-"};

  switch (a) {
    case ACT_IRRIGATE:
      d.unit = "L";
      if (s.moist >= c.moistHigh)      { d.v = V_UNNECESSARY; d.reason = "Soil already wet"; }
      else if (rain >= 5.0f)           { d.v = V_UNNECESSARY; d.reason = "Rain forecast >=5mm"; }
      else if (s.moist < c.moistLow)   { d.v = V_NECESSARY;   d.reason = "Soil below target"; }
      else                             { d.v = V_RISKY;       d.reason = "Moisture in range"; }
      if (d.v == V_UNNECESSARY) { d.savedQty = c.planWaterL; d.avoidCost = c.planWaterL * c.waterPrice; }
      break;

    case ACT_FERTILIZE: {
      d.unit = "kg";
      bool allOk  = s.n >= c.nTarget && s.p >= c.pTarget && s.k >= c.kTarget;
      bool anyLow = s.n < 0.75f * c.nTarget || s.p < 0.75f * c.pTarget || s.k < 0.75f * c.kTarget;
      if (allOk)                                   { d.v = V_UNNECESSARY; d.reason = "NPK at/above target"; }
      else if (rain >= 10.0f)                      { d.v = V_RISKY;       d.reason = "Heavy rain: runoff"; }
      else if (s.ph < c.phMin || s.ph > c.phMax)   { d.v = V_RISKY;       d.reason = "pH out of range"; }
      else if (anyLow)                             { d.v = V_NECESSARY;   d.reason = "Nutrient deficit"; }
      else                                         { d.v = V_RISKY;       d.reason = "Marginal deficit"; }
      if (d.v == V_UNNECESSARY) { d.savedQty = c.planFertKg; d.avoidCost = c.planFertKg * c.fertPrice; }
      break;
    }

    case ACT_SPRAY: {
      // No pest sensor exists: humidity + temperature is used as a disease-pressure proxy.
      d.unit = "L";
      bool pressure = s.hum >= 80.0f && s.airT >= 18.0f && s.airT <= 32.0f;
      if (s.hum < 60.0f)        { d.v = V_UNNECESSARY; d.reason = "Low disease pressure"; }
      else if (rain >= 2.0f)    { d.v = V_RISKY;       d.reason = "Rain: wash-off risk"; }
      else if (pressure)        { d.v = V_NECESSARY;   d.reason = "High disease risk"; }
      else                      { d.v = V_RISKY;       d.reason = "Moderate pressure"; }
      if (d.v == V_UNNECESSARY) { d.savedQty = c.planChemL; d.avoidCost = c.planChemL * c.chemPrice; }
      break;
    }

    default:
      break;
  }
  return d;
}

// ===================== OUTPUTS =====================
void tone1k(uint16_t ms) {                      // simple square wave, works on any core version
  uint32_t end = millis() + ms;
  while (millis() < end) {
    digitalWrite(PIN_BUZZER, HIGH); delayMicroseconds(500);
    digitalWrite(PIN_BUZZER, LOW);  delayMicroseconds(500);
  }
}

void beepN(uint8_t n) {
  for (uint8_t i = 0; i < n; i++) { tone1k(120); delay(100); }
}

const char* netStatus() {
#if USE_NETWORK
  if (WiFi.status() == WL_CONNECTED) return lastPostOk ? "HTTP" : "WiFi";
#endif
  return "----";
}

void drawOled(Verdict v, bool cloud) {
  if (!oledOk) return;
  char b[32];
  const CropProfile& c = crops[cropIdx];

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  snprintf(b, sizeof(b), "%-16s %-4s", c.name, netStatus());
  display.setCursor(0, 0);  display.print(b);

  snprintf(b, sizeof(b), "M:%.0f%% St:%.1fC pH%.1f", sd.moist, sd.soilT, sd.ph);
  display.setCursor(0, 9);  display.print(b);

  snprintf(b, sizeof(b), "N%3.0f P%3.0f K%3.0f R%2.0fmm", sd.n, sd.p, sd.k, rainMm);
  display.setCursor(0, 18); display.print(b);

  snprintf(b, sizeof(b), "Air %.1fC  Hum %.0f%%", sd.airT, sd.hum);
  display.setCursor(0, 27); display.print(b);

  if (action == ACT_NONE) {
    snprintf(b, sizeof(b), "Press an action button");
  } else if (v == V_UNNECESSARY) {
    snprintf(b, sizeof(b), "%-9s Waste %s%.0f", ANAME[action], CURRENCY, dec.avoidCost);
  } else {
    snprintf(b, sizeof(b), "Action: %s", ANAME[action]);
  }
  display.setCursor(0, 36); display.print(b);

  // verdict line, inverted
  display.fillRect(0, 45, 128, 9, SSD1306_WHITE);
  display.setTextColor(SSD1306_BLACK);
  snprintf(b, sizeof(b), "%s%s", VNAME[v], cloud ? " (cloud)" : "");
  display.setCursor(2, 46); display.print(b);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 55); display.print(dec.reason);
  display.display();
}

void updateOutputs() {
  Verdict v = dec.v;
  bool cloud = false;
  if (action != ACT_NONE && cloudVerdict != V_NONE && millis() - cloudAt < 60000) {
    v = cloudVerdict;               // cloud decision overrides the local fallback
    cloud = true;
  }

  digitalWrite(PIN_LED_G, v == V_NECESSARY);
  digitalWrite(PIN_LED_Y, v == V_RISKY);
  digitalWrite(PIN_LED_R, v == V_UNNECESSARY);

  drawOled(v, cloud);

  if (v == V_UNNECESSARY && (lastShown != V_UNNECESSARY || actionPressed)) beepN(3);
  lastShown = v;
  actionPressed = false;
}

// ===================== NETWORK (HTTP POST to the cloud backend) =====================
void applyCloudReply(const String& body) {
  int i = body.indexOf("\"cloudVerdict\"");
  if (i < 0) return;
  String rest = body.substring(i, min((int)body.length(), i + 40));
  // check UNNECESSARY first: it contains the word NECESSARY
  if (rest.indexOf("UNNECESSARY") >= 0)    cloudVerdict = V_UNNECESSARY;
  else if (rest.indexOf("NECESSARY") >= 0) cloudVerdict = V_NECESSARY;
  else if (rest.indexOf("RISKY") >= 0)     cloudVerdict = V_RISKY;
  else return;
  cloudAt = millis();
  forceUpdate = true;
}

void publishTelemetry(uint32_t now) {
#if USE_NETWORK
  if (WiFi.status() != WL_CONNECTED) return;
  if (now < nextPostAllowed) return;            // back off after a failure

  char js[512];
  snprintf(js, sizeof(js),
    "{\"dev\":\"field01\",\"crop\":\"%s\",\"action\":\"%s\","
    "\"moisture\":%.1f,\"soilTemp\":%.1f,\"ph\":%.2f,"
    "\"n\":%.0f,\"p\":%.0f,\"k\":%.0f,"
    "\"airTemp\":%.1f,\"humidity\":%.1f,\"rainMm\":%.1f,"
    "\"verdict\":\"%s\",\"reason\":\"%s\","
    "\"avoidCost\":%.2f,\"savedQty\":%.0f,\"unit\":\"%s\"}",
    crops[cropIdx].name, ANAME[action],
    sd.moist, sd.soilT, sd.ph, sd.n, sd.p, sd.k,
    sd.airT, sd.hum, rainMm,
    VNAME[dec.v], dec.reason, dec.avoidCost, dec.savedQty, dec.unit);

  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(3000);
  if (!http.begin(wifiClient, API_URL)) {
    lastPostOk = false;
    nextPostAllowed = now + 30000;
    return;
  }
  http.addHeader("Content-Type", "application/json");
  int code = http.POST((uint8_t*)js, strlen(js));
  if (code > 0 && code < 300) {
    lastPostOk = true;
    applyCloudReply(http.getString());
  } else {
    lastPostOk = false;
    nextPostAllowed = now + 30000;
    Serial.printf(">> Telemetry POST failed (code %d), retry in 30 s\n", code);
  }
  http.end();
#endif
}

// ===================== INPUT =====================
void handleButtons(uint32_t now) {
  for (int i = 0; i < 3; i++) {
    bool r = digitalRead(btns[i].pin);
    if (r != btns[i].last && now - btns[i].t > 40) {   // 40 ms debounce
      btns[i].t = now;
      btns[i].last = r;
      if (r == LOW) {                                  // pressed (INPUT_PULLUP)
        action = btns[i].act;
        actionPressed = true;
        forceUpdate = true;
        forcePublish = true;
        Serial.printf(">> Planned action: %s\n", ANAME[action]);
      }
    }
  }
}

void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.startsWith("rain ")) {
    rainMm = line.substring(5).toFloat();
    Serial.printf(">> Rain forecast set to %.1f mm\n", rainMm);
  } else if (line.startsWith("crop ")) {
    int i = line.substring(5).toInt();
    if (i >= 1 && i <= NUM_CROPS) {
      cropIdx = i - 1;
      Serial.printf(">> Crop: %s\n", crops[cropIdx].name);
    }
  } else if (line.startsWith("act ")) {
    int a = line.substring(4).toInt();
    if (a >= 0 && a <= 3) { action = (Action)a; actionPressed = true; forcePublish = true; }
  } else {
    Serial.println("Commands: rain <mm> | crop <1-3> | act <0-3> | help");
  }
  forceUpdate = true;
}

void printStatus() {
  Serial.printf("[%s | %s] M=%.0f%% St=%.1fC pH=%.2f N=%.0f P=%.0f K=%.0f Air=%.1fC Hum=%.0f%% Rain=%.0fmm -> %s (%s)",
                crops[cropIdx].name, ANAME[action], sd.moist, sd.soilT, sd.ph, sd.n, sd.p, sd.k,
                sd.airT, sd.hum, rainMm, VNAME[dec.v], dec.reason);
  if (dec.v == V_UNNECESSARY)
    Serial.printf("  AVOIDABLE: %s%.2f (%.0f %s)", CURRENCY, dec.avoidCost, dec.savedQty, dec.unit);
  if (!sd.dhtOk) Serial.print("  [DHT read failed]");
  Serial.println();
}

// ===================== SETUP / LOOP =====================
void setup() {
  Serial.begin(115200);
  Serial.setTimeout(50);

  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_Y, OUTPUT);
  pinMode(PIN_LED_R, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_BTN_IRR,  INPUT_PULLUP);
  pinMode(PIN_BTN_FERT, INPUT_PULLUP);
  pinMode(PIN_BTN_PEST, INPUT_PULLUP);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);            // full 0-3.3 V range on ADC1 pins

  dht.begin();

  Wire.begin(PIN_SDA, PIN_SCL);
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  if (oledOk) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);  display.println("Farm Decision System");
    display.setCursor(0, 20); display.println("Starting...");
    display.display();
  } else {
    Serial.println("OLED not found");
  }

#if USE_NETWORK
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);       // channel 6 connects faster in Wokwi
#endif

  Serial.println("Farm Decision System ready. Type 'help' for commands.");
}

void loop() {
  uint32_t now = millis();

  handleSerial();
  handleButtons(now);

  if (forceUpdate || now - lastSense >= SENSE_MS) {
    forceUpdate = false;
    lastSense = now;
    readSensors(now);
    dec = evaluate(action, sd, crops[cropIdx], rainMm);
    updateOutputs();
    printStatus();
  }

  if (forcePublish || now - lastPublish >= PUBLISH_MS) {
    forcePublish = false;
    lastPublish = now;
    publishTelemetry(now);
  }
}
