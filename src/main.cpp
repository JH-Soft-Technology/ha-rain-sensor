/*
  Rain sensor for Home Assistant based on MQTT discovery.

  Features:
    - WiFi + MQTT configuration via a captive portal (WiFiManager),
      no credentials are stored in source code.
    - Web UI with live status, firmware update (ElegantOTA) and a
      "reset WiFi" button.
    - One JSON state message exposed in HA as several entities:
      cumulative rain (mm), rain rate (mm/h), raining (binary),
      WiFi RSSI, uptime and free heap.

  author: Jiri Horalek
  email: horalek.jiri@gmail.com
  site: https://github.com/JH-Soft-Technology/ha-rain-sensor
  version: 0.4.0
  last change: 03.06.2026
*/
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ElegantOTA.h>

#define MODEL "rainy 0.0.2"
#define SW_VERSION "0.4.0"

#define MQTT_MAX_TRANSFER_SIZE 1024
#define MQTT_INSTANCE_NAME "ha-rain-sensor"

// Captive portal access point shown on first boot / after a WiFi reset.
#define AP_NAME "RainSensor-Setup"

// Adaptive send interval: send frequently while raining for high
// resolution HA graphs, infrequently when dry to save resources.
#define RAIN_ACTIVE_INTERVAL_S 60    // every 60 s while raining
#define RAIN_IDLE_INTERVAL_S 1800    // every 30 min when dry
#define NO_RAIN_TIMEOUT_MS 1200000UL // 20 min without a tip -> idle mode
#define RAIN_NOW_WINDOW_MS 360000UL  // "raining" if a tip in last 6 min

#define MQTT_RECONNECT_INTERVAL_MS 5000
#define DEBOUNCE_MS 150 // reed-switch software debounce

// ---- MQTT discovery topics ----
#define TOPIC_STATE "homeassistant/sensor/" MQTT_INSTANCE_NAME "/state"
#define TOPIC_AVAILABILITY "homeassistant/sensor/" MQTT_INSTANCE_NAME "/availability"
#define PAYLOAD_ONLINE "online"
#define PAYLOAD_OFFLINE "offline"

#define CFG_RAIN "homeassistant/sensor/" MQTT_INSTANCE_NAME "/rain/config"
#define CFG_RATE "homeassistant/sensor/" MQTT_INSTANCE_NAME "/rate/config"
#define CFG_RSSI "homeassistant/sensor/" MQTT_INSTANCE_NAME "/rssi/config"
#define CFG_UPTIME "homeassistant/sensor/" MQTT_INSTANCE_NAME "/uptime/config"
#define CFG_HEAP "homeassistant/sensor/" MQTT_INSTANCE_NAME "/heap/config"
#define CFG_RAINING "homeassistant/binary_sensor/" MQTT_INSTANCE_NAME "/raining/config"

// On the Wemos D1 mini (ESP8266) all GPIO pins except D0 (GPIO16)
// support hardware interrupts, so D1 works fine with attachInterrupt.
const byte RAIN_PIN = D1;

// Rain gauge calibration: the MS-WH-SP-RG tips once per 0.2794 mm of rain.
const float MM_PER_TIP = 0.2794;

// ---- persisted MQTT configuration (saved to LittleFS) ----
struct Config
{
  char host[40];
  char port[6];
  char user[32];
  char pass[32];
};
Config config;

bool shouldSaveConfig = false;

WiFiClient wifi;
PubSubClient mqtt(wifi);
ESP8266WebServer server(80);
WiFiManager wm;

volatile unsigned int tipping_count = 0;  // bucket tips (modified in ISR)
volatile unsigned long last_tip_time = 0; // last counted tip (debounce + "raining")

float total_rain_mm = 0.0;  // cumulative rainfall since boot (mm)
float period_rain_mm = 0.0; // rain since last publish (for rate calc)
float last_rate = 0.0;      // last computed rate (mm/h), for the web UI

unsigned long last_send_rain = 0;
unsigned long last_mqtt_attempt = 0;

/*
  ISR - counts a bucket tip. The MS-WH-SP-RG reed switch closes to GND,
  so we trigger on the FALLING edge. A software debounce filters out
  contact bounce. No delay() because this runs in interrupt context.
*/
IRAM_ATTR void count_tipping()
{
  unsigned long now = millis();
  if (now - last_tip_time > DEBOUNCE_MS)
  {
    tipping_count++;
    last_tip_time = now;
  }
}

/*
  Read the current tip count without resetting it (atomic).
*/
unsigned int peek_tips()
{
  noInterrupts();
  unsigned int count = tipping_count;
  interrupts();
  return count;
}

/*
  Atomically subtract the tips that were already accounted for. We
  subtract (instead of zeroing) so tips arriving during processing are
  preserved.
*/
void consume_tips(unsigned int consumed)
{
  noInterrupts();
  tipping_count -= consumed;
  interrupts();
}

