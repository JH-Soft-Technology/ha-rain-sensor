# Rain sensor for Home Assistant

A DIY wireless rain gauge built around a **Wemos D1 mini (ESP8266)** and the
**MS-WH-SP-RG** tipping-bucket sensor. It measures precipitation and reports it
to [Home Assistant](https://www.home-assistant.io/) over MQTT — with automatic
discovery, a built-in web dashboard, and over-the-air firmware updates.

- 🌧 **Accurate tip counting** — hardware interrupt with software debounce
- 📊 **Rich statistics** — total, rate (mm/h), today / week / month / year
- 🏠 **Home Assistant MQTT discovery** — entities appear automatically
- 🌐 **Built-in web dashboard** — live charts, served straight from the device
- 🔧 **Captive-portal setup** — pick WiFi + enter MQTT, no credentials in code
- ⬆️ **OTA updates** — flash new firmware from the browser
- ⏱ **NTP time sync** — correct daily / weekly / monthly / yearly resets

## Demo

You can try the device's web interface without any hardware — it is a static
snapshot of the real dashboard:

▶️ **[Live demo](https://htmlpreview.github.io/?https://github.com/JH-Soft-Technology/ha-rain-sensor/blob/master/demo/index.html)**

![Web dashboard overview](https://raw.githubusercontent.com/JH-Soft-Technology/ha-rain-sensor/master/demo/screenshot-overview.png)

## What's in this repository

| Path | Contents |
|---|---|
| `src/` | Firmware source (`main.cpp`) |
| `demo/` | Static preview of the web dashboard + screenshot |
| `3d_print/` | STL files for the sensor mount |
| `content/` | Wiring schematic and other images |
| `platformio.ini` | PlatformIO build configuration |
| `firmware-x.y.z.bin` | Pre-built firmware for OTA flashing |

## Used hardware

- Rain bucket sensor [MS-WH-SP-RG](https://www.laskakit.cz/ms-wh-sp-rg-srazkomer/)
- [Wemos D1 mini](https://www.laskakit.cz/wemos-d1-mini-esp8266-wifi-modul/) development board (4 MB flash, needed for OTA)

## Wiring

The rain gauge uses a reed (magnetic) switch on a ~2.9 m cable. The firmware
debounces the signal in software, but on a long cable a single noise spike can
be miscounted as a tip when using interrupts. A simple RC filter on the input is
recommended:

- `10 kΩ` pull-up from D1 to 3V3 (lower impedance than the internal pull-up)
- `100 nF` capacitor from D1 to GND (RC low-pass that smooths noise spikes)
- `1 kΩ` resistor in series with the reed switch (limits the capacitor's
  discharge current and protects the reed contacts)

![Recommended input wiring](https://raw.githubusercontent.com/JH-Soft-Technology/ha-rain-sensor/master/content/images/reed-switch-debounce-wiring.png)

For a short cable next to the board the internal `INPUT_PULLUP` plus the
software debounce is usually enough.

## First-time setup (captive portal)

No credentials are stored in the source code. WiFi and MQTT are configured
through a captive portal on first boot:

1. Install the [MQTT](https://www.home-assistant.io/integrations/mqtt/) integration in Home Assistant and configure the broker.
2. Build and flash the firmware via PlatformIO (4 MB board, `littlefs` filesystem), or upload a pre-built `firmware-x.y.z.bin`.
3. On first boot the device opens a WiFi access point named **`RainSensor-Setup`**. Connect to it with a phone or laptop.
4. In the portal, pick your WiFi network, enter its password, and fill in the MQTT host, port, user and password. Save.
5. The device reboots, connects, and a new rain sensor device appears in the MQTT integration automatically.

Settings are stored on the device (LittleFS), so they survive reboots and
firmware updates.

## Web dashboard and OTA updates

Once connected, the device serves a dashboard on its IP address (shown in the
serial log and your router). It has two tabs:

- **Overview** — "raining now" with current rate, plus today / week / month / year totals, each with a bar chart.
- **Device** — IP address, WiFi signal, MQTT status, uptime, free memory and total rain since boot.

Two actions are available from the header:

- **Update** (`/update`) — upload a new firmware `.bin` straight from the browser (ElegantOTA).
- **Reset WiFi** (`/resetwifi`) — clear WiFi settings and reopen the setup portal.

The [live demo](https://htmlpreview.github.io/?https://github.com/JH-Soft-Technology/ha-rain-sensor/blob/master/demo/index.html) shows exactly this interface.

## Entities exposed in Home Assistant

All values are sent in a single JSON message and split into entities via value
templates, so the device only publishes one MQTT message per update.

| Entity | Type | Unit | Notes |
|---|---|---|---|
| Rain | sensor | mm | cumulative, `state_class: total_increasing` |
| Rain rate | sensor | mm/h | `device_class: precipitation_intensity` |
| Rain today | sensor | mm | resets at local midnight |
| Rain this week | sensor | mm | resets Monday |
| Rain this month | sensor | mm | resets on the 1st |
| Rain this year | sensor | mm | resets on Jan 1 |
| Raining | binary_sensor | — | on while it rained in the last 6 min |
| WiFi signal | sensor | dBm | diagnostic |
| Uptime | sensor | s | diagnostic |
| Free memory | sensor | B | diagnostic |

The period totals reset on real calendar boundaries thanks to **NTP time sync**
(CET/CEST). The cumulative rain uses `total_increasing`, so Home Assistant
tracks resets (after a reboot) and can build its own long-term statistics.

The send interval adapts automatically: **every 60 s** while it is raining,
**every 30 min** after 20 minutes without rain.

## Build and development

- [Visual Studio Code](https://code.visualstudio.com/) with the [PlatformIO](https://platformio.org/) extension.
- Open the project, connect the Wemos D1 mini, and run *Upload*. After the first flash, further updates can be done over WiFi from the `/update` page.

## STL files

Pipe holder (50 mm diameter) with an arm to hold the rain sensor. Uses 6 M4
screws with nuts; the sensor is bolted to the arm with a self-tapping screw.

- [Pipe holder part 1](https://github.com/JH-Soft-Technology/ha-rain-sensor/blob/master/3d_print/rain%20sensor%20tube%20holder%20part%201.stl)
- [Pipe holder part 2](https://github.com/JH-Soft-Technology/ha-rain-sensor/blob/master/3d_print/rain%20sensor%20tube%20holder%20part%202.stl)
- [Arm holder](https://github.com/JH-Soft-Technology/ha-rain-sensor/blob/master/3d_print/Rain%20sensor%20arm.stl)

---

[![buy me a coffee](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/jhoralek)
