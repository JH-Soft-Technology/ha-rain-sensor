/*
  Rain sensor for Home Assistant based on MQTT discovery.

  Features:
    - WiFi + MQTT configuration via a captive portal (WiFiManager),
      no credentials are stored in source code.
    - Web UI with live status, precipitation period totals, firmware
      update (ElegantOTA) and a "reset WiFi" button.
    - NTP time sync (CET/CEST) for daily/weekly/monthly/yearly reset.
    - Period stats (today/week/month/year) persisted to LittleFS.
    - One JSON state message exposed in HA as several entities:
      cumulative rain (mm), rain rate (mm/h), raining (binary),
      today/week/month/year rain, WiFi RSSI, uptime and free heap.

  author: Jiri Horalek
  email: horalek.jiri@gmail.com
  site: https://github.com/JH-Soft-Technology/ha-rain-sensor
  version: 0.7.0
  last change: 05.06.2026
*/
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <ElegantOTA.h>
#include <time.h>

#define MODEL "rainy 0.0.2"
#define SW_VERSION "0.7.0"

#define MQTT_MAX_TRANSFER_SIZE 1024
#define MQTT_INSTANCE_NAME "ha-rain-sensor"

#define AP_NAME "RainSensor-Setup"

#define RAIN_ACTIVE_INTERVAL_S 60
#define RAIN_IDLE_INTERVAL_S 1800

// Adaptive "raining" detection. A tipping bucket is quantized (one tip =
// 0.2794 mm), so in light rain the gap between tips can be tens of minutes
// and a fixed window flickers to "not raining" between tips. Instead we
// wait ~FACTOR x the last gap between tips, clamped to [MIN, MAX]:
// heavy rain -> short timeout (turns off quickly after it stops),
// drizzle    -> long timeout  (does not flicker during long gaps).
#define RAIN_TIMEOUT_FACTOR 2
#define RAIN_MIN_TIMEOUT_MS 600000UL   // 10 min
#define RAIN_MAX_TIMEOUT_MS 3600000UL  // 60 min

#define MQTT_RECONNECT_INTERVAL_MS 5000
#define DEBOUNCE_MS 150

#define NTP_SERVER "pool.ntp.org"
#define TZ_INFO "CET-1CEST,M3.5.0,M10.5.0/3"
#define STATS_SAVE_MS 300000UL // persist period stats every 5 min

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
#define CFG_TODAY "homeassistant/sensor/" MQTT_INSTANCE_NAME "/today/config"
#define CFG_WEEK "homeassistant/sensor/" MQTT_INSTANCE_NAME "/week/config"
#define CFG_MONTH "homeassistant/sensor/" MQTT_INSTANCE_NAME "/month/config"
#define CFG_YEAR "homeassistant/sensor/" MQTT_INSTANCE_NAME "/year/config"

// On the Wemos D1 mini (ESP8266) all GPIO pins except D0 (GPIO16)
// support hardware interrupts, so D1 works fine with attachInterrupt.
const byte RAIN_PIN = D1;
const float MM_PER_TIP = 0.2794;

// ---- persisted MQTT configuration ----
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

volatile unsigned int tipping_count = 0;
volatile unsigned long last_tip_time = 0;
volatile unsigned long last_tip_interval = 0; // gap (ms) between the last two tips

float total_rain_mm = 0.0;
float period_rain_mm = 0.0;
float last_rate = 0.0;

// Period statistics — reset daily/weekly/monthly/yearly via NTP time
float rain_today_mm = 0.0;
float rain_week_mm = 0.0;
float rain_month_mm = 0.0;
float rain_year_mm = 0.0;

// Reference timestamps stored alongside period stats in LittleFS
int ref_yday = -1;  // tm_yday at last save (0–365)
int ref_week = -1;  // ISO week number at last save
int ref_month = -1; // tm_mon at last save (0–11)
int ref_year = -1;  // tm_year at last save (years since 1900)

// Per-bucket history arrays for in-tile sparkline charts
float rain_hourly[24] = {};      // rain per hour of today     (index = tm_hour)
float rain_daily_week[7] = {};   // rain per weekday Mon–Sun   (index = (wday+6)%7)
float rain_daily_month[31] = {}; // rain per day of month      (index = tm_mday-1)
float rain_monthly[12] = {};     // rain per month of year     (index = tm_mon)

int hist_hour = -1; // current accumulation bucket indices (updated each minute)
int hist_wday = -1;
int hist_mday = -1;
int hist_mon = -1;

unsigned long last_send_rain = 0;
unsigned long last_mqtt_attempt = 0;
unsigned long last_stats_save = 0;
unsigned long last_rollover_chk = 0;

// ---- ISR + tip helpers ----

IRAM_ATTR void count_tipping()
{
  unsigned long now = millis();
  if (now - last_tip_time > DEBOUNCE_MS)
  {
    if (last_tip_time != 0) last_tip_interval = now - last_tip_time;
    tipping_count++;
    last_tip_time = now;
  }
}

unsigned int peek_tips()
{
  noInterrupts();
  unsigned int c = tipping_count;
  interrupts();
  return c;
}

void consume_tips(unsigned int n)
{
  noInterrupts();
  tipping_count -= n;
  interrupts();
}

