/*
  Rain sensor for Home Assistant based on MQTT discovery.

  Will automatically create a rain sensor device, as long as the
  MQTT discovery integration is configured in Home Assistant.

  author: Jiri Horalek
  email: horalek.jiri@gmail.com
  site: https://github.com/JH-Soft-Technology/ha-rain-sensor
  version: 0.3.0
  last change: 03.06.2026
*/
#include <Arduino.h>
#include <PubSubClient.h>
#include <ESP8266WiFi.h>
#include <ArduinoJson.h>

// Secrets (WiFi + MQTT credentials) live in a separate, git-ignored
// header. Copy include/secrets.example.h to include/secrets.h and fill
// in your own values before building.
#include "secrets.h"

#define MODEL "rainy 0.0.2"
#define SW_VERSION "0.3.0"

#define MQTT_MAX_TRANSFER_SIZE 1024
#define MQTT_INSTANCE_NAME "ha-rain-sensor"
// Adaptive send interval: send more frequently while it is raining so
// HA graphs have good resolution; drop to a long interval when dry to
// save bandwidth and broker resources.
#define RAIN_ACTIVE_INTERVAL_S 60    // every 60 s while raining
#define RAIN_IDLE_INTERVAL_S 1800    // every 30 min when dry
#define NO_RAIN_TIMEOUT_MS 1200000UL // 20 min without a tip → idle mode

// How long (ms) to wait for WiFi / MQTT before giving up the current
// attempt. Nothing blocks forever any more: if a connection cannot be
// established the loop simply tries again on the next iteration.
#define WIFI_CONNECT_TIMEOUT_MS 15000
#define MQTT_RECONNECT_INTERVAL_MS 5000

#define TOPIC_PREFIX "homeassistant/sensor"

// Shared availability topic for the whole device (used by LWT).
#define TOPIC_AVAILABILITY TOPIC_PREFIX "/" MQTT_INSTANCE_NAME "/availability"
#define PAYLOAD_ONLINE "online"
#define PAYLOAD_OFFLINE "offline"

#define TOPIC_RAIN_SENSOR_UNIQUE_ID "rain_sensor" // unique id of the entity
#define TOPIC_RAIN_SENSOR_NAME "Rain sensor"
#define TOPIC_RAIN_SENSOR_CONFIG TOPIC_PREFIX "/" TOPIC_RAIN_SENSOR_UNIQUE_ID "/config"
#define TOPIC_RAIN_SENSOR_STATE TOPIC_PREFIX "/" TOPIC_RAIN_SENSOR_UNIQUE_ID "/state"

// On the Wemos D1 mini (ESP8266) all GPIO pins except D0 (GPIO16)
// support hardware interrupts, so D1 works fine with attachInterrupt.
const byte RAIN_PIN = D1;

// Rain gauge calibration: the MS-WH-SP-RG tips once per 0.2794 mm of rain.
const float MM_PER_TIP = 0.2794;

WiFiClient wifi;
PubSubClient mqtt(wifi); // mqtt client for publishing

volatile unsigned int tipping_count = 0;  // bucket tips (volatile - modified in ISR)
volatile unsigned long last_tip_time = 0; // timestamp of last counted tip (debounce)

unsigned long last_send_rain = 0;
unsigned long last_mqtt_attempt = 0;

float total_rain_mm = 0.0; // cumulative rainfall since boot (mm)

// Debounce window for the reed switch (ms). A real tip cannot occur
// faster than this; anything sooner is contact bounce and is ignored.
#define DEBOUNCE_MS 150