/*
  True if a tip occurred recently (used for the binary "raining" entity).
*/
boolean raining_now()
{
  return (millis() - last_tip_time) < RAIN_NOW_WINDOW_MS;
}

// ---------------------------------------------------------------------
//  Configuration persistence (LittleFS)
// ---------------------------------------------------------------------

/*
  Load the MQTT config from /config.json. Returns false if missing.
*/
boolean load_config()
{
  if (!LittleFS.exists("/config.json"))
  {
    return false;
  }
  File f = LittleFS.open("/config.json", "r");
  if (!f)
  {
    return false;
  }
  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err)
  {
    return false;
  }
  strlcpy(config.host, doc["host"] | "", sizeof(config.host));
  strlcpy(config.port, doc["port"] | "1883", sizeof(config.port));
  strlcpy(config.user, doc["user"] | "", sizeof(config.user));
  strlcpy(config.pass, doc["pass"] | "", sizeof(config.pass));
  return true;
}

/*
  Save the MQTT config to /config.json.
*/
void save_config()
{
  DynamicJsonDocument doc(512);
  doc["host"] = config.host;
  doc["port"] = config.port;
  doc["user"] = config.user;
  doc["pass"] = config.pass;

  File f = LittleFS.open("/config.json", "w");
  if (!f)
  {
    Serial.println("Failed to open config file for writing");
    return;
  }
  serializeJson(doc, f);
  f.close();
  Serial.println("Config saved");
}

void save_config_callback()
{
  shouldSaveConfig = true;
}

// ---------------------------------------------------------------------
//  Home Assistant MQTT discovery
// ---------------------------------------------------------------------

/*
  Fill a HA "device" object so all entities are grouped under one device.
*/
void add_device_info(JsonObject device)
{
  JsonArray ids = device.createNestedArray("ids");
  ids.add(MQTT_INSTANCE_NAME);
  device["mf"] = "JH SOFT Technology";
  device["mdl"] = MODEL;
  device["name"] = "Rain sensor";
  device["sw"] = SW_VERSION;
}

/*
  Build a generic sensor discovery config that reads one field from the
  shared JSON state topic via a value template.
*/
DynamicJsonDocument build_sensor_config(const char *name, const char *uniq,
                                        const char *val_tpl, const char *unit,
                                        const char *dev_cla, const char *stat_cla,
                                        const char *icon, boolean diagnostic)
{
  DynamicJsonDocument c(1024);
  c["name"] = name;
  c["uniq_id"] = uniq;
  c["stat_t"] = TOPIC_STATE;
  c["val_tpl"] = val_tpl;
  if (unit)
    c["unit_of_meas"] = unit;
  if (dev_cla)
    c["dev_cla"] = dev_cla;
  if (stat_cla)
    c["stat_cla"] = stat_cla;
  if (icon)
    c["icon"] = icon;
  if (diagnostic)
    c["ent_cat"] = "diagnostic";
  c["avty_t"] = TOPIC_AVAILABILITY;
  c["pl_avail"] = PAYLOAD_ONLINE;
  c["pl_not_avail"] = PAYLOAD_OFFLINE;
  add_device_info(c.createNestedObject("dev"));
  return c;
}

/*
  Build the binary "raining" discovery config.
*/
DynamicJsonDocument build_raining_config()
{
  DynamicJsonDocument c(1024);
  c["name"] = "Raining";
  c["uniq_id"] = "rain_sensor_raining";
  c["stat_t"] = TOPIC_STATE;
  c["val_tpl"] = "{{ 'ON' if value_json.raining else 'OFF' }}";
  c["dev_cla"] = "moisture";
  c["icon"] = "mdi:weather-pouring";
  c["avty_t"] = TOPIC_AVAILABILITY;
  c["pl_avail"] = PAYLOAD_ONLINE;
  c["pl_not_avail"] = PAYLOAD_OFFLINE;
  add_device_info(c.createNestedObject("dev"));
  return c;
}

// ---------------------------------------------------------------------
//  MQTT
// ---------------------------------------------------------------------