// How long without a tip until we decide it stopped raining. Scales with
// the last gap between tips so light rain doesn't flicker, clamped so heavy
// rain turns off promptly and a lone tip doesn't latch "raining" forever.
unsigned long rain_timeout_ms()
{
  unsigned long t = (unsigned long)RAIN_TIMEOUT_FACTOR * last_tip_interval;
  if (t < RAIN_MIN_TIMEOUT_MS) t = RAIN_MIN_TIMEOUT_MS;
  if (t > RAIN_MAX_TIMEOUT_MS) t = RAIN_MAX_TIMEOUT_MS;
  return t;
}

boolean raining_now()
{
  if (last_tip_time == 0) return false; // no tip ever -> not raining
  return (millis() - last_tip_time) < rain_timeout_ms();
}

// ---- Time helpers ----

boolean get_localtime(struct tm &ti)
{
  time_t now = time(nullptr);
  if (now < 1000000000UL)
    return false;
  localtime_r(&now, &ti);
  return true;
}

// Monday-based ISO week number (strftime %W).
int week_num(const struct tm &ti)
{
  char buf[4];
  strftime(buf, sizeof(buf), "%W", &ti);
  return atoi(buf);
}

void sync_ntp()
{
  configTime(0, 0, NTP_SERVER);
  setenv("TZ", TZ_INFO, 1);
  tzset();
  Serial.print("Waiting for NTP ...");
  time_t now = 0;
  for (int i = 0; i < 50 && now < 1000000000UL; i++)
  {
    delay(100);
    time(&now);
  }
  Serial.println(now > 1000000000UL ? " OK" : " failed");
}

// ---- Period-stats persistence (LittleFS /stats.json) ----

void load_stats()
{
  if (!LittleFS.exists("/stats.json"))
    return;
  File f = LittleFS.open("/stats.json", "r");
  if (!f)
    return;
  DynamicJsonDocument doc(400);
  if (deserializeJson(doc, f))
  {
    f.close();
    return;
  }
  f.close();
  total_rain_mm = doc["total"] | 0.0f;
  rain_today_mm = doc["today"] | 0.0f;
  rain_week_mm = doc["week"] | 0.0f;
  rain_month_mm = doc["month"] | 0.0f;
  rain_year_mm = doc["yr"] | 0.0f;
  ref_yday = doc["yday"] | -1;
  ref_week = doc["wk"] | -1;
  ref_month = doc["mon"] | -1;
  ref_year = doc["year"] | -1;
}

void save_stats()
{
  DynamicJsonDocument doc(400);
  doc["total"] = total_rain_mm;
  doc["today"] = rain_today_mm;
  doc["week"] = rain_week_mm;
  doc["month"] = rain_month_mm;
  doc["yr"] = rain_year_mm;
  doc["yday"] = ref_yday;
  doc["wk"] = ref_week;
  doc["mon"] = ref_month;
  doc["year"] = ref_year;
  File f = LittleFS.open("/stats.json", "w");
  if (!f)
    return;
  serializeJson(doc, f);
  f.close();
}

void save_history(); // defined below, forward-declared for check_rollover

// Reset period accumulators that have rolled over since the last save.
void check_rollover()
{
  struct tm ti;
  if (!get_localtime(ti))
    return;

  int cur_yday = ti.tm_yday;
  int cur_week = week_num(ti);
  int cur_month = ti.tm_mon;
  int cur_year = ti.tm_year;

  // Always keep current-bucket indices up to date
  hist_hour = ti.tm_hour;
  hist_wday = (ti.tm_wday + 6) % 7; // Mon=0 … Sun=6
  hist_mday = ti.tm_mday - 1;
  hist_mon = ti.tm_mon;

  if (ref_year == -1)
  {
    ref_yday = cur_yday;
    ref_week = cur_week;
    ref_month = cur_month;
    ref_year = cur_year;
    save_stats();
    return;
  }

  bool changed = false;

  if (cur_year != ref_year)
  {
    rain_year_mm = rain_month_mm = rain_week_mm = rain_today_mm = 0;
    memset(rain_monthly, 0, sizeof(rain_monthly));
    memset(rain_daily_month, 0, sizeof(rain_daily_month));
    memset(rain_daily_week, 0, sizeof(rain_daily_week));
    memset(rain_hourly, 0, sizeof(rain_hourly));
    ref_year = cur_year;
    ref_month = cur_month;
    ref_week = cur_week;
    ref_yday = cur_yday;
    changed = true;
  }
  else if (cur_month != ref_month)
  {
    rain_month_mm = rain_week_mm = rain_today_mm = 0;
    memset(rain_daily_month, 0, sizeof(rain_daily_month));
    memset(rain_daily_week, 0, sizeof(rain_daily_week));
    memset(rain_hourly, 0, sizeof(rain_hourly));
    ref_month = cur_month;
    ref_week = cur_week;
    ref_yday = cur_yday;
    changed = true;
  }
  else if (cur_week != ref_week)
  {
    rain_week_mm = rain_today_mm = 0;
    memset(rain_daily_week, 0, sizeof(rain_daily_week));
    memset(rain_hourly, 0, sizeof(rain_hourly));
    ref_week = cur_week;
    ref_yday = cur_yday;
    changed = true;
  }
  else if (cur_yday != ref_yday)
  {
    rain_today_mm = 0;
    memset(rain_hourly, 0, sizeof(rain_hourly));
    ref_yday = cur_yday;
    changed = true;
  }

  if (changed)
  {
    save_stats();
    save_history();
  }
}

