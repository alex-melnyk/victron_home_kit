/*
 * Victron MultiPlus-II GX → Apple HomeKit Bridge
 * Hardware: Arduino Nano ESP32 (NORA-W106 / ESP32-S3)
 *
 * Libraries required (install via Arduino Library Manager):
 *   - HomeSpan  (by Gregg Berman)     >= 2.1.x   (needs ESP32 core >= 3.3.0)
 *   - PubSubClient  (by Nick O'Leary)
 *   - ArduinoJson  (by Benoit Blanchon)
 *
 * Build/flash with the ESPRESSIF core (not Arduino's fork):
 *   FQBN esp32:esp32:nano_nora  — see flash.sh / the /flash command.
 *
 * A HomeKit BRIDGE with TWO logical accessories, so each device's readings are
 * grouped in its own detail page. HomeKit has no volt/watt unit, so in Apple's
 * Home app volts/watts are carried as "lux" (the number is exact; only the label
 * is a stand-in). The same values are ALSO exposed as Eve custom characteristics
 * (Voltage, Consumption) so Eve / Controller for HomeKit / Home+ show real V / W.
 * Tip: hide both from the Home tab via Edit Home View — reach them via details.
 *
 *   Accessory "Victron Battery":
 *     - Battery SOC     → Humidity Sensor  (0-100 %, "96%") [primary tile value]
 *     - Battery Service → Battery Level + Charging (shown in the "Status" block)
 *     - Battery Voltage → Light Sensor     ("53 lux" = 53 V)
 *   Accessory "Victron Inverter":
 *     - Inverter Power  → Light Sensor     ("675 lux" = 675 W, AC output) [primary]
 *     - Grid Power      → Light Sensor     ("720 lux" = 720 W, AC input)
 *
 * Victron MQTT topics used (VRM_ID + vebus instance verified live on this unit):
 *   Read (subscribe):
 *     N/<VRM_ID>/system/0/Dc/Battery/Soc
 *     N/<VRM_ID>/system/0/Dc/Battery/Voltage
 *     N/<VRM_ID>/system/0/Dc/Battery/Power       (sign → battery charging indicator)
 *     N/<VRM_ID>/vebus/275/Ac/Out/P              (inverter AC output power)
 *     N/<VRM_ID>/vebus/275/Ac/ActiveIn/P         (grid / AC input power)
 *   Write:
 *     R/<VRM_ID>/keepalive                       (empty, every ~30s, keeps values flowing)
 *
 * NOTE: the vebus instance (275 on the author's unit) is device-specific. Verify
 * yours by querying the live GX broker:
 *   mosquitto_sub -h <GX_IP> -t 'N/<VRM_ID>/vebus/#' -v   (look at the instance number)
 *   then update the "vebus/275/..." topics below to match.
 */

#include "HomeSpan.h"
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>

// Your private settings live in arduino_secrets.h (git-ignored).
// Copy arduino_secrets.example.h -> arduino_secrets.h and fill in your values.
#include "arduino_secrets.h"

// ============================================================
//  Configuration (values come from arduino_secrets.h)
// ============================================================

const char* WIFI_SSID     = SECRET_WIFI_SSID;
const char* WIFI_PASSWORD = SECRET_WIFI_PASSWORD;

// Victron GX IP address
const char* VICTRON_IP    = SECRET_VICTRON_IP;
const int   VICTRON_PORT  = 1883;             // 1883 = unsecured MQTT (default on Venus OS)

// VRM Portal ID
const char* VRM_ID        = SECRET_VRM_ID;

// HomeKit pairing code — 8 DIGITS, no dashes (you type it as XXX-XX-XXX in Home).
const char* HOMEKIT_CODE  = SECRET_HOMEKIT_CODE;

// MultiPlus serial number (cosmetic — shown in the Home app accessory details)
const char* DEVICE_SERIAL = SECRET_DEVICE_SERIAL;

// ============================================================
//  MQTT topic builder
// ============================================================
String topic(const char* path) {
  return String("N/") + VRM_ID + "/" + path;
}
String readTopic(const char* path) {
  return String("R/") + VRM_ID + "/" + path;
}
String writeTopic(const char* path) {
  return String("W/") + VRM_ID + "/" + path;
}

// ============================================================
//  Global state (updated by MQTT callbacks)
// ============================================================
float g_soc       = 0.0;   // Battery state of charge 0-100 %
float g_voltage   = 0.0;   // Battery voltage in V
float g_battPower = 0.0;   // Battery DC power in W (negative = charging)
float g_acOutP    = 0.0;   // Inverter AC output power in W (>= 0)
float g_gridP     = 0.0;   // Grid (active AC input) power in W

// ============================================================
//  Forward declarations
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length);
void mqttReconnect();
void mqttKeepAlive();

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

unsigned long lastKeepalive = 0;
const unsigned long KEEPALIVE_MS = 30000; // Victron drops silent clients after ~60s; refresh at 30s

unsigned long lastReconnectAttempt = 0;
const unsigned long RECONNECT_MS = 5000;  // retry MQTT every 5s without blocking HomeKit

