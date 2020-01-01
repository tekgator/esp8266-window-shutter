#ifndef config_h
#define config_h

#define CLIENT_ID_PREFIX "esp8266"

#define WIFI_SSID "SSID here"
#define WIFI_PASSWORD "WPA key here"

#define MQTT_HOST IPAddress(192, 168, 1, 2)
#define MQTT_PORT 1883
#define MQTT_USER "MQTT user here"
#define MQTT_PWD "MQTT password here"

/* defines whether, after WiFi and MQTT is connected, the onboard LED stays active */
#define LED_ONBOARD_ACTIVE false

#endif