// ---- MQTT configuration persistence (LittleFS /config.json) ----

boolean load_config()
{
  if (!LittleFS.exists("/config.json"))
    return false;
  File f = LittleFS.open("/config.json", "r");
  if (!f)
    return false;
  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err)
    return false;
  strlcpy(config.host, doc["host"] | "", sizeof(config.host));
  strlcpy(config.port, doc["port"] | "1883", sizeof(config.port));
  strlcpy(config.user, doc["user"] | "", sizeof(config.user));
  strlcpy(config.pass, doc["pass"] | "", sizeof(config.pass));
  return true;
}

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

void load_history()
{
  if (!LittleFS.exists("/history.json"))
    return;
  File f = LittleFS.open("/history.json", "r");
  if (!f)
    return;
  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, f))
  {
    f.close();
    return;
  }
  f.close();
  JsonArray ah = doc["h"].as<JsonArray>();
  JsonArray aw = doc["w"].as<JsonArray>();
  JsonArray am = doc["m"].as<JsonArray>();
  JsonArray ay = doc["y"].as<JsonArray>();
  for (int i = 0; i < (int)ah.size() && i < 24; i++)
    rain_hourly[i] = ah[i] | 0.0f;
  for (int i = 0; i < (int)aw.size() && i < 7; i++)
    rain_daily_week[i] = aw[i] | 0.0f;
  for (int i = 0; i < (int)am.size() && i < 31; i++)
    rain_daily_month[i] = am[i] | 0.0f;
  for (int i = 0; i < (int)ay.size() && i < 12; i++)
    rain_monthly[i] = ay[i] | 0.0f;
}

void save_history()
{
  DynamicJsonDocument doc(1024);
  JsonArray ah = doc.createNestedArray("h");
  JsonArray aw = doc.createNestedArray("w");
  JsonArray am = doc.createNestedArray("m");
  JsonArray ay = doc.createNestedArray("y");
  for (int i = 0; i < 24; i++)
    ah.add(round(rain_hourly[i] * 100) / 100.0f);
  for (int i = 0; i < 7; i++)
    aw.add(round(rain_daily_week[i] * 100) / 100.0f);
  for (int i = 0; i < 31; i++)
    am.add(round(rain_daily_month[i] * 100) / 100.0f);
  for (int i = 0; i < 12; i++)
    ay.add(round(rain_monthly[i] * 100) / 100.0f);
  File f = LittleFS.open("/history.json", "w");
  if (!f)
    return;
  serializeJson(doc, f);
  f.close();
}

void save_config_callback() { shouldSaveConfig = true; }

// ---- Home Assistant MQTT discovery ----

void add_device_info(JsonObject device)
{
  JsonArray ids = device.createNestedArray("ids");
  ids.add(MQTT_INSTANCE_NAME);
  device["mf"] = "JH SOFT Technology";
  device["mdl"] = MODEL;
  device["name"] = "Rain sensor";
  device["sw"] = SW_VERSION;
}

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

// ---- MQTT ----