// ============================================================
//  HomeKit: Battery Service (hidden — provides low-battery alert)
//  HomeKit renders BatteryService only in the accessory's Settings,
//  not as a tile, so we pair it with the SOC humidity tile below.
// ============================================================
struct VictronBattery : Service::BatteryService {

  Characteristic::BatteryLevel      battLevel;      // 0-100
  Characteristic::StatusLowBattery  statusLow;      // 0=Normal, 1=Low
  Characteristic::ChargingState     chargingState;  // 0=Not charging, 1=Charging, 2=Not chargeable

  VictronBattery() : Service::BatteryService() {
    battLevel.setVal(0);
    statusLow.setVal(0);
    chargingState.setVal(0);
  }

  void update(float soc, float battPower) {
    battLevel.setVal((int)soc);
    statusLow.setVal(soc < 20 ? 1 : 0);           // Low-battery alert below 20%
    chargingState.setVal(battPower < 0 ? 1 : 0);  // Negative DC power = battery charging
  }
};

// ============================================================
//  HomeKit: SOC as a Humidity Sensor
//  HumiditySensor's CurrentRelativeHumidity is a 0-100 % float,
//  which maps perfectly to state-of-charge and gives a real
//  percentage TILE in the Home app.
// ============================================================
struct VictronSocSensor : Service::HumiditySensor {

  Characteristic::CurrentRelativeHumidity soc;   // 0-100 %

  VictronSocSensor(const char* name) : Service::HumiditySensor() {
    new Characteristic::Name(name);              // labels this reading in the detail view
    soc.setVal(0);
  }

  void updateValue(float v) {
    if (v < 0)   v = 0;
    if (v > 100) v = 100;
    soc.setVal(v);
  }
};

// ============================================================
//  Eve custom characteristics — REAL units (V, W).
//  Apple's Home app ignores unknown characteristics (it keeps showing
//  "lux"), but Eve / Controller for HomeKit / Home+ recognise these
//  Eve UUIDs and display them with correct units. Must be declared at
//  global scope (after HomeSpan.h). Signature:
//    CUSTOM_CHAR(name, uuid, perms, format, default, min, max, staticRange)
// ============================================================
CUSTOM_CHAR(EveVoltage,     E863F10A-079E-48FF-8F27-9C2605A29F52, PR+EV, FLOAT, 0, 0,   400,   false);  // Volts
CUSTOM_CHAR(EveConsumption, E863F10D-079E-48FF-8F27-9C2605A29F52, PR+EV, FLOAT, 0, 0,   10000, false);  // Watts

enum EveUnit { EVE_NONE = 0, EVE_VOLT = 1, EVE_WATT = 2 };

// ============================================================
//  HomeKit: Generic numeric sensor for voltage / power
//  Uses a LightSensor so Apple's Home app shows the number on the tile
//  ("710 lux"). When eveUnit is set, it ALSO carries an Eve custom
//  characteristic so third-party apps show the real unit (V or W).
//  (LightSensor min is 0.0001, so we clamp up from 0.)
// ============================================================
struct VictronNumberSensor : Service::LightSensor {

  Characteristic::CurrentAmbientLightLevel value;  // lux; shown on the tile in Apple Home
  SpanCharacteristic* eve = nullptr;               // real V/W for Eve-aware apps

  VictronNumberSensor(const char* name, float initialVal, int eveUnit = EVE_NONE) : Service::LightSensor() {
    new Characteristic::Name(name);              // labels this reading in the detail view
    value.setVal(initialVal < 0.0001 ? 0.0001 : initialVal);
    float real = initialVal < 0 ? 0 : initialVal;
    if (eveUnit == EVE_VOLT) eve = new Characteristic::EveVoltage(real);
    else if (eveUnit == EVE_WATT) eve = new Characteristic::EveConsumption(real);
  }

  void updateValue(float v) {
    value.setVal(v < 0.0001 ? 0.0001 : v);       // clamp: LightSensor min is 0.0001
    if (eve) eve->setVal(v < 0 ? 0 : v);         // Eve char shows the true V/W
  }
};

// ============================================================
//  Global HomeKit service pointers
// ============================================================
VictronBattery*      hkBattery = nullptr;
VictronSocSensor*    hkSoc     = nullptr;
VictronNumberSensor* hkVoltage = nullptr;
VictronNumberSensor* hkPower   = nullptr;
VictronNumberSensor* hkGrid    = nullptr;

