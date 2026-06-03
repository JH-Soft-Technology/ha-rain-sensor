/*
  Credentials template.

  Copy this file to "secrets.h" (in the same include/ folder) and fill
  in your own values. secrets.h is git-ignored so your credentials are
  never committed.

      cp include/secrets.example.h include/secrets.h
*/
#ifndef SECRETS_H
#define SECRETS_H

// ---- WiFi ----
#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PWD "your-wifi-password"

// ---- MQTT broker ----
#define MQTT_HOST "192.168.1.10" // your MQTT broker host / IP
#define MQTT_PORT 1883           // your MQTT broker port

// Leave user name and password empty ("") if your broker does not
// require authentication.
#define MQTT_USER_NAME "" // your MQTT user name
#define MQTT_PASSWORD ""  // your MQTT password

#endif // SECRETS_H
