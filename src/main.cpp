/*
  Rain sensor for Home assistant based on mqtt discovery.

  Will automatically create device with sensor, only when
  Mqtt discovery integration is configured

  author: Jiri Horalek
  email: horalek.jiri@gmail.com
  site: https://github.com/JH-Soft-Technology/ha-rain-sensor
  version: 0.0.1
  last change: 10.07.2022
*/
#include <Arduino.h>
#include <PubSubClient.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>

#define MODEL "rainy 0.0.2"
#define SW_VERSION "0.0.3"

#define WIFI_SSID "{SSID}"
#define WIFI_PWD "{PASSWORD}"

#define MQTT_MAX_TRANSFER_SIZE 512
#define MQTT_INSTANCE_NAME "ha-rain-distance-sensor"
#define MQTT_HOST "{HOST IP}" // change to your mqtt borer host
#define MQTT_PORT 1883        // change to your mqtt broker port
// if you do not have set up user and pwd, just leave blank
#define MQTT_USER_NAME "{MQTT USER NAME}" // change to your mqtt user name when you have user name set up
#define MQTT_PASSWORD "{MQTT PASSWORD}"   // change to your mqtt password when you have pwd set up
#define MQTT_RAIN_SEND_INTERVAL 600       // in seconds 600=10 min.
#define MQTT_DISTANCE_SEND_INTERVAL 60    // 1 min. interval

#define TOPIC_PREFIX "homeassistant/sensor"

#define TOPIC_RAIN_SENSOR_UNIQUE_ID "rain_sensor" // change to your needs will be unique i8d of the device
#define TOPIC_RAIN_SENSOR_NAME "Rain sensor"
#define TOPIC_RAIN_SENSOR_CONFIG TOPIC_PREFIX "/" TOPIC_RAIN_SENSOR_UNIQUE_ID "/config"
#define TOPIC_RAIN_SENSOR_STATE TOPIC_PREFIX "/" TOPIC_RAIN_SENSOR_UNIQUE_ID "/state"

#define TOPIC_DISTANCE_SENSOR_UNIQUE_ID "distance_sensor" // change to your needs will be uniqeu id of the device
#define TOPIC_DISTANCE_SENSOR_NAME "Distance sensor"
#define TOPIC_DISTANCE_SENSOR_CONFIG TOPIC_PREFIX "/" TOPIC_DISTANCE_SENSOR_UNIQUE_ID "/config"
#define TOPIC_DISTANCE_SENSOR_STATE TOPIC_PREFIX "/" TOPIC_DISTANCE_SENSOR_UNIQUE_ID "/state"

// very important. If you are using Wemos D1 mini, then there is no
// interrupting supported. You need to use D1 PIN. It is verified
const byte RAIN_PIN = D1;

// distance sensor pin setup
const byte ECHO_PIN = D7;
const byte TRIG_PIN = D6;

WiFiClient wifi;
PubSubClient mqtt(wifi); // define mqtt client for publishing

int wifi_status = WL_IDLE_STATUS;

unsigned int tipping_count = 0; // count bucket woter tipping
unsigned long last_send_rain, last_send_distance;

long duration, distance; // Duration used to calcualte distance

/*
  Initialization WiFi connection
*/
void connect_to_wifi()
{
  Serial.print("Connecting to Wifi [");
  Serial.print(WIFI_SSID);
  Serial.print("] network .");

  WiFi.begin(WIFI_SSID, WIFI_PWD);
  WiFi.mode(WIFI_STA); // wifi mode STA 1

  while (WiFi.status() != WL_CONNECTED)
  {
    // waiting for connection
    delay(500);
    Serial.print(".");
  }

  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  wifi_status = WiFi.status();

  Serial.println(" connected");
}

/*
  This method define json object which is tailor-made
  for home assistant mqtt discovery.

  Will automatically create device with sensor to
  measure rain

  @return String with JSON format data to define device in HA
*/
DynamicJsonDocument define_config_rain_sensor_to_ha_device()
{
  DynamicJsonDocument config(1024);
  DynamicJsonDocument device(1024);

  device["ids"] = "[" + String(MQTT_INSTANCE_NAME) + "]";
  device["mf"] = "JH SOFT Technology";
  device["mdl"] = MODEL;
  device["name"] = TOPIC_RAIN_SENSOR_UNIQUE_ID;
  device["sw"] = SW_VERSION;

  config["~"] = "homeassistant/sensor/" + String(TOPIC_RAIN_SENSOR_UNIQUE_ID);
  config["name"] = TOPIC_RAIN_SENSOR_NAME;
  config["uniq_id"] = TOPIC_RAIN_SENSOR_UNIQUE_ID;
  config["stat_t"] = "~/state";
  config["schema"] = "json";
  config["unit_of_meas"] = "mm";
  config["icon"] = "mdi:weather-rainy";
  config["pl_avail"] = "online";      // payload_available
  config["pl_not_avail"] = "offline"; // payload_not_available
  config["dev"] = device;

  return config;
}

/**
 * @brief Definition of distance sensor in HA
 *
 * @return DynamicJsonDocument  JSON format string
 */
DynamicJsonDocument define_config_distance_sensor_to_ha_device()
{
  DynamicJsonDocument config(1024);
  DynamicJsonDocument device(1024);

  device["identifiers"] = "[" + String(MQTT_INSTANCE_NAME) + "]";
  device["mf"] = "JH SOFT Technology";
  device["mdl"] = MODEL;
  device["name"] = TOPIC_DISTANCE_SENSOR_UNIQUE_ID;
  device["sw"] = SW_VERSION;

  config["~"] = "homeassistant/sensor/" + String(TOPIC_DISTANCE_SENSOR_UNIQUE_ID);
  config["name"] = TOPIC_DISTANCE_SENSOR_NAME;
  config["uniq_id"] = TOPIC_DISTANCE_SENSOR_UNIQUE_ID;
  config["stat_t"] = "~/state";
  config["schema"] = "json";
  config["unit_of_meas"] = "cm";
  config["icon"] = "mdi:car";
  config["pl_avail"] = "online";      // payload_available
  config["pl_not_avail"] = "offline"; // payload_not_available
  config["dev"] = device;

  return config;
}