boolean mqtt_reconnect()
{
  if (mqtt.connected())
    return true;
  if (millis() - last_mqtt_attempt < MQTT_RECONNECT_INTERVAL_MS)
    return false;
  last_mqtt_attempt = millis();
  Serial.print("Connecting to mqtt broker ...");
  boolean connected;
  if (strlen(config.user) > 0)
    connected = mqtt.connect(MQTT_INSTANCE_NAME, config.user, config.pass,
                             TOPIC_AVAILABILITY, 0, true, PAYLOAD_OFFLINE);
  else
    connected = mqtt.connect(MQTT_INSTANCE_NAME, NULL, NULL,
                             TOPIC_AVAILABILITY, 0, true, PAYLOAD_OFFLINE);
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

boolean publish_config(DynamicJsonDocument doc, const char *topic)
{
  char buffer[MQTT_MAX_TRANSFER_SIZE];
  size_t n = serializeJson(doc, buffer, sizeof(buffer));
  if (!n)
  {
    Serial.println("Config serialization failed (buffer too small)");
    return false;
  }
  return mqtt.publish(topic, buffer, true);
}

void publish_discovery()
{
  publish_config(build_sensor_config(
                     "Rain", "rain_sensor",
                     "{{ value_json.rain }}", "mm",
                     "precipitation", "total_increasing", "mdi:weather-rainy", false),
                 CFG_RAIN);

  publish_config(build_sensor_config(
                     "Rain rate", "rain_sensor_rate",
                     "{{ value_json.rate }}", "mm/h",
                     "precipitation_intensity", "measurement", "mdi:weather-pouring", false),
                 CFG_RATE);

  publish_config(build_sensor_config(
                     "Rain today", "rain_sensor_today",
                     "{{ value_json.today }}", "mm",
                     "precipitation", "measurement", "mdi:weather-rainy", false),
                 CFG_TODAY);

  publish_config(build_sensor_config(
                     "Rain this week", "rain_sensor_week",
                     "{{ value_json.week }}", "mm",
                     "precipitation", "measurement", "mdi:calendar-week", false),
                 CFG_WEEK);

  publish_config(build_sensor_config(
                     "Rain this month", "rain_sensor_month",
                     "{{ value_json.month }}", "mm",
                     "precipitation", "measurement", "mdi:calendar-month", false),
                 CFG_MONTH);

  publish_config(build_sensor_config(
                     "Rain this year", "rain_sensor_year",
                     "{{ value_json.year }}", "mm",
                     "precipitation", "measurement", "mdi:calendar", false),
                 CFG_YEAR);

  publish_config(build_sensor_config(
                     "WiFi signal", "rain_sensor_rssi",
                     "{{ value_json.rssi }}", "dBm",
                     "signal_strength", "measurement", NULL, true),
                 CFG_RSSI);

  publish_config(build_sensor_config(
                     "Uptime", "rain_sensor_uptime",
                     "{{ value_json.uptime }}", "s",
                     "duration", "measurement", NULL, true),
                 CFG_UPTIME);

  publish_config(build_sensor_config(
                     "Free memory", "rain_sensor_heap",
                     "{{ value_json.heap }}", "B",
                     NULL, "measurement", "mdi:memory", true),
                 CFG_HEAP);

  publish_config(build_raining_config(), CFG_RAINING);
}

boolean publish_state()
{
  if (!mqtt.connected())
    return false;
  DynamicJsonDocument doc(384);
  doc["rain"] = round(total_rain_mm * 100) / 100.0;
  doc["rate"] = round(last_rate * 10) / 10.0;
  doc["raining"] = raining_now();
  doc["rssi"] = WiFi.RSSI();
  doc["uptime"] = millis() / 1000UL;
  doc["heap"] = ESP.getFreeHeap();
  doc["today"] = round(rain_today_mm * 100) / 100.0;
  doc["week"] = round(rain_week_mm * 100) / 100.0;
  doc["month"] = round(rain_month_mm * 100) / 100.0;
  doc["year"] = round(rain_year_mm * 100) / 100.0;
  char buffer[384];
  serializeJson(doc, buffer, sizeof(buffer));
  return mqtt.publish(TOPIC_STATE, buffer, true);
}

// ---- Web UI ----

// Stream a single x-axis label for a sparkline.
void send_xlabel(int idx, int bw, int y, int hi, int xfs, const char *lbl)
{
  int cx = 1 + idx * (bw + 1) + bw / 2;
  char buf[110];
  snprintf(buf, sizeof(buf),
           "<text x='%d' y='%d' font-size='%d' fill='%s'"
           " font-weight='%s' text-anchor='middle'>%s</text>",
           cx, y, xfs,
           idx == hi ? "#3b82f6" : "#94a3b8",
           idx == hi ? "700" : "400",
           lbl);
  server.sendContent(buf);
}

// Sparkline bar chart embedded in a precipitation tile.
// data  : float array (mm per bucket)
// n     : number of buckets
// hi    : currently active bucket index (highlighted), -1 = none
// type  : 0=hourly(24), 1=weekly(7), 2=monthly(31), 3=yearly(12)
//
// Layout (viewBox 0 0 260 124):
//   TOP=24 px  — value labels above bars
//   BAREA=80px — bar drawing area  (max bar height 76 px)
//   BOT=20 px  — x-axis label area
//
// Dense charts (n>12): value labels rotated −90° along each bar.
// Sparse charts (n≤12): value labels horizontal above each bar.
void send_mini_chart(const float *data, int n, int hi, int type)
{
  const int W = 260;
  const int TOP = 24;
  const int BAREA = 80;
  const int BOT = 20;
  const int H = TOP + BAREA + BOT; // 124
  const int BASE = TOP + BAREA;    // 104
  const int MAXBH = BAREA - 4;     // 76

  bool dense = (n > 12);
  int bw = max(2, (W - 2) / n - 1);

  // Font sizes
  int vfs = dense ? 7 : min(13, max(8, bw * 42 / 100));
  int xfs = dense ? 6 : min(11, max(7, bw * 37 / 100));

  // Max value
  float mx = 0;
  for (int i = 0; i < n; i++)
    if (data[i] > mx)
      mx = data[i];

  // SVG open + background + baseline
  char hdr[240];
  snprintf(hdr, sizeof(hdr),
           "<svg viewBox='0 0 %d %d'"
           " style='width:100%%;margin-top:.6rem;display:block'>"
           "<rect width='%d' height='%d' fill='#eff6ff' rx='3'/>"
           "<line x1='1' y1='%d' x2='%d' y2='%d'"
           " stroke='#bfdbfe' stroke-width='0.8'/>",
           W, H, W, H, BASE, W - 1, BASE);
  server.sendContent(hdr);

  // Bars + value labels
  for (int i = 0; i < n; i++)
  {
    if (data[i] == 0)
      continue;
    bool hi_bar = (i == hi);
    int bh = max(2, (int)round(data[i] / mx * MAXBH));
    int bx = 1 + i * (bw + 1);
    int by = BASE - bh;
    int cx = bx + bw / 2;

    // Value string: 1 decimal if < 10, integer otherwise
    char vs[8];
    if (data[i] < 10.0f)
      snprintf(vs, sizeof(vs), "%.1f", data[i]);
    else
      snprintf(vs, sizeof(vs), "%d", (int)round(data[i]));

    // Bar
    char bar[100];
    snprintf(bar, sizeof(bar),
             "<rect x='%d' y='%d' width='%d' height='%d' fill='%s' rx='1'/>",
             bx, by, bw, bh,
             hi_bar ? "#3b82f6" : "#bfdbfe");
    server.sendContent(bar);

    // Value label
    if (dense)
    {
      // Rotated −90°: text-anchor=start, rotation point at bar top → text reads bottom→top
      int ty = max(TOP + 14, by - 2);
      char lbl[200];
      snprintf(lbl, sizeof(lbl),
               "<text x='%d' y='%d' font-size='%d' fill='%s'"
               " font-weight='%s' text-anchor='start'"
               " transform='rotate(-90 %d %d)'>%s</text>",
               cx, ty, vfs,
               hi_bar ? "#1e40af" : "#64748b",
               hi_bar ? "700" : "400",
               cx, ty, vs);
      server.sendContent(lbl);
    }
    else
    {
      int lbl_y = max(TOP - 2, by - 4);
      char lbl[150];
      snprintf(lbl, sizeof(lbl),
               "<text x='%d' y='%d' font-size='%d' fill='%s'"
               " font-weight='%s' text-anchor='middle'>%s</text>",
               cx, lbl_y, vfs,
               hi_bar ? "#1e40af" : "#64748b",
               hi_bar ? "700" : "400",
               vs);
      server.sendContent(lbl);
    }
  }

  // X-axis labels (type-specific)
  int xlabel_y = H - 3;
  if (type == 0)
  {
    // Hourly: 0h, 6h, 12h, 18h, plus current hour if different
    const int fp[4] = {0, 6, 12, 18};
    for (int k = 0; k < 4; k++)
    {
      char lbl[4];
      snprintf(lbl, sizeof(lbl), "%d", fp[k]);
      send_xlabel(fp[k], bw, xlabel_y, hi, xfs, lbl);
    }
    if (hi >= 0 && hi != 0 && hi != 6 && hi != 12 && hi != 18)
    {
      char lbl[12];
      snprintf(lbl, sizeof(lbl), "%d", hi);
      send_xlabel(hi, bw, xlabel_y, hi, xfs, lbl);
    }
  }
  else if (type == 1)
  {
    // Weekly: Mo Tu We Th Fr Sa Su
    const char *const wl[7] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
    for (int k = 0; k < 7; k++)
      send_xlabel(k, bw, xlabel_y, hi, xfs, wl[k]);
  }
  else if (type == 2)
  {
    // Monthly: 1, 5, 10, 15, 20, 25, 31
    const int mi[7] = {0, 4, 9, 14, 19, 24, 30};
    const char *const ml[7] = {"1", "5", "10", "15", "20", "25", "31"};
    for (int k = 0; k < 7; k++)
      send_xlabel(mi[k], bw, xlabel_y, hi, xfs, ml[k]);
  }
  else
  {
    // Yearly: J F M A M J J A S O N D
    const char *const yl[12] = {"J", "F", "M", "A", "M", "J", "J", "A", "S", "O", "N", "D"};
    for (int k = 0; k < 12; k++)
      send_xlabel(k, bw, xlabel_y, hi, xfs, yl[k]);
  }

  server.sendContent_P(PSTR("</svg>"));
}

// Inline SVG bar chart — pastel blue palette, streams from flash.
void send_bar_chart()
{
  const float vals[4] = {rain_today_mm, rain_week_mm, rain_month_mm, rain_year_mm};
  const char *cols[4] = {"#3b82f6", "#60a5fa", "#93c5fd", "#bfdbfe"};
  const char *labs[4] = {"Today", "Week", "Month", "Year"};
  const int bx[4] = {20, 105, 190, 275};
  const int cx[4] = {52, 137, 222, 307};
  const int BW = 65, BASE = 142, MAXH = 115;

  float mx = 0;
  for (int i = 0; i < 4; i++)
    if (vals[i] > mx)
      mx = vals[i];

  server.sendContent_P(PSTR(
      "<div class='ca'>"
      "<div class='lb' style='margin-bottom:.6rem'>Precipitation overview</div>"
      "<svg viewBox='0 0 340 180' style='width:100%'>"
      "<rect width='340' height='180' fill='#f0f7ff' rx='10'/>"
      "<line x1='10' y1='142' x2='330' y2='142' stroke='#bfdbfe' stroke-width='1.5'/>"));

  char buf[300];
  for (int i = 0; i < 4; i++)
  {
    int bh = (mx > 0 && vals[i] > 0) ? max(2, (int)round(vals[i] / mx * MAXH)) : 0;
    int by = BASE - bh;
    int ty = max(14, bh > 14 ? by - 3 : by - 16);

    char vs[10];
    if (vals[i] == 0)
      strcpy(vs, "0");
    else if (vals[i] < 10)
      snprintf(vs, sizeof(vs), "%.2f", vals[i]);
    else if (vals[i] < 100)
      snprintf(vs, sizeof(vs), "%.1f", vals[i]);
    else
      snprintf(vs, sizeof(vs), "%.0f", vals[i]);

    snprintf(buf, sizeof(buf),
             "<rect x='%d' y='%d' width='%d' height='%d' fill='%s' rx='4'/>"
             "<text x='%d' y='%d' text-anchor='middle' font-size='12'"
             " fill='#1e3a8a' font-weight='600'>%s</text>"
             "<text x='%d' y='165' text-anchor='middle' font-size='11' fill='#64748b'>%s</text>",
             bx[i], by, BW, bh, cols[i],
             cx[i], ty, vs,
             cx[i], labs[i]);
    server.sendContent(buf);
  }

  server.sendContent_P(PSTR("</svg></div>"));
}

void handle_root()
{
  struct tm ti;
  boolean has_time = get_localtime(ti);
  char time_str[20] = "not synced";
  if (has_time)
    strftime(time_str, sizeof(time_str), "%d.%m.%Y %H:%M", &ti);
  bool rain = raining_now();

  server.sendHeader("Connection", "close");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  // ---- HEAD + CSS (pastel blue) + JS tab switcher ----
  server.sendContent_P(PSTR(
      "<!DOCTYPE html><html><head>"
      "<meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Rain sensor</title><style>"
      "*{box-sizing:border-box;margin:0;padding:0}"
      "body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;"
      "background:#eff6ff;color:#1e293b}"
      "nav{background:#dbeafe;padding:.75rem 1.2rem;display:flex;"
      "align-items:center;justify-content:space-between;"
      "border-bottom:1px solid #bfdbfe}"
      ".logo{font-size:1.05rem;font-weight:700;color:#1e40af}"
      ".logo small{font-weight:400;opacity:.65;margin-left:.35rem;font-size:.75rem}"
      ".nb{padding:.38rem .75rem;border-radius:6px;font-size:.8rem;"
      "font-weight:500;text-decoration:none;color:#fff;margin-left:.4rem}"
      ".nbu{background:#2563eb}.nbr{background:#dc2626}.nbo{background:#d97706}"
      ".ov{display:none;position:fixed;inset:0;background:rgba(0,0,0,.45);"
      "z-index:10;align-items:center;justify-content:center}.ov.sh{display:flex}"
      ".mdl{background:#fff;border-radius:10px;padding:1.4rem;max-width:340px;"
      "margin:1rem;box-shadow:0 10px 30px rgba(0,0,0,.2)}"
      ".mdl h3{font-size:1rem;color:#1e293b;margin-bottom:.6rem}"
      ".mdl p{font-size:.85rem;color:#475569;line-height:1.45;margin-bottom:1.1rem}"
      ".mrow{display:flex;gap:.5rem;justify-content:flex-end}"
      ".mbtn{padding:.45rem .9rem;border-radius:6px;font-size:.82rem;"
      "font-weight:500;border:none;cursor:pointer;font-family:inherit}"
      ".mc{background:#e2e8f0;color:#1e293b}.mok{background:#dc2626;color:#fff}"
      ".tbr{background:#fff;border-bottom:1px solid #dbeafe;"
      "display:flex;padding:0 1rem}"
      ".tn{padding:.65rem 1rem;font-size:.88rem;font-weight:500;"
      "color:#94a3b8;cursor:pointer;border:none;background:none;"
      "border-bottom:2px solid transparent;margin-bottom:-1px}"
      ".ta{color:#2563eb;border-bottom-color:#2563eb;font-weight:600}"
      ".cn{max-width:820px;margin:0 auto;padding:1.2rem}"
      ".tp{display:none}.ac{display:block}"
      ".gr{display:grid;grid-template-columns:1fr 1fr;"
      "gap:1rem;margin-bottom:1rem}"
      ".cd{background:#fff;border-radius:14px;padding:1.1rem 1.3rem;"
      "border:1px solid #e0effe;box-shadow:0 2px 6px rgba(37,99,235,.07)}"
      ".hl{border-top:3px solid #3b82f6}"
      ".lb{font-size:.7rem;color:#64748b;text-transform:uppercase;"
      "letter-spacing:.07em;font-weight:500}"
      ".vl{font-size:1.9rem;font-weight:700;margin-top:.25rem;"
      "color:#1e40af;line-height:1.1}"
      ".un{font-size:.8rem;font-weight:400;color:#93c5fd;margin-left:.1rem}"
      ".ca{background:#fff;border-radius:14px;padding:1.1rem 1.3rem;"
      "border:1px solid #e0effe;"
      "box-shadow:0 2px 6px rgba(37,99,235,.07);margin-bottom:1rem}"
      ".row{display:flex;align-items:center;gap:.6rem;margin-bottom:.3rem}"
      ".dot{width:11px;height:11px;border-radius:50%;flex-shrink:0}"
      ".rn{background:#3b82f6;box-shadow:0 0 8px rgba(59,130,246,.4)}"
      ".dr{background:#cbd5e1}"
      ".st{font-size:.95rem;font-weight:600;color:#1e40af}"
      "table{width:100%;border-collapse:collapse;font-size:.85rem}"
      "td{padding:.4rem 0;border-bottom:1px solid #f0f7ff;color:#475569}"
      "td:last-child{text-align:right;font-weight:600;color:#1e3a8a}"
      "tr:last-child td{border:none}"
      "</style>"
      "<script>"
      "var sw=function(el){"
      "var id=el.dataset.id;localStorage.setItem('t',id);"
      "document.querySelectorAll('.tp').forEach("
      "function(p){p.classList.remove('ac')});"
      "document.getElementById(id).classList.add('ac');"
      "document.querySelectorAll('.tn').forEach("
      "function(n){n.classList.remove('ta')});"
      "el.classList.add('ta')};"
      "window.onload=function(){"
      "var t=localStorage.getItem('t');"
      "if(t){var b=document.querySelector('.tn[data-id='+t+']');"
      "if(b)sw(b)}};"
      "var rr=function(){document.getElementById('rovl').classList.add('sh')};"
      "var rc=function(){document.getElementById('rovl').classList.remove('sh')};"
      "</script></head><body>"));

  // ---- Navigation bar ----
  char nav_buf[360];
  snprintf(nav_buf, sizeof(nav_buf),
           "<nav><span class='logo'>&#127783; Rain Sensor"
           "<small>v%s</small></span><span>"
           "<a class='nb nbu' href='/update'>Update</a>"
           "<a class='nb nbo' href='#' onclick='rr();return false'>Reset úhrnu srážek</a>"
           "<a class='nb nbr' href='/resetwifi'>Reset WiFi</a>"
           "</span></nav>",
           SW_VERSION);
  server.sendContent(nav_buf);

  // ---- Rain reset confirmation dialog ----
  server.sendContent_P(PSTR(
      "<div class='ov' id='rovl'><div class='mdl'>"
      "<h3>Reset úhrnu srážek</h3>"
      "<p>Opravdu vynulovat všechny naměřené srážky? Vynuluje se celkový "
      "úhrn i hodnoty za dnešek, týden, měsíc a rok včetně grafů. "
      "Akci nelze vrátit zpět.</p>"
      "<div class='mrow'>"
      "<button class='mbtn mc' onclick='rc()'>Storno</button>"
      "<button class='mbtn mok' onclick=\"location.href='/resetrain'\">Potvrdit</button>"
      "</div></div></div>"));

  // ---- Tab bar ----
  server.sendContent_P(PSTR(
      "<div class='tbr'>"
      "<button class='tn ta' data-id='ov' onclick='sw(this)'>Overview</button>"
      "<button class='tn' data-id='dv' onclick='sw(this)'>Device</button>"
      "</div>"
      "<div class='cn'>"
      "<div id='ov' class='tp ac'>" // Overview tab — default active
      ));

  // ---- Overview: raining status + rate ----
  char status[300];
  snprintf(status, sizeof(status),
           "<div class='ca'>"
           "<div class='row'><div class='dot %s'></div>"
           "<span class='st'>%s</span></div>"
           "<div style='font-size:2.3rem;font-weight:700;color:#1e40af;line-height:1'>"
           "%.1f<span style='font-size:1.1rem;color:#93c5fd;margin-left:.25rem'>"
           "mm/h</span></div>"
           "</div>",
           rain ? "rn" : "dr",
           rain ? "Raining now" : "Not raining",
           last_rate);
  server.sendContent(status);

  // ---- Overview: 2×2 precipitation cards, each with an inline sparkline ----
  server.sendContent_P(PSTR("<div class='gr'>"));

  char cv[80];

  // Today — hourly bars (24 columns, current hour highlighted)
  server.sendContent_P(PSTR("<div class='cd hl'><div class='lb'>Today</div>"));
  snprintf(cv, sizeof(cv), "<div class='vl'>%.2f<span class='un'>mm</span></div>", rain_today_mm);
  server.sendContent(cv);
  send_mini_chart(rain_hourly, 24, hist_hour, 0);
  server.sendContent_P(PSTR("</div>"));

  // This week — 7 bars Mon–Sun (current weekday highlighted)
  server.sendContent_P(PSTR("<div class='cd'><div class='lb'>This week</div>"));
  snprintf(cv, sizeof(cv), "<div class='vl'>%.2f<span class='un'>mm</span></div>", rain_week_mm);
  server.sendContent(cv);
  send_mini_chart(rain_daily_week, 7, hist_wday, 1);
  server.sendContent_P(PSTR("</div>"));

  // This month — 31 bars by day (current day highlighted)
  server.sendContent_P(PSTR("<div class='cd'><div class='lb'>This month</div>"));
  snprintf(cv, sizeof(cv), "<div class='vl'>%.2f<span class='un'>mm</span></div>", rain_month_mm);
  server.sendContent(cv);
  send_mini_chart(rain_daily_month, 31, hist_mday, 2);
  server.sendContent_P(PSTR("</div>"));

  // This year — 12 bars Jan–Dec (current month highlighted)
  server.sendContent_P(PSTR("<div class='cd'><div class='lb'>This year</div>"));
  snprintf(cv, sizeof(cv), "<div class='vl'>%.2f<span class='un'>mm</span></div>", rain_year_mm);
  server.sendContent(cv);
  send_mini_chart(rain_monthly, 12, hist_mon, 3);
  server.sendContent_P(PSTR("</div>"));

  server.sendContent_P(PSTR("</div>")); // end .gr

  // ---- Overview: bar chart ----
  send_bar_chart();

  // ---- Device tab ----
  server.sendContent_P(PSTR(
      "</div>" // end #ov
      "<div id='dv' class='tp'>"
      "<div class='ca'>"));

  char dev[400];
  snprintf(dev, sizeof(dev),
           "<table>"
           "<tr><td>IP address</td><td>%s</td></tr>"
           "<tr><td>WiFi signal</td><td>%d dBm</td></tr>"
           "<tr><td>MQTT</td><td>%s</td></tr>"
           "<tr><td>Uptime</td><td>%lu s</td></tr>"
           "<tr><td>Free memory</td><td>%u B</td></tr>"
           "<tr><td>Time (NTP)</td><td>%s</td></tr>"
           "</table>",
           WiFi.localIP().toString().c_str(),
           WiFi.RSSI(),
           mqtt.connected() ? "connected" : "disconnected",
           millis() / 1000UL,
           ESP.getFreeHeap(),
           time_str);
  server.sendContent(dev);

  server.sendContent_P(PSTR("</div></div></div></body></html>"));
}

void handle_reset_rain()
{
  // Zero every accumulated rainfall value (used to clear bogus test data).
  noInterrupts();
  tipping_count = 0; // drop any pending unconsumed tips
  interrupts();

  total_rain_mm = 0.0;
  period_rain_mm = 0.0;
  last_rate = 0.0;
  rain_today_mm = 0.0;
  rain_week_mm = 0.0;
  rain_month_mm = 0.0;
  rain_year_mm = 0.0;
  for (int i = 0; i < 24; i++) rain_hourly[i] = 0.0;
  for (int i = 0; i < 7; i++) rain_daily_week[i] = 0.0;
  for (int i = 0; i < 31; i++) rain_daily_month[i] = 0.0;
  for (int i = 0; i < 12; i++) rain_monthly[i] = 0.0;

  save_stats();      // persist the zeroed totals so the reset survives reboot
  save_history();
  publish_state();   // push zeros to Home Assistant right away

  server.sendHeader("Connection", "close");
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "Rain totals reset");
}