/*
  Interrupt Service Routine (ISR) - called automatically on every tip
  of the bucket. The MS-WH-SP-RG uses a reed switch that closes to GND,
  so we trigger on the FALLING edge (HIGH->LOW).

  A software debounce (DEBOUNCE_MS) filters out the mechanical contact
  bounce of the reed switch. No delay() is used here because blocking
  calls are not allowed inside an ISR.

  IRAM_ATTR places the routine in IRAM so it runs reliably on ESP8266.
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
  Initialize / (re)establish the WiFi connection.

  Non-blocking with a timeout: if the connection is not up within
  WIFI_CONNECT_TIMEOUT_MS the function returns false and the caller can
  simply try again later instead of hanging forever.

  @return true if connected
*/
boolean connect_to_wifi()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    return true;
  }

  Serial.print("Connecting to Wifi [");
  Serial.print(WIFI_SSID);
  Serial.print("] network .");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PWD);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED)
  {
    if (millis() - start > WIFI_CONNECT_TIMEOUT_MS)
    {
      Serial.println(" timeout");
      return false;
    }
    delay(500);
    Serial.print(".");
  }

  Serial.println(" connected");
  return true;
}

/*
  Fill a Home Assistant "device" object so that both entities are
  grouped under one device. identifiers must be a JSON array.
*/
void add_device_info(JsonObject device)
{
  JsonArray ids = device.createNestedArray("ids"); // identifiers
  ids.add(MQTT_INSTANCE_NAME);
  device["mf"] = "JH SOFT Technology"; // manufacturer
  device["mdl"] = MODEL;               // model
  device["name"] = "Rain sensor";
  device["sw"] = SW_VERSION; // software version
}

/*
  Build the MQTT discovery config for the rain sensor.

  @return DynamicJsonDocument with the discovery payload
*/
DynamicJsonDocument define_config_rain_sensor_to_ha_device()
{
  DynamicJsonDocument config(1024);

  config["~"] = "homeassistant/sensor/" TOPIC_RAIN_SENSOR_UNIQUE_ID;
  config["name"] = TOPIC_RAIN_SENSOR_NAME;
  config["uniq_id"] = TOPIC_RAIN_SENSOR_UNIQUE_ID;
  config["stat_t"] = "~/state";
  config["unit_of_meas"] = "mm";
  config["dev_cla"] = "precipitation";     // device_class
  config["stat_cla"] = "total_increasing"; // state_class: HA auto-tracks resets
  config["icon"] = "mdi:weather-rainy";
  config["avty_t"] = TOPIC_AVAILABILITY; // availability_topic
  config["pl_avail"] = PAYLOAD_ONLINE;
  config["pl_not_avail"] = PAYLOAD_OFFLINE;

  add_device_info(config.createNestedObject("dev"));

  return config;
}

/*
  (Re)connect to the MQTT broker, non-blocking.

  Registers a Last Will & Testament so the broker automatically marks
  the device "offline" if it drops off the network. On a successful
  connect it (re)publishes "online" to the availability topic.

  Only one connection attempt is made per call and attempts are rate
  limited by MQTT_RECONNECT_INTERVAL_MS, so loop() never blocks.

  @return true if connected
*/
boolean mqtt_reconnect()
{
  if (mqtt.connected())
  {
    return true;
  }

  if (millis() - last_mqtt_attempt < MQTT_RECONNECT_INTERVAL_MS)
  {
    return false; // wait before trying again
  }
  last_mqtt_attempt = millis();

  Serial.print("Connecting to mqtt broker ...");

  boolean connected;
  if (strlen(MQTT_USER_NAME) > 0 && strlen(MQTT_PASSWORD) > 0)
  {
    connected = mqtt.connect(MQTT_INSTANCE_NAME, MQTT_USER_NAME, MQTT_PASSWORD,
                             TOPIC_AVAILABILITY, 0, true, PAYLOAD_OFFLINE);
  }
  else
  {
    connected = mqtt.connect(MQTT_INSTANCE_NAME, NULL, NULL,
                             TOPIC_AVAILABILITY, 0, true, PAYLOAD_OFFLINE);
  }

  if (connected)
  {
    Serial.println(" connected");
    // mark the device available and (re)send discovery so HA picks it
    // up again after a broker restart.
    mqtt.publish(TOPIC_AVAILABILITY, PAYLOAD_ONLINE, true);
    return true;
  }

  Serial.print(" failed, rc=");
  Serial.println(mqtt.state());
  return false;
}

