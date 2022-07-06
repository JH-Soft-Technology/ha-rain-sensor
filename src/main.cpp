/*
  Rain sensor for Home assistant based on mqtt discovery.

  Will automatically create device with sensor, only when
  Mqtt discovery integration is configured

  author: Jiri Horalek
  email: horalek.jiri@gmail.com
  site: https://github.com/JH-Soft-Technology/ha-rain-sensor

*/
#include <Arduino.h>
#include <PubSubClient.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>

#define MODEL "rainy 0.0.1"
#define SW_VERSION "0.0.1"

#define WIFI_SSID "ufo"
#define WIFI_PWD "Porsche356"

#define MQTT_HOST "192.168.2.5"   // change to your mqtt borer host
#define MQTT_PORT 2883            // change to your mqtt broker port
#define MQTT_USER_NAME "ufo"      // change to your mqtt user name when you have user name set up
#define MQTT_PASSWORD "ufoufoufo" // change to your mqtt password when you have pwd set up
#define MQTT_SEND_INTERVAL 10     // in seconds

#define TOPIC_SENSOR_UNIQUE_ID "rain_sensor" // change to your needs will be unique id of the device
#define TOPIC_SENSOR_NAME "Rain sensor"
#define TOPIC_CONFIG "homeassistant/sensor/" TOPIC_SENSOR_UNIQUE_ID "/config"

const byte RAIN_PIN = 14; // D5

WiFiClient wifi;
PubSubClient mqtt(wifi); // define mqtt client for publishing

int wifi_status = WL_IDLE_STATUS;

unsigned int drip_count = 0; // count bucket woter dripping
unsigned long last_send;

/*
  Increment drip from the bucket
*/
void count_dripping()
{
  drip_count++;
}

/*
  Initialization WiFi connection
*/
void connect_to_wifi()
{
  Serial.print("Connecting to Wifi [");
  Serial.print(WIFI_SSID);
  Serial.println("] network ...");

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

  Serial.print("Connected to Wifi [");
  Serial.print(WIFI_SSID);
  Serial.println("]");
}

/*
  This method define json object which is tailor-made
  for home assistant mqtt discovery.

  Will automatically create device with sensor to
  measure rain

  @return String with JSON format data to define device in HA
*/
DynamicJsonDocument define_config_ha_device()
{
  DynamicJsonDocument config(1024);
  DynamicJsonDocument device(1024);

  device["identifiers"] = "[" + String(TOPIC_SENSOR_UNIQUE_ID) + "]";
  device["manufacturer"] = "JH SOFT Technology";
  device["model"] = MODEL;
  device["name"] = TOPIC_SENSOR_UNIQUE_ID;
  device["sw_version"] = SW_VERSION;

  config["~"] = "homeassistant/sensor/" + String(TOPIC_SENSOR_UNIQUE_ID);
  config["name"] = TOPIC_SENSOR_NAME;
  config["unique_id"] = TOPIC_SENSOR_UNIQUE_ID;
  config["stat_t"] = "~/state";
  config["schema"] = "json";
  config["unit_of_meas"] = "mm";
  config["dev"] = device;

  // String config = "{";
  // // entity definition
  // config += "\"~\": \"homeassistant/sensor/\"" TOPIC_SENSOR_UNIQUE_ID ",";
  // config += "\"name\": " TOPIC_SENSOR_NAME ",";
  // config += "\"unique_id\": " TOPIC_SENSOR_UNIQUE_ID ",";
  // config += "\"stat_t\": \"~/state\",";
  // config += "\"schema\": \"json\",";
  // config += "\"unit_of_meas\": \"mm\",";
  // // end of entity definition
  // // device definition
  // config += "\"dev\": {";
  // config += "\"identifiers\": [" TOPIC_SENSOR_UNIQUE_ID "],";
  // config += "\"manufacturer\" : \"JH Soft Technology\",";
  // config += "\"model\": " MODEL ",";
  // config += "\"name\": " TOPIC_SENSOR_UNIQUE_ID ",";
  // config += "\"sw_version\": " SW_VERSION ",";
  // config += "}";
  // // end of device definition
  // config += "}";

  return config;
}

/*
  Will publish data into specific topic

  @param topic - specific address where will be placed data in mqtt broker
  @param payload - data in JSON format
*/
void send_topic(String topic, String payload)
{
}

/*
  Setup app
*/
void setup()
{
  Serial.begin(115200);

  // pinMode(RAIN_PIN, INPUT_PULLUP);
  // // need to attache pin interruption
  // attachInterrupt(digitalPinToInterrupt(RAIN_PIN), count_dripping, RISING);

  delay(10); // wait for a while

  String config = define_config_ha_device();
  connect_to_wifi();

  send_topic(TOPIC_CONFIG, config);
}

/*
  Here is running program repeatedly
*/
void loop()
{
}