void handle_reset_wifi()
{
  save_stats();
  save_history();
  server.send(200, "text/html",
              "<html><body style='font-family:sans-serif;margin:2rem'>"
              "WiFi settings cleared. The device is restarting and will "
              "open the <b>" AP_NAME "</b> setup portal.</body></html>");
  delay(1000);
  wm.resetSettings();
  ESP.restart();
}

// ---- Setup / loop ----

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

  WiFiManagerParameter p_host("host", "MQTT host", config.host, sizeof(config.host) - 1);
  WiFiManagerParameter p_port("port", "MQTT port", config.port, sizeof(config.port) - 1);
  WiFiManagerParameter p_user("user", "MQTT user", config.user, sizeof(config.user) - 1);
  WiFiManagerParameter p_pass("pass", "MQTT password", config.pass, sizeof(config.pass) - 1);
  wm.addParameter(&p_host);
  wm.addParameter(&p_port);
  wm.addParameter(&p_user);
  wm.addParameter(&p_pass);
  wm.setSaveConfigCallback(save_config_callback);
  wm.setConfigPortalTimeout(180);

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

  sync_ntp();
  load_stats();
  load_history();
  check_rollover(); // reset any periods that elapsed while the device was off

  mqtt.setServer(config.host, atoi(config.port));
  mqtt.setBufferSize(MQTT_MAX_TRANSFER_SIZE);
  if (mqtt_reconnect())
    publish_discovery();

  server.on("/", handle_root);
  server.on("/resetwifi", handle_reset_wifi);
  server.on("/resetrain", handle_reset_rain);
  ElegantOTA.begin(&server);
  server.begin();
  Serial.println("Web server started on port 80");

  last_send_rain = millis() - RAIN_ACTIVE_INTERVAL_S * 1000UL;
  last_stats_save = millis();
  last_rollover_chk = millis();
}