/*
  (Re)connect to the MQTT broker, non-blocking. Registers a Last Will so
  the broker marks the device offline if it drops. Publishes discovery
  and "online" on a successful (re)connect.
*/
boolean mqtt_reconnect()
{
  if (mqtt.connected())
  {
    return true;
  }
  if (millis() - last_mqtt_attempt < MQTT_RECONNECT_INTERVAL_MS)
  {
    return false;
  }
  last_mqtt_attempt = millis();

  Serial.print("Connecting to mqtt broker ...");

  boolean connected;
  if (strlen(config.user) > 0)
  {
    connected = mqtt.connect(MQTT_INSTANCE_NAME, config.user, config.pass,
                             TOPIC_AVAILABILITY, 0, true, PAYLOAD_OFFLINE);
  }
  else
  {
    connected = mqtt.connect(MQTT_INSTANCE_NAME, NULL, NULL,
                             TOPIC_AVAILABILITY, 0, true, PAYLOAD_OFFLINE);
  }

  if (!connected)
  {
    Serial.print(" failed, rc=");
    Serial.println(mqtt.state());
    return false;
  }

  Serial.println(" connected");
  mqtt.publish(TOPIC_AVAILABILITY, PAYLOAD_ONLINE, true);
  return true;
}

/*
  Serialize a discovery document and publish it (retained).
*/
boolean publish_config(DynamicJsonDocument doc, const char *topic)
{
  char buffer[MQTT_MAX_TRANSFER_SIZE];
  size_t n = serializeJson(doc, buffer, sizeof(buffer));
  if (n == 0)
  {
    Serial.println("Config serialization failed (buffer too small)");
    return false;
  }
  return mqtt.publish(topic, buffer, true);
}

/*
  Publish all discovery configs for the device's entities.
*/
void publish_discovery()
{
  publish_config(build_sensor_config("Rain", "rain_sensor",
                                     "{{ value_json.rain }}", "mm",
                                     "precipitation", "total_increasing",
                                     "mdi:weather-rainy", false),
                 CFG_RAIN);
  publish_config(build_sensor_config("Rain rate", "rain_sensor_rate",
                                     "{{ value_json.rate }}", "mm/h",
                                     "precipitation_intensity", "measurement",
                                     "mdi:weather-pouring", false),
                 CFG_RATE);
  publish_config(build_sensor_config("WiFi signal", "rain_sensor_rssi",
                                     "{{ value_json.rssi }}", "dBm",
                                     "signal_strength", "measurement",
                                     NULL, true),
                 CFG_RSSI);
  publish_config(build_sensor_config("Uptime", "rain_sensor_uptime",
                                     "{{ value_json.uptime }}", "s",
                                     "duration", "measurement",
                                     NULL, true),
                 CFG_UPTIME);
  publish_config(build_sensor_config("Free memory", "rain_sensor_heap",
                                     "{{ value_json.heap }}", "B",
                                     NULL, "measurement",
                                     "mdi:memory", true),
                 CFG_HEAP);
  publish_config(build_raining_config(), CFG_RAINING);
}

/*
  Publish the current state as one JSON message (retained).
*/
boolean publish_state()
{
  if (!mqtt.connected())
  {
    return false;
  }
  DynamicJsonDocument doc(256);
  doc["rain"] = round(total_rain_mm * 100) / 100.0;
  doc["rate"] = round(last_rate * 10) / 10.0;
  doc["raining"] = raining_now();
  doc["rssi"] = WiFi.RSSI();
  doc["uptime"] = millis() / 1000UL;
  doc["heap"] = ESP.getFreeHeap();

  char buffer[256];
  serializeJson(doc, buffer, sizeof(buffer));
  return mqtt.publish(TOPIC_STATE, buffer, true);
}

// ---------------------------------------------------------------------
//  Web UI
// ---------------------------------------------------------------------

/*
  Live status page. Auto-refreshes every 10 s.
*/
void handle_root()
{
  char html[1700];
  snprintf(html, sizeof(html),
           "<!DOCTYPE html><html><head><meta charset='utf-8'>"
           "<meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<meta http-equiv='refresh' content='10'>"
           "<title>Rain sensor</title><style>"
           "body{font-family:sans-serif;margin:1.5rem;max-width:480px;color:#222}"
           "h1{font-size:1.3rem}table{width:100%%;border-collapse:collapse}"
           "td{padding:.45rem;border-bottom:1px solid #e2e2e2}"
           ".v{text-align:right;font-weight:bold}"
           "a.btn{display:inline-block;margin-top:1rem;margin-right:.5rem;"
           "padding:.55rem 1rem;color:#fff;text-decoration:none;border-radius:6px}"
           "</style></head><body><h1>Rain sensor <small>v%s</small></h1><table>"
           "<tr><td>Total rain</td><td class='v'>%.2f mm</td></tr>"
           "<tr><td>Rate</td><td class='v'>%.1f mm/h</td></tr>"
           "<tr><td>Raining</td><td class='v'>%s</td></tr>"
           "<tr><td>WiFi signal</td><td class='v'>%d dBm</td></tr>"
           "<tr><td>IP address</td><td class='v'>%s</td></tr>"
           "<tr><td>Uptime</td><td class='v'>%lu s</td></tr>"
           "<tr><td>Free memory</td><td class='v'>%u B</td></tr>"
           "<tr><td>MQTT</td><td class='v'>%s</td></tr>"
           "</table>"
           "<a class='btn' style='background:#185FA5' href='/update'>Firmware update</a>"
           "<a class='btn' style='background:#A32D2D' href='/resetwifi'>Reset WiFi</a>"
           "</body></html>",
           SW_VERSION, total_rain_mm, last_rate, raining_now() ? "yes" : "no",
           WiFi.RSSI(), WiFi.localIP().toString().c_str(),
           millis() / 1000UL, ESP.getFreeHeap(),
           mqtt.connected() ? "connected" : "disconnected");
  server.send(200, "text/html", html);
}

