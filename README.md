# Rain sensor

A project which brings alive a rain bucket sensor with Wemos D1 mini to measure
amount of water precipitation through MQTT to [Home Assistant](https://www.home-assistant.io/) using MQTT discovery.

## Used hardware

- Rain bucket sensor [MS-WH-SP-RG](https://pl.banggood.com/Misol-WH-SP-RG-1PC-Spare-Part-For-Weather-Station-For-Rain-Meter-Measure-Rain-Volume-Rain-Gauge-p-1440220.html?imageAb=1&akmClientCountry=CZ&a=1657088088.4075&cur_warehouse=CN&DCC=CZ&currency=USD&akmClientCountry=CZ)
- [Wemos D1 mini](https://www.banggood.com/Geekcreit-D1-Mini-V2_3_0-WIFI-Internet-Of-Things-Development-Board-Based-ESP8266-ESP-12S-4MB-FLASH-p-1214756.html?cur_warehouse=CN&rmmds=search) development board

## Dev platform

- [Visual Studio Code](https://code.visualstudio.com/) - pretty good and customizable tool
- [PlatformIO](https://platformio.org/) - can be extension into the Visual studio code and can substitute traditional Arduino IDE.

## Environment

It is using mqtt protocol to brings data from hardware into the digital world. tailor-made for the [home assistant](https://www.home-assistant.io/) environment.

## STL files

- Pipe holder with 50 mm diameter and arm to hold the rain sensor. Used 6 M4 screws with nuts. Sensor is bolted to the arm with self-tapping screw.

  - [Pipe holder part 1](https://github.com/JH-Soft-Technology/ha-rain-sensor/blob/master/3d_print/rain%20sensor%20tube%20holder%20part%201.stl)
  - [Pipe holder part 2](https://github.com/JH-Soft-Technology/ha-rain-sensor/blob/master/3d_print/rain%20sensor%20tube%20holder%20part%202.stl)
  - [Arm holder](https://github.com/JH-Soft-Technology/ha-rain-sensor/blob/master/3d_print/Rain%20sensor%20arm.stl)

## Wiring

The rain gauge uses a reed (magnetic) switch on a ~2.9 m cable. The firmware
debounces the signal in software, but on a long cable a single noise spike can
be miscounted as a tip when using interrupts. A simple RC filter on the input is
recommended:

- `10 kΩ` pull-up from D1 to 3V3 (lower impedance than the internal pull-up)
- `100 nF` capacitor from D1 to GND (RC low-pass that smooths noise spikes)
- `1 kΩ` resistor in series with the reed switch (limits the capacitor's
  discharge current and protects the reed contacts)

![Recommended input wiring](https://github.com/JH-Soft-Technology/ha-rain-sensor/blob/master/content/images/reed-switch-debounce-wiring.png)

For a short cable next to the board the internal `INPUT_PULLUP` plus the
software debounce is usually enough.

## First-time setup (captive portal)

No credentials are stored in the source code. WiFi and MQTT are configured
through a captive portal on first boot:

1. Install the [MQTT](https://www.home-assistant.io/integrations/mqtt/) integration in Home Assistant and configure the broker.
2. Build and flash the firmware via PlatformIO (4 MB board, `littlefs` filesystem).
3. On first boot the device opens a WiFi access point named **`RainSensor-Setup`**.
   Connect to it with a phone or laptop.
4. In the portal, pick your WiFi network, enter its password, and fill in the
   MQTT host, port, user and password. Save.
5. The device reboots, connects, and a new rain sensor device appears in the
   MQTT integration automatically.

The settings are stored on the device (LittleFS), so they survive reboots and
firmware updates.

## Web interface and OTA updates

Once connected, the device serves a small web UI on its IP address (shown in the
serial log and your router):

- `/` — live status: total rain, rate, raining, WiFi signal, IP, uptime, free memory, MQTT state
- `/update` — upload a new firmware `.bin` straight from the browser (ElegantOTA)
- `/resetwifi` — clear WiFi settings and reopen the setup portal

## Entities exposed in Home Assistant

All values are sent in a single JSON message and split into entities via value templates:

| Entity | Type | Notes |
|---|---|---|
| Rain | sensor (mm) | cumulative, `state_class: total_increasing` |
| Rain rate | sensor (mm/h) | `device_class: precipitation_intensity` |
| Raining | binary_sensor | on while it rained in the last 6 min |
| WiFi signal | sensor (dBm) | diagnostic |
| Uptime | sensor (s) | diagnostic |
| Free memory | sensor (B) | diagnostic |

The cumulative rain uses `total_increasing`, so Home Assistant automatically
tracks resets (after a reboot) and calculates hourly / daily / monthly
statistics. Use a `utility_meter` helper for per-day or per-week resets.

The send interval adapts automatically: **every 60 s** while it is raining,
**every 30 min** after 20 minutes without rain.

[![buy me a coffee](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/jhoralek)
