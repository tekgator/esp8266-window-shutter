# esp8266-window-shutter

An ESP8266 program to connect 2 Becker Window Shutter Motors via their proprietary remote control.

This program is more for private use but if you like, I'm open for suggestions. All I can say: It works for me :)...

## MQTT messages

The table shows the possible MQTT messages. Values with **#** are to be replace with the device name (ESP + Chip ID) or shutter number (1 or 2).

Area | Topic | Payload | Send / Receive | Retained | Note
--- | --- | --- | --- | --- | ---
Global | `ESPs/cmd` | `announce` | Receive | No | Device will announce current status of itself and both shutters 
Device | `ESP#/availability` | `online`<br>`offline` | Send | Yes |Last will topic, to show availability off the device
Shutter | `ESP#/shutter#/state` | `open`<br>`close` | Send | Yes | Status of the shutter
Shutter | `ESP#/shutter#/position` | `0` to `100` | Send | Yes | Position of the shutter
Shutter | `ESP#/shutter#/set` | `down`<br>`stop`<br>`up` | Receive | No | Start down or upwards movement or stop shutter movement.
Shutter | `ESP#/shutter#/set_position` | `0` to `100` | Receive | No | Start down or upwards movement or stop shutter movement.