// ============================================================
//  MQTT callback — called when a message arrives
// ============================================================
void mqttCallback(char* topicStr, byte* payload, unsigned int length) {
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) return;

  float val = doc["value"] | 0.0f;
  String t = String(topicStr);

  Serial.printf("[MQTT] %s -> %.2f\n", topicStr, val);

  if (t.indexOf("Dc/Battery/Soc") >= 0) {
    g_soc = val;
    if (hkSoc)     hkSoc->updateValue(g_soc);
    if (hkBattery) hkBattery->update(g_soc, g_battPower);
  }
  else if (t.indexOf("Dc/Battery/Voltage") >= 0) {
    g_voltage = val;
    if (hkVoltage) hkVoltage->updateValue(g_voltage);
  }
  else if (t.indexOf("Ac/ActiveIn/P") >= 0) {
    g_gridP = val;                                 // power from the grid (AC input)
    if (hkGrid) hkGrid->updateValue(g_gridP);
  }
  else if (t.indexOf("Ac/Out/P") >= 0) {
    g_acOutP = val;
    if (hkPower) hkPower->updateValue(g_acOutP);
  }
  else if (t.indexOf("Dc/Battery/Power") >= 0) {
    g_battPower = val;                              // drives the charging indicator only
    if (hkBattery) hkBattery->update(g_soc, g_battPower);
  }
}

// ============================================================
//  MQTT reconnect + subscribe (non-blocking)
//  Attempts one connection per RECONNECT_MS so homeSpan.poll()
//  keeps running and HomeKit stays responsive if the GX is down.
// ============================================================
void mqttReconnect() {
  if (mqtt.connected()) return;
  if (millis() - lastReconnectAttempt < RECONNECT_MS) return;
  lastReconnectAttempt = millis();

  Serial.print("[MQTT] Connecting to Victron GX...");
  if (mqtt.connect("VictronHomeKit")) {
    Serial.println(" connected!");

    mqtt.subscribe(topic("system/0/Dc/Battery/Soc").c_str());
    mqtt.subscribe(topic("system/0/Dc/Battery/Voltage").c_str());
    mqtt.subscribe(topic("system/0/Dc/Battery/Power").c_str());
    mqtt.subscribe(topic("vebus/275/Ac/Out/P").c_str());
    mqtt.subscribe(topic("vebus/275/Ac/ActiveIn/P").c_str());

    // Prime values: keepalive makes the GX (re)publish everything; the broker
    // only forwards the topics we subscribed to above.
    mqtt.publish(readTopic("keepalive").c_str(), "");
    lastKeepalive = millis();
  } else {
    Serial.printf(" failed (rc=%d), will retry in %lus\n", mqtt.state(), RECONNECT_MS / 1000);
  }
}

// ============================================================
//  MQTT keepalive — Victron disconnects silent clients after ~60s
// ============================================================
void mqttKeepAlive() {
  if (millis() - lastKeepalive > KEEPALIVE_MS) {
    mqtt.publish(readTopic("keepalive").c_str(), "");
    lastKeepalive = millis();
    Serial.println("[MQTT] Keepalive sent");
  }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== Victron HomeKit Bridge ===");

  // --- WiFi (connect first so MQTT is usable; HomeSpan reuses this link) ---
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WiFi] Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());

  // --- MQTT ---
  mqtt.setServer(VICTRON_IP, VICTRON_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(512);

  // --- HomeSpan ---
  homeSpan.setLogLevel(1);
  homeSpan.begin(Category::Bridges, "Victron Bridge");
  homeSpan.setPairingCode(HOMEKIT_CODE);

  // Accessory 1: the bridge itself (required — AccessoryInformation only)
  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Victron Bridge");
      new Characteristic::Manufacturer("Victron Energy");
      new Characteristic::Model("MultiPlus-II GX 48/3000");
      new Characteristic::SerialNumber(DEVICE_SERIAL);
      new Characteristic::FirmwareRevision("2.1");

  // Accessory 2: "Victron Battery" — SOC (primary %) + battery Status + voltage.
  // Tap it → SOC %, Status(Battery Level, Charging), Battery Voltage.
  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Victron Battery");
      new Characteristic::Manufacturer("Victron Energy");
      new Characteristic::Model("MultiPlus-II 48/3000/35-32");
    hkSoc     = new VictronSocSensor("Battery SOC");   // humidity %  (primary)
    hkSoc->setPrimary();
    hkBattery = new VictronBattery();                  // Battery Level + Charging → Status
    hkVoltage = new VictronNumberSensor("Battery Voltage", 53.0, EVE_VOLT);  // lux + real V (Eve)

  // Accessory 3: "Victron Inverter" — inverter AC output + grid AC input.
  // Tap it → Inverter Power, Grid Power.
  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Victron Inverter");
      new Characteristic::Manufacturer("Victron Energy");
      new Characteristic::Model("MultiPlus-II 48/3000/35-32");
    hkPower = new VictronNumberSensor("Inverter Power", 0.0, EVE_WATT);  // lux + real W (Eve) (primary)
    hkPower->setPrimary();
    hkGrid  = new VictronNumberSensor("Grid Power", 0.0, EVE_WATT);      // lux + real W (Eve)

  Serial.println("[HomeSpan] Bridge defined (Battery + Inverter). Use your HomeKit pairing code to add it.");
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  // MQTT: non-blocking reconnect + service the connection when up
  mqttReconnect();          // self-throttles; no-op if already connected
  if (mqtt.connected()) {
    mqtt.loop();
    mqttKeepAlive();
  }

  // HomeSpan loop (handles HAP protocol, pairing, etc.) — always runs
  homeSpan.poll();
}