/*
  Publish a payload to a topic. Ensures the broker connection is up
  first; if it cannot be established the publish is skipped (no block).

  @return true if published
*/
boolean publish(const char *topic, const char *payload, boolean retain)
{
  if (!mqtt_reconnect())
  {
    return false;
  }
  return mqtt.publish(topic, payload, retain);
}

/*
  Serialize a discovery config document and publish it (retained).

  @return true if published
*/
boolean send_config_topic(DynamicJsonDocument payload, const char *topic)
{
  char buffer[MQTT_MAX_TRANSFER_SIZE];
  size_t n = serializeJson(payload, buffer, sizeof(buffer));
  if (n == 0)
  {
    Serial.println("Config serialization failed (buffer too small)");
    return false;
  }
  return publish(topic, buffer, true);
}

/*
  Publish a floating point state value (retained).

  @param state - measured value
  @param topic - destination topic
  @return true if published
*/
boolean send_state_topic(float state, const char *topic)
{
  char buffer[16]; // enough for "%.2f" of any realistic value
  snprintf(buffer, sizeof(buffer), "%.2f", state);
  return publish(topic, buffer, true);
}

/*
  Read the current tip count without resetting it (atomic).

  @return number of tips accumulated so far
*/
unsigned int peek_tips()
{
  noInterrupts();
  unsigned int count = tipping_count;
  interrupts();
  return count;
}

/*
  Atomically subtract the tips that were already reported. We subtract
  (instead of zeroing) so that tips arriving while the value was being
  published are preserved for the next interval.

  @param consumed - number of tips that were successfully sent
*/
void consume_tips(unsigned int consumed)
{
  noInterrupts();
  tipping_count -= consumed;
  interrupts();
}

/*
  Setup
*/
void setup()
{
  Serial.begin(115200);

  // Reed switch closes to GND -> use internal pull-up and trigger on
  // the FALLING edge. Using an interrupt guarantees no tip is missed,
  // even during heavy rain when loop() is busy with other work.
  pinMode(RAIN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(RAIN_PIN), count_tipping, FALLING);

  delay(1000);
  connect_to_wifi();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  // increase buffer size so the discovery payload fits
  mqtt.setBufferSize(MQTT_MAX_TRANSFER_SIZE);

  // connect and send discovery config
  if (mqtt_reconnect())
  {
    if (send_config_topic(define_config_rain_sensor_to_ha_device(), TOPIC_RAIN_SENSOR_CONFIG))
    {
      Serial.println("Config for rain sensor sent successfully");
    }
  }

  // force a send on the first loop iteration
  last_send_rain = millis() - RAIN_ACTIVE_INTERVAL_S * 1000UL;
}

/*
  Main loop
*/
void loop()
{
  // keep connections alive (non-blocking)
  if (!connect_to_wifi())
  {
    return; // no WiFi yet, try again next iteration
  }
  mqtt_reconnect();
  mqtt.loop();

  // Accumulate tips into the running total immediately so they are
  // never lost, even if a later MQTT publish fails.
  unsigned int tips = peek_tips();
  if (tips > 0)
  {
    consume_tips(tips);
    total_rain_mm += tips * MM_PER_TIP;
  }

  // Adaptive interval: frequent updates while it is raining give
  // high-resolution HA graphs; infrequent updates when dry save
  // bandwidth and broker resources.
  unsigned long interval_ms = (millis() - last_tip_time < NO_RAIN_TIMEOUT_MS)
                                  ? RAIN_ACTIVE_INTERVAL_S * 1000UL
                                  : RAIN_IDLE_INTERVAL_S * 1000UL;

  if (millis() - last_send_rain > interval_ms)
  {
    if (send_state_topic(total_rain_mm, TOPIC_RAIN_SENSOR_STATE))
    {
      last_send_rain = millis();
    }
  }
}