void loop()
{
  server.handleClient();

  if (WiFi.status() == WL_CONNECTED)
  {
    mqtt_reconnect();
    mqtt.loop();
  }

  // Accumulate tips into period totals and history buckets.
  unsigned int tips = peek_tips();
  if (tips > 0)
  {
    consume_tips(tips);
    float mm = tips * MM_PER_TIP;
    total_rain_mm += mm;
    period_rain_mm += mm;
    rain_today_mm += mm;
    rain_week_mm += mm;
    rain_month_mm += mm;
    rain_year_mm += mm;
    if (hist_hour >= 0)
      rain_hourly[hist_hour] += mm;
    if (hist_wday >= 0)
      rain_daily_week[hist_wday] += mm;
    if (hist_mday >= 0)
      rain_daily_month[hist_mday] += mm;
    if (hist_mon >= 0)
      rain_monthly[hist_mon] += mm;
  }

  // Check for day/week/month/year rollover once per minute.
  if (millis() - last_rollover_chk > 60000UL)
  {
    last_rollover_chk = millis();
    check_rollover();
  }

  // Persist stats and history periodically to survive power loss.
  if (millis() - last_stats_save > STATS_SAVE_MS)
  {
    last_stats_save = millis();
    save_stats();
    save_history();
  }

  unsigned long interval_ms = raining_now()
                                  ? RAIN_ACTIVE_INTERVAL_S * 1000UL
                                  : RAIN_IDLE_INTERVAL_S * 1000UL;

  if (millis() - last_send_rain > interval_ms)
  {
    unsigned long elapsed_ms = millis() - last_send_rain;
    if (elapsed_ms > 0)
      last_rate = (period_rain_mm / (elapsed_ms / 1000.0)) * 3600.0;
    if (publish_state())
    {
      period_rain_mm = 0.0;
      last_send_rain = millis();
    }
  }
}