/*
  Clear stored WiFi credentials and restart into the setup portal.
*/
void handle_reset_wifi()
{
  server.send(200, "text/html",
              "<html><body style='font-family:sans-serif;margin:2rem'>"
              "WiFi settings cleared. The device is restarting and will "
              "open the <b>" AP_NAME "</b> setup portal.</body></html>");
  delay(1000);
  wm.resetSettings();
  ESP.restart();
}

// ---------------------------------------------------------------------
//  Setup / loop
// ---------------------------------------------------------------------

void setup()
{
  Serial.begin(115200);

  pinMode(RAIN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(RAIN_PIN), count_tipping, FALLING);

  if (!LittleFS.begin())
  {
    Serial.println("LittleFS mount failed, formatting...");
    LittleFS.format();
    LittleFS.begin();
  }
  load_config();

  // WiFiManager: captive portal for WiFi + MQTT settings. The portal
  // scans for available networks and lets the user pick one and enter
  // the password, plus the MQTT broker details below.
  WiFiManagerParameter p_host("host", "MQTT host", config.host, sizeof(config.host) - 1);
  WiFiManagerParameter p_port("port", "MQTT port", config.port, sizeof(config.port) - 1);
  WiFiManagerParameter p_user("user", "MQTT user", config.user, sizeof(config.user) - 1);
  WiFiManagerParameter p_pass("pass", "MQTT password", config.pass, sizeof(config.pass) - 1);
  wm.addParameter(&p_host);
  wm.addParameter(&p_port);
  wm.addParameter(&p_user);
  wm.addParameter(&p_pass);
  wm.setSaveConfigCallback(save_config_callback);
  wm.setConfigPortalTimeout(180); // give up the portal after 3 min

  if (!wm.autoConnect(AP_NAME))
  {
    Serial.println("WiFi connect/portal failed, restarting");
    delay(2000);
    ESP.restart();
  }

  if (shouldSaveConfig)
  {
    strlcpy(config.host, p_host.getValue(), sizeof(config.host));
    strlcpy(config.port, p_port.getValue(), sizeof(config.port));
    strlcpy(config.user, p_user.getValue(), sizeof(config.user));
    strlcpy(config.pass, p_pass.getValue(), sizeof(config.pass));
    save_config();
  }

  WiFi.setAutoReconnect(true);
  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());

  // MQTT
  mqtt.setServer(config.host, atoi(config.port));
  mqtt.setBufferSize(MQTT_MAX_TRANSFER_SIZE);
  if (mqtt_reconnect())
  {
    publish_discovery();
  }

  // Web server + OTA
  server.on("/", handle_root);
  server.on("/resetwifi", handle_reset_wifi);
  ElegantOTA.begin(&server); // serves the /update page
  server.begin();
  Serial.println("Web server started on port 80");

  // force a send on the first loop iteration
  last_send_rain = millis() - RAIN_ACTIVE_INTERVAL_S * 1000UL;
}

void loop()
{
  server.handleClient();
  ElegantOTA.loop();

  if (WiFi.status() == WL_CONNECTED)
  {
    mqtt_reconnect();
    mqtt.loop();
  }

  // Accumulate tips immediately so they are never lost on a failed publish.
  unsigned int tips = peek_tips();
  if (tips > 0)
  {
    consume_tips(tips);
    float mm = tips * MM_PER_TIP;
    total_rain_mm += mm;
    period_rain_mm += mm;
  }

  // Adaptive interval based on recent rain activity.
  unsigned long interval_ms = (millis() - last_tip_time < NO_RAIN_TIMEOUT_MS)
                                  ? RAIN_ACTIVE_INTERVAL_S * 1000UL
                                  : RAIN_IDLE_INTERVAL_S * 1000UL;

  if (millis() - last_send_rain > interval_ms)
  {
    // rate in mm/h based on the rain accumulated over this period
    unsigned long elapsed_ms = millis() - last_send_rain;
    if (elapsed_ms > 0)
    {
      last_rate = (period_rain_mm / (elapsed_ms / 1000.0)) * 3600.0;
    }

    if (publish_state())
    {
      period_rain_mm = 0.0;
      last_send_rain = millis();
    }
  }
}