/*
  Will send topic with payload into the broker

  @param payload - message (data)
  @param topic - it is address where data will be stored
*/
boolean send(char *payload, char *topic, boolean retain)
{
  if (mqtt.connected())
  {
    Serial.println("Already connected into the mqtt broker.");
    return mqtt.publish(topic, payload, retain);
  }
  else
  {
    Serial.print("Connecting to mqtt broker .");
  }

  while (!mqtt.connected())
  {
    Serial.print(".");

    boolean connected = false;

    if (strlen(MQTT_USER_NAME) > 0 && strlen(MQTT_PASSWORD) > 0)
    {
      connected = mqtt.connect(MQTT_INSTANCE_NAME, MQTT_USER_NAME, MQTT_PASSWORD);
    }
    else
    {
      connected = mqtt.connect(MQTT_INSTANCE_NAME);
    }

    if (connected)
    {
      Serial.println(". connected");
      return mqtt.publish(topic, payload, retain);
    }
  }

  return false;
}

/*
  Will publish data into specific topic

  @param topic - specific address where will be placed data in mqtt broker
  @param payload - data in JSON format
*/
boolean send_config_topic(DynamicJsonDocument payload, String topic)
{
  char c_buffer[MQTT_MAX_TRANSFER_SIZE];
  serializeJson(payload, c_buffer);

  char c_topic[topic.length() + 1];
  strcpy(c_topic, topic.c_str());

  return send(c_buffer, c_topic, true);
}

/*
  Will publish state data from rain sensor into the HA device

  @param state - is a float number of watter tipped in mm
  @param topic - is a topic where should be data sent
 */
boolean send_state_topic(float state, String topic)
{
  char s_buffer[sizeof(state)];
  sprintf(s_buffer, "%.2f", state);

  char c_topic[topic.length() + 1];
  strcpy(c_topic, topic.c_str());

  return send(s_buffer, c_topic, true);
}

/*
  Increment tipping from the bucket
*/
void count_tipping()
{
  if (digitalRead(RAIN_PIN) == HIGH)
  {
    tipping_count++;

    Serial.print("tipping: ");
    Serial.println(tipping_count);
    delay(500);
  }
}

/*
  Will calculate watter tipping in mm

  @return float number of mm
*/
float calculate_rain()
{
  float r = (tipping_count ) * 0.2794;
  tipping_count = 0;

  return r;
}

/**
 * @brief Will meausre distance between sensor and objects
 *
 * @return int Value is in cm
 */
long measure_distance()
{
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  duration = pulseIn(ECHO_PIN, HIGH);
  // Calculate  the distance in cm based on the speed of sound.
  distance = duration / 58.2;

  Serial.print("Distance: ");
  Serial.print(distance);
  Serial.println(" cm");

  return distance;
}

/*
  Setup app
*/
void setup()
{
  Serial.begin(115200);

  pinMode(RAIN_PIN, INPUT);

  pinMode(ECHO_PIN, INPUT);  // comment if you dont have distance sensor
  pinMode(TRIG_PIN, OUTPUT); // comment if you dont have distance sensor

  delay(1000);
  connect_to_wifi();

  // setup mqtt broker
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  // neccessary to encrese buffer size for pyload messages
  mqtt.setBufferSize(MQTT_MAX_TRANSFER_SIZE);
  // send config topic for rain sensor
  if (send_config_topic(define_config_rain_sensor_to_ha_device(), TOPIC_RAIN_SENSOR_CONFIG))
  {
    Serial.print("Config for ");
    Serial.print(TOPIC_RAIN_SENSOR_CONFIG);
    Serial.println(" hs been send successffully");
  }
  // comment if you dont have distance sensor
  // send config topic for distance sensor
  if (send_config_topic(define_config_distance_sensor_to_ha_device(), TOPIC_DISTANCE_SENSOR_CONFIG))
  {
    Serial.print("Config for ");
    Serial.print(TOPIC_DISTANCE_SENSOR_CONFIG);
    Serial.println(" hs been send successffully");
  }

  last_send_rain = millis() - MQTT_RAIN_SEND_INTERVAL * 1000;
  last_send_distance = millis() - MQTT_DISTANCE_SEND_INTERVAL * 1000; // comment if you dont have distance sensor
}

/*
  Here is running program repeatedly
*/
void loop()
{
  measure_distance();
  delay(50); // need to delay because of the calculation

  count_tipping();

  if (WiFi.status() != WL_CONNECTED)
  {
    WiFi.begin(WIFI_SSID, WIFI_PWD);
    while (WiFi.status() != WL_CONNECTED)
    {
      delay(500);
    }
  }
  // comment if you dont have distance sensor
  if (millis() - last_send_distance > MQTT_DISTANCE_SEND_INTERVAL * 1000)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      if (send_state_topic(measure_distance(), TOPIC_DISTANCE_SENSOR_STATE))
      {
        last_send_distance = millis();
      }
    }
  }

  if (millis() - last_send_rain > MQTT_RAIN_SEND_INTERVAL * 1000)
  {
    if (WiFi.status() == WL_CONNECTED)
    {
      if (send_state_topic(calculate_rain(), TOPIC_RAIN_SENSOR_STATE))
      {
        last_send_rain = millis();
      }
    }
  }
}
