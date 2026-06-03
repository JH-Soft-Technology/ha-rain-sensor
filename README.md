# Rain senosor

An project which brings alive rain bucket sensor with Wemos D1 mini to measure 
amount of water precipitation through mqtt to [Home assistant](https://www.home-assistant.io/) using mqtt discovery.

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

![Without shield](https://github.com/JH-Soft-Technology/ha-rain-sensor/blob/master/content/images/WeMos-d1-connect-to-ms-wh-sp-rg-rain-tipping-sensor.png)

## Wiring with shield

![With shield](https://github.com/JH-Soft-Technology/ha-rain-sensor/blob/master/content/images/WeMos-d1-connect-with-shield-to-ms-wh-sp-rg-rain-tipping-sensor.png)

## Recommended input wiring (debounce / noise filtering)

The rain gauge uses a reed (magnetic) switch on a ~2.9 m cable. The firmware
already debounces the signal in software, but on a long cable a single noise
spike can be miscounted as a tip when using interrupts. A simple RC filter on
the input is recommended:

- `10 kΩ` pull-up from D1 to 3V3 (lower impedance than the internal pull-up)
- `100 nF` capacitor from D1 to GND (RC low-pass that smooths noise spikes)
- `1 kΩ` resistor in series with the reed switch (limits the capacitor's
  discharge current and protects the reed contacts)

![Recommended input wiring](https://github.com/JH-Soft-Technology/ha-rain-sensor/blob/master/content/images/reed-switch-debounce-wiring.png)

For a short cable next to the board the internal `INPUT_PULLUP` plus the
software debounce is usually enough.

## Home Assistant and sketch setup

- Need to configure mqtt-broker. Install it as new integration named [MQTT](https://www.home-assistant.io/integrations/mqtt/) and configure it.
- Setup the main.cpp file in this project. Setup WiFi connection and point to the MQTT broker installed in HA. 

- When you turn on the Wemos D1 mini, a new rain sensor device in the MQTT integration will automatically appear.

![HA MATT discovery](https://github.com/JH-Soft-Technology/ha-rain-sensor/blob/master/content/images/MQTT-discovery-device-rain-sensor.png)
  
- Can be used

![HA dashboard](https://github.com/JH-Soft-Technology/ha-rain-sensor/blob/master/content/images/HA-lovelace-panel.png)

[![buy me a coffee](https://www.buymeacoffee.com/assets/img/custom_images/orange_img.png)](https://www.buymeacoffee.com/jhoralek)
